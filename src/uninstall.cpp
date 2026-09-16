#include "uninstall.h"
#include "util.h"
#include "instance.h"

#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <vector>

namespace uninstall {
namespace {

const wchar_t* kAppName = L"OMT Mini";
const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OMTMini";
const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool has_arg(const wchar_t* needle) {
    const wchar_t* cmd = GetCommandLineW();
    return cmd && wcsstr(cmd, needle) != nullptr;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        result = path;
        CoTaskMemFree(path);
    }
    return result;
}

void unregister_vcam() {
    const std::wstring dll = util::exe_dir() + L"\\omtmini_vcam.dll";
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) return;
    using RegFn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<RegFn>(reinterpret_cast<void*>(
        GetProcAddress(mod, "DllUnregisterServer")));
    if (fn) fn();
    FreeLibrary(mod);
}

// The program folder cannot delete itself while this executable is running in
// it, so hand the job to a detached shell that waits for us to exit.
void schedule_folder_removal(const std::wstring& dir) {
    if (dir.empty()) return;

    std::wstring command = L"/c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + dir + L"\"";

    wchar_t system_dir[MAX_PATH] = {};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    const std::wstring cmd_exe = std::wstring(system_dir) + L"\\cmd.exe";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = cmd_exe.c_str();
    info.lpParameters = command.c_str();
    info.nShow  = SW_HIDE;
    ShellExecuteExW(&info);
}

} // namespace

bool requested() { return has_arg(L"--uninstall"); }
bool silent()    { return has_arg(L"--silent"); }

void run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // A copy already in the tray has to go first. This process is usually that
    // copy, in which case there is nothing to wait for.
    instance::request_quit_and_wait(15000);

    unregister_vcam();

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kAppName);
        RegCloseKey(key);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

    const std::wstring programs = known_folder(FOLDERID_Programs);
    if (!programs.empty()) DeleteFileW((programs + L"\\OMT Mini.lnk").c_str());
    const std::wstring desktop = known_folder(FOLDERID_Desktop);
    if (!desktop.empty()) DeleteFileW((desktop + L"\\OMT Mini.lnk").c_str());

    const std::wstring dir = util::exe_dir();

    if (!silent()) {
        MessageBoxW(nullptr,
                    L"OMT Mini has been removed.\n\n"
                    L"Settings and logs are kept in %APPDATA%\\OMT Mini so a "
                    L"reinstall picks up where you left off. Delete that folder by "
                    L"hand if you want them gone.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
    }

    schedule_folder_removal(dir);
    CoUninitialize();
}

} // namespace uninstall
