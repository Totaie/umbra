/**
 * @file src/self_update.h
 * @brief Updating this host to its newest release on a paired client's request.
 *
 * The client only ever asks "update yourself". Which release, which file and where
 * it comes from are decided here: the newest non-prerelease of this host's own
 * GitHub repository (SUNSHINE_REPO_OWNER/SUNSHINE_REPO_NAME), its UmbraHostSetup
 * installer, fetched over verified HTTPS into a folder only SYSTEM and
 * administrators can write, and checked against the SHA-256 GitHub publishes for it
 * before it is run. Nothing from the request reaches a path, a URL or a command.
 */
#pragma once

#include <string>

namespace self_update {
  enum class start_result_e {
    started,  ///< Downloading; the installer runs after it is verified.
    current,  ///< Already the newest release.
    busy,  ///< An update is already under way.
    unavailable,  ///< Not on this platform, or no release could be found.
  };

  struct status_t {
    std::string state;  ///< "idle", "downloading", "installing" or "failed"
    std::string version;  ///< The version being installed, if any
    std::string detail;  ///< Why it failed, if it did
  };

  /**
   * @brief Starts updating to the newest release, if this host is behind it.
   * @param version Receives the version being installed, or the installed one if current.
   * @param detail Receives the reason when the result is `unavailable`.
   */
  start_result_e start(std::string &version, std::string &detail);

  status_t status();
}  // namespace self_update
