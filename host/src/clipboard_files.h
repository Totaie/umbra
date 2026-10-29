/**
 * @file src/clipboard_files.h
 * @brief Files on the clipboard, copied between a client and this host.
 *
 * Both directions run over the paired HTTPS connection (see nvhttp.cpp), and this
 * is the part that touches the disk. Nothing here decides *who* may do it; the
 * handlers check the client's certificate, its permissions and that it is
 * streaming before they get this far. What this part makes sure of:
 *
 * - A client only ever reads files that are on the host's clipboard right now,
 *   addressed by their position in a listing the host made, never by a path the
 *   client sent. Links inside copied folders are left out rather than followed,
 *   and a file that has been swapped for another since it was listed is refused.
 * - A client only ever writes into a fresh folder of its own under the staging
 *   root, with names that cannot climb out of it, alias another entry, or mean
 *   something special to Windows, and never more than it said it would send.
 * - Every file is created new, never opened through a link, never overwritten.
 *
 * On Windows the handlers run all of this while impersonating the signed-in user,
 * so it can do no more on disk than that user could.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace clipboard_files {
  /// Bytes per request either way. The HTTPS server refuses requests over 1 MiB.
  inline constexpr std::size_t chunk_bytes = 768 * 1024;
  inline constexpr std::size_t max_entries = 10000;
  inline constexpr std::size_t max_depth = 32;
  inline constexpr std::size_t max_name_bytes = 255;
  inline constexpr std::size_t max_path_bytes = 1024;
  inline constexpr std::uint64_t max_total_bytes = 64ull << 30;
  inline constexpr std::size_t max_uploads_per_client = 2;
  /// Space left free on the disk after an upload, whatever its size.
  inline constexpr std::uint64_t keep_free_bytes = 1ull << 30;
  inline constexpr std::chrono::minutes upload_idle_limit {10};
  /// How long pasted files stay in the staging folder for the paste to copy them out.
  inline constexpr std::chrono::hours keep_staged {24};

  /// One file or folder, by its path relative to the copy ('/' separated, UTF-8).
  struct entry_t {
    std::string path;
    std::uint64_t size = 0;
    bool directory = false;
  };

  /// Whether a single file or folder name is safe on both Windows and POSIX.
  bool valid_name(std::string_view name, std::string *why = nullptr);

  /// Whether a relative path is made of valid names and stays within limits.
  bool valid_relative_path(std::string_view path, std::string *why = nullptr);

  /**
   * @brief Whether a listing can be written out exactly as it says.
   * @details Every path valid, no two the same (ignoring ASCII case, as Windows
   *          does), every entry's parent listed before it as a folder, folders of
   *          size zero, and within the count and size limits.
   */
  bool valid_entries(const std::vector<entry_t> &entries, std::string *why = nullptr);

  /// The path for a UTF-8 string, without the local code page getting a say.
  std::filesystem::path path_from_utf8(std::string_view utf8);
  std::string path_to_utf8(const std::filesystem::path &path);

  /// Identifies a file regardless of the name it is reached by.
  struct file_id_t {
    std::uint64_t volume = 0;
    std::uint64_t index = 0;

    bool operator==(const file_id_t &) const = default;
  };

  /// What the host's clipboard held when the client asked.
  struct snapshot_t {
    std::uint32_t sequence = 0;
    std::vector<entry_t> entries;
    std::vector<std::filesystem::path> sources;  ///< Where each entry is on disk.
    std::vector<file_id_t> ids;  ///< Which file each entry was when listed.
    bool truncated = false;  ///< Over the limits; such a listing is not offered at all.
  };

  /**
   * @brief Lists the clipboard's files and everything in its folders.
   * @details Links and reparse points inside folders are skipped, as is anything
   *          that is not a plain file or folder. A link that was itself copied is
   *          resolved once, to what it points at. Two copied items with the same
   *          name keep the first.
   */
  snapshot_t make_snapshot(const std::vector<std::filesystem::path> &tops, std::uint32_t sequence);

  /**
   * @brief Reads up to chunk_bytes of a listed file.
   * @return False if the index or offset is out of range, or the file at that path
   *         is no longer the one that was listed.
   */
  bool read_chunk(const snapshot_t &snapshot, std::size_t index, std::uint64_t offset, std::string &out, std::string *why = nullptr);

  /// Removes a folder and its contents without following any link inside it.
  bool remove_tree(const std::filesystem::path &path);

  /// Uploads from clients, from the first request to the paste.
  class uploads_t {
  public:
    struct begin_result_t {
      std::string id;  ///< Empty on failure.
      std::string error;
    };

    /**
     * @brief Starts an upload into a new folder under @p root.
     * @param client The certificate identity of the client; later calls must match.
     * @param free_bytes Space available under @p root.
     */
    begin_result_t begin(const std::string &client, const std::vector<entry_t> &entries, const std::filesystem::path &root, std::uint64_t free_bytes);

    /// Appends @p data to file @p index, which must so far hold exactly @p offset bytes.
    bool write(const std::string &client, const std::string &id, std::size_t index, std::uint64_t offset, std::string_view data, std::string *why = nullptr);

    /**
     * @brief Finishes an upload whose files are all complete.
     * @param tops Receives the top-level items, which are what goes on the clipboard.
     * @param files Receives every file, for marking where they came from.
     */
    bool commit(const std::string &client, const std::string &id, std::vector<std::filesystem::path> &tops, std::vector<std::filesystem::path> &files, std::string *why = nullptr);

    void abort(const std::string &client, const std::string &id);

    /// Drops idle uploads, and staged folders under @p root older than keep_staged.
    void sweep(const std::filesystem::path &root);

    std::size_t active() const;

  private:
    struct upload_t {
      std::string client;
      std::filesystem::path folder;
      std::vector<entry_t> entries;
      std::vector<std::uint64_t> written;
      std::chrono::steady_clock::time_point touched;
    };

    mutable std::mutex mutex_;
    std::map<std::string, upload_t> uploads_;
  };
}  // namespace clipboard_files
