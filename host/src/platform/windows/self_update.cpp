/**
 * @file src/platform/windows/self_update.cpp
 * @brief Running the host's own installer, for src/self_update.h.
 */
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <string>

#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  bool self_update_supported() {
    return true;
  }

  bool launch_detached_installer(const std::filesystem::path &installer, const std::string &arguments) {
    // The installer stops the service, which closes the job this process runs in, and
    // with it everything still inside. It has to break away from that job or it would
    // be killed half way through replacing the host. The service allows breakaway; if
    // that ever changes this fails here instead of leaving a half-installed host.
    std::wstring command = L"\"" + installer.native() + L"\"";
    if (!arguments.empty()) {
      command += L" " + std::wstring(arguments.begin(), arguments.end());
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    const DWORD flags = CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    const std::wstring directory = installer.parent_path().native();
    if (!CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(), &startup, &process)) {
      BOOST_LOG(error) << "Self-update: could not start the installer ("sv << GetLastError() << ')';
      return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
  }
}  // namespace platf
