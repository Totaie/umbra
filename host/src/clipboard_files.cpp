/**
 * @file src/clipboard_files.cpp
 * @brief Files on the clipboard, copied between a client and this host.
 */
#include "clipboard_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <set>

#include <openssl/rand.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace clipboard_files {
  namespace {
    void say(std::string *why, std::string text) {
      if (why) {
        *why = std::move(text);
      }
    }

    bool valid_utf8(std::string_view s) {
      for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n;
        std::uint32_t cp;
        if (c < 0x80) {
          ++i;
          continue;
        } else if ((c & 0xE0) == 0xC0) {
          n = 1;
          cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
          n = 2;
          cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
          n = 3;
          cp = c & 0x07;
        } else {
          return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
          if (i + k >= s.size()) {
            return false;
          }
          const auto cc = static_cast<unsigned char>(s[i + k]);
          if ((cc & 0xC0) != 0x80) {
            return false;
          }
          cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and values past Unicode are all ways to smuggle
        // a second spelling of a name past a comparison.
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
          return false;
        }
        i += n + 1;
      }
      return true;
    }

    std::string ascii_lower(std::string_view s) {
      std::string out(s);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }

    bool reserved_on_windows(std::string_view name) {
      // The device names are reserved with any extension, and with trailing spaces
      // before it: "nul.txt" and "con .log" are the console and the null device.
      auto base = ascii_lower(name.substr(0, name.find('.')));
      while (!base.empty() && base.back() == ' ') {
        base.pop_back();
      }
      static const std::array<std::string_view, 6> words {"con"sv, "prn"sv, "aux"sv, "nul"sv, "conin$"sv, "conout$"sv};
      if (std::find(words.begin(), words.end(), base) != words.end()) {
        return true;
      }
      if (base.size() >= 4 && (base.starts_with("com"sv) || base.starts_with("lpt"sv))) {
        const auto rest = std::string_view(base).substr(3);
        // COM0-9 and LPT0-9, and the superscript digits Windows also treats as devices.
        if (rest.size() == 1 && rest[0] >= '0' && rest[0] <= '9') {
          return true;
        }
        if (rest == "\xC2\xB9"sv || rest == "\xC2\xB2"sv || rest == "\xC2\xB3"sv) {
          return true;
        }
      }
      return false;
    }

    std::string random_id() {
      std::array<unsigned char, 16> bytes {};
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};
      }
      static constexpr char hex[] = "0123456789abcdef";
      std::string out;
      for (auto b : bytes) {
        out += hex[b >> 4];
        out += hex[b & 0xF];
      }
      return out;
    }

    bool looks_like_id(std::string_view s) {
      return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             });
    }

    // ---- the file system, without following links ----

#ifdef _WIN32
    struct handle_t {
      HANDLE h = INVALID_HANDLE_VALUE;

      ~handle_t() {
        if (h != INVALID_HANDLE_VALUE) {
          CloseHandle(h);
        }
      }

      explicit operator bool() const {
        return h != INVALID_HANDLE_VALUE;
      }
    };

    bool id_of(HANDLE h, file_id_t &id, bool &plain_file) {
      BY_HANDLE_FILE_INFORMATION info {};
      if (!GetFileInformationByHandle(h, &info)) {
        return false;
      }
      id.volume = info.dwVolumeSerialNumber;
      id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
      plain_file = !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE));
      return true;
    }

    bool open_read(const std::filesystem::path &p, handle_t &f) {
      f.h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      return static_cast<bool>(f);
    }

    bool make_folder(const std::filesystem::path &p) {
      return CreateDirectoryW(p.c_str(), nullptr) != 0;
    }

    bool append_to(const std::filesystem::path &p, bool create, std::uint64_t offset, std::string_view data, std::string *why) {
      handle_t f;
      f.h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (!f) {
        say(why, "could not open the file for writing ("s + std::to_string(GetLastError()) + ")");
        return false;
      }
      file_id_t id;
      bool plain = false;
      LARGE_INTEGER size {};
      if (!id_of(f.h, id, plain) || !plain || !GetFileSizeEx(f.h, &size) || static_cast<std::uint64_t>(size.QuadPart) != offset) {
        say(why, "the file on disk is not the one being written");
        return false;
      }
      LARGE_INTEGER end {};
      if (!SetFilePointerEx(f.h, end, nullptr, FILE_END)) {
        say(why, "could not seek");
        return false;
      }
      std::size_t done = 0;
      while (done < data.size()) {
        DWORD wrote = 0;
        const auto want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 20));
        if (!WriteFile(f.h, data.data() + done, want, &wrote, nullptr) || wrote == 0) {
          say(why, "could not write ("s + std::to_string(GetLastError()) + ")");
          return false;
        }
        done += wrote;
      }
      return true;
    }
#else
    struct handle_t {
      int fd = -1;

      ~handle_t() {
        if (fd >= 0) {
          close(fd);
        }
      }

      explicit operator bool() const {
        return fd >= 0;
      }
    };

    bool id_of(int fd, file_id_t &id, bool &plain_file) {
      struct stat st {};
      if (fstat(fd, &st) != 0) {
        return false;
      }
      id.volume = static_cast<std::uint64_t>(st.st_dev);
      id.index = static_cast<std::uint64_t>(st.st_ino);
      plain_file = S_ISREG(st.st_mode);
      return true;
    }

    bool open_read(const std::filesystem::path &p, handle_t &f) {
      f.fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      return static_cast<bool>(f);
    }

    bool make_folder(const std::filesystem::path &p) {
      return mkdir(p.c_str(), 0700) == 0;
    }

    bool append_to(const std::filesystem::path &p, bool create, std::uint64_t offset, std::string_view data, std::string *why) {
      handle_t f;
      f.fd = create ? open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : open(p.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
      if (!f) {
        say(why, "could not open the file for writing");
        return false;
      }
      file_id_t id;
      bool plain = false;
      struct stat st {};
      if (!id_of(f.fd, id, plain) || !plain || fstat(f.fd, &st) != 0 || static_cast<std::uint64_t>(st.st_size) != offset) {
        say(why, "the file on disk is not the one being written");
        return false;
      }
      std::size_t done = 0;
      while (done < data.size()) {
        const auto wrote = ::write(f.fd, data.data() + done, data.size() - done);
        if (wrote <= 0) {
          say(why, "could not write");
          return false;
        }
        done += static_cast<std::size_t>(wrote);
      }
      return true;
    }
#endif

    /// A folder's entries, links and specials marked, without following anything.
    struct dir_item_t {
      std::filesystem::path path;
      std::string name;
      bool directory = false;
      bool plain_file = false;  ///< False for links, reparse points, devices, sockets.
      std::uint64_t size = 0;
    };

    bool list_folder(const std::filesystem::path &folder, std::vector<dir_item_t> &out) {
#ifdef _WIN32
      WIN32_FIND_DATAW data {};
      const auto pattern = folder / L"*";
      HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (find == INVALID_HANDLE_VALUE) {
        return false;
      }
      do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
          continue;
        }
        dir_item_t item;
        item.path = folder / name;
        item.name = path_to_utf8(std::filesystem::path(name));
        const bool reparse = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
        item.directory = !reparse && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        item.plain_file = !reparse && !(data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE));
        item.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        out.push_back(std::move(item));
      } while (FindNextFileW(find, &data));
      FindClose(find);
      return true;
#else
      DIR *dir = opendir(folder.c_str());
      if (!dir) {
        return false;
      }
      while (auto *ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") {
          continue;
        }
        dir_item_t item;
        item.path = folder / name;
        item.name = name;
        struct stat st {};
        if (lstat(item.path.c_str(), &st) != 0) {
          continue;
        }
        item.directory = S_ISDIR(st.st_mode);
        item.plain_file = S_ISREG(st.st_mode);
        item.size = static_cast<std::uint64_t>(st.st_size);
        out.push_back(std::move(item));
      }
      closedir(dir);
      return true;
#endif
    }

    /// The identity of a plain file at @p p, reached without following a link.
    bool plain_file_id(const std::filesystem::path &p, file_id_t &id, std::uint64_t *size = nullptr) {
      handle_t f;
      if (!open_read(p, f)) {
        return false;
      }
      bool plain = false;
#ifdef _WIN32
      if (!id_of(f.h, id, plain) || !plain) {
        return false;
      }
      if (size) {
        LARGE_INTEGER s {};
        if (!GetFileSizeEx(f.h, &s)) {
          return false;
        }
        *size = static_cast<std::uint64_t>(s.QuadPart);
      }
#else
      if (!id_of(f.fd, id, plain) || !plain) {
        return false;
      }
      if (size) {
        struct stat st {};
        if (fstat(f.fd, &st) != 0) {
          return false;
        }
        *size = static_cast<std::uint64_t>(st.st_size);
      }
#endif
      return true;
    }

    bool is_plain_folder(const std::filesystem::path &p) {
#ifdef _WIN32
      const DWORD attrs = GetFileAttributesW(p.c_str());
      return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
      struct stat st {};
      return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }
  }  // namespace

  std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
  }

  std::string path_to_utf8(const std::filesystem::path &path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
  }

  bool valid_name(std::string_view name, std::string *why) {
    if (name.empty() || name.size() > max_name_bytes) {
      say(why, "a name is empty or too long");
      return false;
    }
    if (!valid_utf8(name)) {
      say(why, "a name is not valid UTF-8");
      return false;
    }
    if (name == "."sv || name == ".."sv) {
      say(why, "a name is . or ..");
      return false;
    }
    for (unsigned char c : name) {
      if (c < 0x20 || c == 0x7F || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos) {
        say(why, "a name has a character Windows or POSIX does not allow in it");
        return false;
      }
    }
    if (name.back() == ' ' || name.back() == '.') {
      // Windows drops these, so "a." would land on "a".
      say(why, "a name ends in a space or a dot");
      return false;
    }
    if (reserved_on_windows(name)) {
      say(why, "a name is a Windows device name");
      return false;
    }
    return true;
  }

  bool valid_relative_path(std::string_view path, std::string *why) {
    if (path.empty() || path.size() > max_path_bytes) {
      say(why, "a path is empty or too long");
      return false;
    }
    std::size_t depth = 0;
    std::size_t start = 0;
    while (true) {
      const auto slash = path.find('/', start);
      const auto name = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
      if (!valid_name(name, why)) {
        return false;
      }
      if (++depth > max_depth) {
        say(why, "a path is nested too deeply");
        return false;
      }
      if (slash == std::string_view::npos) {
        return true;
      }
      start = slash + 1;
    }
  }

  bool valid_entries(const std::vector<entry_t> &entries, std::string *why) {
    if (entries.empty() || entries.size() > max_entries) {
      say(why, "too many files, or none");
      return false;
    }
    std::set<std::string> seen;
    std::set<std::string> folders;
    std::uint64_t total = 0;
    for (const auto &e : entries) {
      if (!valid_relative_path(e.path, why)) {
        return false;
      }
      const auto key = ascii_lower(e.path);
      if (!seen.insert(key).second) {
        say(why, "two entries have the same name");
        return false;
      }
      const auto slash = key.rfind('/');
      if (slash != std::string::npos && !folders.contains(key.substr(0, slash))) {
        say(why, "an entry comes before the folder it is in");
        return false;
      }
      if (e.directory) {
        if (e.size != 0) {
          say(why, "a folder has a size");
          return false;
        }
        folders.insert(key);
      }
      if (e.size > max_total_bytes || total + e.size > max_total_bytes) {
        say(why, "the files are too large in total");
        return false;
      }
      total += e.size;
    }
    return true;
  }

  snapshot_t make_snapshot(const std::vector<std::filesystem::path> &tops, std::uint32_t sequence) {
    snapshot_t snap;
    snap.sequence = sequence;
    std::set<std::string> top_names;
    std::uint64_t total = 0;

    auto add = [&](entry_t e, std::filesystem::path source, file_id_t id) {
      if (snap.entries.size() >= max_entries || total + e.size > max_total_bytes) {
        snap.truncated = true;
        return false;
      }
      total += e.size;
      snap.entries.push_back(std::move(e));
      snap.sources.push_back(std::move(source));
      snap.ids.push_back(id);
      return true;
    };

    // Depth-first, folders before their contents, as valid_entries() wants.
    std::function<bool(const std::filesystem::path &, const std::string &, std::size_t)> walk =
      [&](const std::filesystem::path &folder, const std::string &rel, std::size_t depth) {
        if (depth >= max_depth) {
          snap.truncated = true;
          return false;
        }
        std::vector<dir_item_t> items;
        if (!list_folder(folder, items)) {
          return true;  // Unreadable: listed empty, as Explorer would copy it.
        }
        std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
          return a.name < b.name;
        });
        for (auto &item : items) {
          const auto child = rel + "/" + item.name;
          if (!valid_relative_path(child)) {
            continue;
          }
          if (item.directory) {
            if (!add({child, 0, true}, {}, {}) || !walk(item.path, child, depth + 1)) {
              return false;
            }
          } else if (item.plain_file) {
            file_id_t id;
            std::uint64_t size = 0;
            if (!plain_file_id(item.path, id, &size)) {
              continue;
            }
            if (!add({child, size, false}, item.path, id)) {
              return false;
            }
          }
          // Links, junctions, devices: left out.
        }
        return true;
      };

    for (const auto &top : tops) {
      std::error_code ec;
      // A link that was copied itself is followed this once, to what it names.
      const auto real = std::filesystem::canonical(top, ec);
      if (ec) {
        continue;
      }
      const auto name = path_to_utf8(real.filename());
      if (!valid_name(name) || !top_names.insert(ascii_lower(name)).second) {
        continue;
      }
      if (is_plain_folder(real)) {
        if (!add({name, 0, true}, {}, {}) || !walk(real, name, 1)) {
          break;
        }
      } else {
        file_id_t id;
        std::uint64_t size = 0;
        if (!plain_file_id(real, id, &size)) {
          continue;
        }
        if (!add({name, size, false}, real, id)) {
          break;
        }
      }
    }
    return snap;
  }

  bool read_chunk(const snapshot_t &snapshot, std::size_t index, std::uint64_t offset, std::string &out, std::string *why) {
    out.clear();
    if (index >= snapshot.entries.size() || snapshot.entries[index].directory) {
      say(why, "no such file in the listing");
      return false;
    }
    handle_t f;
    if (!open_read(snapshot.sources[index], f)) {
      say(why, "the file cannot be opened any more");
      return false;
    }
    file_id_t id;
    bool plain = false;
#ifdef _WIN32
    LARGE_INTEGER size {};
    if (!id_of(f.h, id, plain) || !plain || !(id == snapshot.ids[index]) || !GetFileSizeEx(f.h, &size)) {
      say(why, "the file is not the one that was listed");
      return false;
    }
    const auto file_size = static_cast<std::uint64_t>(size.QuadPart);
#else
    struct stat st {};
    if (!id_of(f.fd, id, plain) || !plain || !(id == snapshot.ids[index]) || fstat(f.fd, &st) != 0) {
      say(why, "the file is not the one that was listed");
      return false;
    }
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
#endif
    if (offset > file_size) {
      say(why, "the offset is past the end of the file");
      return false;
    }
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_bytes, file_size - offset));
    out.resize(want);
    std::size_t done = 0;
#ifdef _WIN32
    LARGE_INTEGER pos {};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(f.h, pos, nullptr, FILE_BEGIN)) {
      say(why, "could not seek");
      return false;
    }
    while (done < want) {
      DWORD got = 0;
      if (!ReadFile(f.h, out.data() + done, static_cast<DWORD>(want - done), &got, nullptr) || got == 0) {
        break;
      }
      done += got;
    }
#else
    while (done < want) {
      const auto got = pread(f.fd, out.data() + done, want - done, static_cast<off_t>(offset + done));
      if (got <= 0) {
        break;
      }
      done += static_cast<std::size_t>(got);
    }
#endif
    out.resize(done);
    return true;
  }

  bool remove_tree(const std::filesystem::path &path) {
    std::vector<dir_item_t> items;
    if (list_folder(path, items)) {
      for (const auto &item : items) {
        if (item.directory) {
          remove_tree(item.path);
        } else {
          // A link is removed as itself, never what it points at.
#ifdef _WIN32
          if (!DeleteFileW(item.path.c_str())) {
            RemoveDirectoryW(item.path.c_str());  // a junction or directory link
          }
#else
          unlink(item.path.c_str());
#endif
        }
      }
    }
#ifdef _WIN32
    return RemoveDirectoryW(path.c_str()) != 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
  }

  uploads_t::begin_result_t uploads_t::begin(const std::string &client, const std::vector<entry_t> &entries, const std::filesystem::path &root, std::uint64_t free_bytes) {
    begin_result_t result;
    std::string why;
    if (!valid_entries(entries, &why)) {
      result.error = why;
      return result;
    }
    std::uint64_t total = 0;
    for (const auto &e : entries) {
      total += e.size;
    }
    if (total + keep_free_bytes > free_bytes) {
      result.error = "not enough free space on the host";
      return result;
    }

    std::lock_guard lock(mutex_);
    std::size_t mine = 0;
    for (const auto &[id, upload] : uploads_) {
      mine += upload.client == client ? 1 : 0;
    }
    if (mine >= max_uploads_per_client) {
      result.error = "too many uploads at once";
      return result;
    }

    const auto id = random_id();
    if (id.empty()) {
      result.error = "no randomness for a folder name";
      return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto folder = root / id;
    if (!make_folder(folder)) {
      result.error = "could not create the staging folder";
      return result;
    }
    for (const auto &e : entries) {
      // Folders now, and empty files too, since no chunk will ever create them.
      const auto path = folder / path_from_utf8(e.path);
      const bool made = e.directory ? make_folder(path) : (e.size != 0 || append_to(path, true, 0, {}, nullptr));
      if (!made) {
        remove_tree(folder);
        result.error = "could not create a folder or an empty file";
        return result;
      }
    }

    upload_t upload;
    upload.client = client;
    upload.folder = folder;
    upload.entries = entries;
    upload.written.assign(entries.size(), 0);
    upload.touched = std::chrono::steady_clock::now();
    uploads_.emplace(id, std::move(upload));
    result.id = id;
    return result;
  }

  bool uploads_t::write(const std::string &client, const std::string &id, std::size_t index, std::uint64_t offset, std::string_view data, std::string *why) {
    std::lock_guard lock(mutex_);
    const auto it = uploads_.find(id);
    if (it == uploads_.end() || it->second.client != client) {
      say(why, "no such upload");
      return false;
    }
    auto &upload = it->second;
    if (index >= upload.entries.size() || upload.entries[index].directory) {
      say(why, "no such file in the upload");
      return false;
    }
    const auto &entry = upload.entries[index];
    if (offset != upload.written[index] || data.size() > chunk_bytes || offset + data.size() > entry.size) {
      say(why, "the data does not continue the file where it left off, or runs past its size");
      return false;
    }
    if (!append_to(upload.folder / path_from_utf8(entry.path), offset == 0, offset, data, why)) {
      return false;
    }
    upload.written[index] += data.size();
    upload.touched = std::chrono::steady_clock::now();
    return true;
  }

  bool uploads_t::commit(const std::string &client, const std::string &id, std::vector<std::filesystem::path> &tops, std::vector<std::filesystem::path> &files, std::string *why) {
    std::lock_guard lock(mutex_);
    const auto it = uploads_.find(id);
    if (it == uploads_.end() || it->second.client != client) {
      say(why, "no such upload");
      return false;
    }
    auto &upload = it->second;
    tops.clear();
    files.clear();
    for (std::size_t i = 0; i < upload.entries.size(); ++i) {
      const auto &e = upload.entries[i];
      const auto path = upload.folder / path_from_utf8(e.path);
      if (!e.directory) {
        if (upload.written[i] != e.size) {
          say(why, "a file is not complete");
          return false;
        }
        files.push_back(path);
      }
      if (e.path.find('/') == std::string::npos) {
        tops.push_back(path);
      }
    }
    // Done with it; the folder stays for the paste to copy from until sweep() expires it.
    uploads_.erase(it);
    return true;
  }

  void uploads_t::abort(const std::string &client, const std::string &id) {
    std::lock_guard lock(mutex_);
    const auto it = uploads_.find(id);
    if (it == uploads_.end() || it->second.client != client) {
      return;
    }
    remove_tree(it->second.folder);
    uploads_.erase(it);
  }

  void uploads_t::sweep(const std::filesystem::path &root) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::set<std::string> live;
    for (auto it = uploads_.begin(); it != uploads_.end();) {
      if (now - it->second.touched > upload_idle_limit) {
        remove_tree(it->second.folder);
        it = uploads_.erase(it);
      } else {
        live.insert(it->first);
        ++it;
      }
    }

    // Only folders this code named, so a stray link or file in the root is left alone.
    std::vector<dir_item_t> items;
    if (!list_folder(root, items)) {
      return;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now() - keep_staged;
    for (const auto &item : items) {
      if (!item.directory || !looks_like_id(item.name) || live.contains(item.name)) {
        continue;
      }
      std::error_code ec;
      const auto when = std::filesystem::last_write_time(item.path, ec);
      if (!ec && when < cutoff) {
        remove_tree(item.path);
      }
    }
  }

  std::size_t uploads_t::active() const {
    std::lock_guard lock(mutex_);
    return uploads_.size();
  }
}  // namespace clipboard_files
