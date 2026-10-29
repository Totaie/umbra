/**
 * @file src/self_update.cpp
 * @brief Updating this host to its newest release on a paired client's request.
 */
#include "self_update.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "config.h"
#include "httpcommon.h"
#include "logging.h"
#include "platform/common.h"
#include "state_storage.h"
#include "update.h"
#include "utility.h"
#include "version_compare.h"

using namespace std::literals;

namespace self_update {
  namespace {
    // Generous for an installer, and a ceiling on what a bad release could make us write.
    constexpr curl_off_t max_installer_bytes = 512ll * 1024 * 1024;

    std::mutex status_mutex;
    status_t current_status {"idle", "", ""};
    std::atomic<bool> in_progress {false};

    void set_status(const std::string &state, const std::string &version, const std::string &detail = {}) {
      std::lock_guard lock(status_mutex);
      current_status = {state, version, detail};
    }

    struct release_t {
      std::string version;
      std::string asset_name;
      std::string download_url;
      std::string sha256;
    };

    /// The newest stable release newer than this build that has an installer with a checksum.
    std::optional<release_t> newest_release(std::string &detail, bool &is_current) {
      is_current = false;
      std::string json;
      if (!update::download_github_release_data(SUNSHINE_REPO_OWNER, SUNSHINE_REPO_NAME, json)) {
        detail = "could not reach GitHub to look for a release";
        return std::nullopt;
      }

      std::optional<release_t> best;
      try {
        for (const auto &rel : nlohmann::json::parse(json)) {
          if (rel.value("draft", false) || rel.value("prerelease", false)) {
            continue;
          }
          const auto tag = rel.value("tag_name", ""s);
          if (tag.empty() || (best && version_compare::compare_semver(best->version, tag) >= 0)) {
            continue;
          }
          for (const auto &asset : rel.value("assets", nlohmann::json::array())) {
            const auto name = asset.value("name", ""s);
            const auto digest = asset.value("digest", ""s);
            if (name.starts_with("UmbraHostSetup-x64-") && name.ends_with(".exe") &&
                digest.starts_with("sha256:") && digest.size() == 7 + 64) {
              best = release_t {tag, name, asset.value("browser_download_url", ""s), digest.substr(7)};
              break;
            }
          }
        }
      } catch (const std::exception &e) {
        detail = "GitHub's release list could not be read"s;
        return std::nullopt;
      }

      if (!best) {
        detail = "no release has an installer this host can verify";
        return std::nullopt;
      }
      if (version_compare::compare_semver(PROJECT_VERSION, best->version) >= 0) {
        is_current = true;
        return std::nullopt;
      }
      return best;
    }

    size_t write_file(void *data, size_t size, size_t count, void *user) {
      auto *out = static_cast<std::ofstream *>(user);
      out->write(static_cast<const char *>(data), static_cast<std::streamsize>(size * count));
      return out->good() ? size * count : 0;
    }

    bool download(const release_t &release, const std::filesystem::path &target, std::string &detail) {
      // Only GitHub and its release storage, only over HTTPS, redirects included.
      if (!release.download_url.starts_with("https://github.com/")) {
        detail = "the installer is not hosted on GitHub";
        return false;
      }

      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      if (!out) {
        detail = "could not write the installer to disk";
        return false;
      }

      CURL *curl = curl_easy_init();
      if (!curl) {
        detail = "could not start the download";
        return false;
      }
      curl_easy_setopt(curl, CURLOPT_URL, release.download_url.c_str());
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
      curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, max_installer_bytes);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "UmbraHost-SelfUpdate/1.0");
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
      http::configure_curl_tls(curl);

      const CURLcode res = curl_easy_perform(curl);
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      char *final_url = nullptr;
      curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
      const std::string landed = final_url ? final_url : "";
      curl_easy_cleanup(curl);
      out.close();

      if (res != CURLE_OK || code != 200) {
        detail = "the download failed ("s + curl_easy_strerror(res) + ", HTTP " + std::to_string(code) + ")";
        return false;
      }
      if (!landed.starts_with("https://github.com/") && landed.find(".githubusercontent.com/") == std::string::npos) {
        detail = "the download was redirected somewhere other than GitHub";
        return false;
      }
      return true;
    }

    std::string sha256_of(const std::filesystem::path &file) {
      std::ifstream in(file, std::ios::binary);
      if (!in) {
        return {};
      }
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
      }
      std::vector<char> buffer(1 << 20);
      while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) {
          EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(in.gcount()));
        }
      }
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int length = 0;
      EVP_DigestFinal_ex(ctx, digest, &length);
      EVP_MD_CTX_free(ctx);

      static constexpr char hex[] = "0123456789abcdef";
      std::string out;
      for (unsigned int i = 0; i < length; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0xF];
      }
      return out;
    }

    std::string lower(std::string text) {
      for (auto &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return text;
    }

    void run(release_t release) {
      auto done = util::fail_guard([]() {
        in_progress = false;
      });

      // Beside the config, in a folder locked to SYSTEM and administrators: whatever
      // is in here runs as SYSTEM, so nobody else may put anything in it, or swap the
      // installer between the checksum and the launch.
      const auto folder = std::filesystem::path(config::nvhttp.file_state).parent_path() / "updates";
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      if (!statefile::secure_private_directory(folder.string())) {
        set_status("failed", release.version, "could not make a private folder for the installer");
        return;
      }
      for (const auto &old : std::filesystem::directory_iterator(folder, ec)) {
        std::filesystem::remove(old.path(), ec);
      }

      const auto installer = folder / release.asset_name;
      std::string detail;
      BOOST_LOG(info) << "Self-update: downloading "sv << release.asset_name;
      if (!download(release, installer, detail)) {
        BOOST_LOG(warning) << "Self-update: "sv << detail;
        std::filesystem::remove(installer, ec);
        set_status("failed", release.version, detail);
        return;
      }

      if (lower(sha256_of(installer)) != lower(release.sha256)) {
        BOOST_LOG(warning) << "Self-update: the installer does not match its published checksum; not running it"sv;
        std::filesystem::remove(installer, ec);
        set_status("failed", release.version, "the installer did not match the checksum GitHub published for it");
        return;
      }

      set_status("installing", release.version);
      BOOST_LOG(info) << "Self-update: installing "sv << release.version << "; the host restarts when it is done"sv;
      if (!platf::launch_detached_installer(installer, "/S")) {
        set_status("failed", release.version, "could not start the installer");
      }
      // From here the installer stops this process, replaces it and starts the new one.
    }
  }  // namespace

  start_result_e start(std::string &version, std::string &detail) {
    if (!platf::self_update_supported()) {
      detail = "this host cannot update itself on this platform";
      return start_result_e::unavailable;
    }

    bool expected = false;
    if (!in_progress.compare_exchange_strong(expected, true)) {
      std::lock_guard lock(status_mutex);
      version = current_status.version;
      return start_result_e::busy;
    }

    bool is_current = false;
    auto release = newest_release(detail, is_current);
    if (!release) {
      in_progress = false;
      version = PROJECT_VERSION;
      return is_current ? start_result_e::current : start_result_e::unavailable;
    }

    version = release->version;
    set_status("downloading", release->version);
    std::thread(run, *release).detach();
    return start_result_e::started;
  }

  status_t status() {
    std::lock_guard lock(status_mutex);
    return current_status;
  }
}  // namespace self_update
