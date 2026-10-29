/**
 * @file src/platform/windows/clipboard_files.cpp
 * @brief Files on the Windows clipboard, for src/clipboard_files.h.
 */
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on

#include <chrono>
#include <thread>

#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace {
    /// The clipboard is shared by every process, and another one may hold it for a moment.
    bool open_clipboard() {
      for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(nullptr)) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      BOOST_LOG(warning) << "Clipboard files: the clipboard is busy"sv;
      return false;
    }
  }  // namespace

  bool clipboard_files_supported() {
    return true;
  }

  bool as_desktop_user(const std::function<void()> &fn) {
    // Never as SYSTEM: what a client sends lands where the user could put it, and
    // what it reads is what the user could read.
    HANDLE token = retrieve_users_token(false);
    if (!token) {
      return false;
    }
    const auto ec = impersonate_current_user(token, fn);
    CloseHandle(token);
    return !ec;
  }

  std::optional<std::filesystem::path> clipboard_staging_root() {
    HANDLE token = retrieve_users_token(false);
    if (!token) {
      return std::nullopt;
    }
    PWSTR local = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, token, &local);
    CloseHandle(token);
    if (FAILED(hr) || !local) {
      if (local) {
        CoTaskMemFree(local);
      }
      return std::nullopt;
    }
    std::filesystem::path root = std::filesystem::path(local) / L"UmbraHost" / L"Pasted files";
    CoTaskMemFree(local);
    return root;
  }

  std::uint32_t clipboard_sequence() {
    return GetClipboardSequenceNumber();
  }

  bool get_clipboard_files(std::vector<std::filesystem::path> &paths) {
    paths.clear();
    if (!IsClipboardFormatAvailable(CF_HDROP) || !open_clipboard()) {
      return false;
    }
    if (HANDLE data = GetClipboardData(CF_HDROP)) {
      if (auto drop = static_cast<HDROP>(GlobalLock(data))) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
          const UINT length = DragQueryFileW(drop, i, nullptr, 0);
          if (length == 0 || length > 32767) {
            continue;
          }
          std::wstring path(length + 1, L'\0');
          if (DragQueryFileW(drop, i, path.data(), length + 1) == length) {
            path.resize(length);
            paths.emplace_back(path);
          }
        }
        GlobalUnlock(data);
      }
    }
    CloseClipboard();
    return !paths.empty();
  }

  bool set_clipboard_files(const std::vector<std::filesystem::path> &paths) {
    if (paths.empty()) {
      return false;
    }

    // CF_HDROP: a DROPFILES header, then the paths, each NUL-terminated, then one more NUL.
    std::size_t chars = 1;
    for (const auto &p : paths) {
      chars += p.native().size() + 1;
    }
    const std::size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL drop_memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    HGLOBAL effect_memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    auto release = [&]() {
      if (drop_memory) {
        GlobalFree(drop_memory);
      }
      if (effect_memory) {
        GlobalFree(effect_memory);
      }
    };
    if (!drop_memory || !effect_memory) {
      release();
      return false;
    }

    auto *drop = static_cast<DROPFILES *>(GlobalLock(drop_memory));
    if (!drop) {
      release();
      return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto *out = reinterpret_cast<wchar_t *>(reinterpret_cast<char *>(drop) + sizeof(DROPFILES));
    for (const auto &p : paths) {
      const auto &w = p.native();
      std::copy(w.begin(), w.end(), out);
      out += w.size();
      *out++ = L'\0';
    }
    *out = L'\0';
    GlobalUnlock(drop_memory);

    // A copy, not a move: pasting must leave the staged files where they are.
    if (auto *effect = static_cast<DWORD *>(GlobalLock(effect_memory))) {
      *effect = DROPEFFECT_COPY;
      GlobalUnlock(effect_memory);
    }
    // Spelled out wide: CFSTR_PREFERREDDROPEFFECT follows UNICODE, which this build does not define.
    const UINT effect_format = RegisterClipboardFormatW(L"Preferred DropEffect");

    if (!open_clipboard()) {
      release();
      return false;
    }
    bool ok = EmptyClipboard() && SetClipboardData(CF_HDROP, drop_memory) != nullptr;
    if (ok) {
      drop_memory = nullptr;  // The clipboard owns it now.
      if (effect_format && SetClipboardData(effect_format, effect_memory) != nullptr) {
        effect_memory = nullptr;
      }
    }
    CloseClipboard();
    release();
    if (!ok) {
      BOOST_LOG(warning) << "Clipboard files: could not put the pasted files on the clipboard"sv;
    }
    return ok;
  }

  void mark_from_other_device(const std::filesystem::path &file) {
    // The Mark of the Web: SmartScreen asks before running a program that came this
    // way, and Office opens documents in Protected View, as for an internet download.
    const auto stream = file.native() + L":Zone.Identifier";
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      return;
    }
    static constexpr char zone[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    DWORD wrote = 0;
    WriteFile(h, zone, sizeof(zone) - 1, &wrote, nullptr);
    CloseHandle(h);
  }
}  // namespace platf
