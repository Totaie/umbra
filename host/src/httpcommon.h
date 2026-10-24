/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <string_view>

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"
#include "uuid.h"

namespace http {
  /**
   * @brief Whether a request header or query parameter carries a secret, so that the
   *        request logs show that it was there and never what it was: stream keys
   *        (rikey), pairing proofs and secrets, session cookies and credentials.
   */
  inline bool is_secret_request_field(std::string_view name) {
    static constexpr std::string_view secrets[] {
      "authorization", "cookie", "rikey", "rikeyid", "pskauth", "salt", "clientchallenge",
      "serverchallengeresp", "clientpairingsecret", "clientcert", "otpauth", "otp", "pin", "x-csrf-token", "csrf_token",
    };
    for (const auto secret : secrets) {
      if (name.size() == secret.size() &&
          std::equal(name.begin(), name.end(), secret.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
          })) {
        return true;
      }
    }
    return false;
  }


  enum class creds_state {
    missing_file,
    missing_fields,
    unreadable,
    malformed,
    configured
  };

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  creds_state user_creds_state(const std::string &file);
  int reload_user_creds(const std::string &file);
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  bool configure_curl_tls(CURL *curl);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);
  std::string cookie_escape(const std::string &value);
  std::string cookie_unescape(const std::string &value);

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

#ifdef _WIN32
  extern std::string shared_virtual_display_guid;
#endif

  // Update origin ACL from current config
  void refresh_origin_acl();

}  // namespace http
