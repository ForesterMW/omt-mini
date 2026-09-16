// OMT Mini setup.
//
// Installs per user into %LOCALAPPDATA%\Programs\OMT Mini. Nothing is written
// outside the user's profile, so no administrator prompt appears at any point.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <cstdio>

#include "payload.h"

namespace {

const wchar_t* kAppName     = L"OMT Mini";
const wchar_t* kExeName     = L"OMTMini.exe";
const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OMTMini";
const wchar_t* kRunKey      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum ControlId {
    kIdInstall = 100,
    kIdCancel,
    kIdStartup,
    kIdVcam,
    kIdDesktopShortcut,
    kIdStatus,
    kIdProgress,
    kIdPathLabel,
};

HFONT g_font = nullptr;
HFONT g_title_font = nullptr;
HWND  g_status = nullptr;
HWND  g_progress = nullptr;
bool  g_busy = false;

std::wstring install_dir() {
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = local;
        CoTaskMemFree(local);
    } else {
        wchar_t buf[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, buf);
        dir = buf;
    }
    return dir + L"\\Programs\\OMT Mini";
}

std::wstring start_menu_shortcut() {
    PWSTR programs = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        dir = programs;
        CoTaskMemFree(programs);
    }
    if (dir.empty()) return {};
    return dir + L"\\OMT Mini.lnk";
}

std::wstring desktop_shortcut() {
    PWSTR desktop = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
        dir = desktop;
        CoTaskMemFree(desktop);
    }
    if (dir.empty()) return {};
    return dir + L"\\OMT Mini.lnk";
}

void set_status(const wchar_t* text) {
    if (g_status) SetWindowTextW(g_status, text);
}

bool create_directories(const std::wstring& path) {
    return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS ||
           GetLastError() == ERROR_ALREADY_EXISTS ||
           PathIsDirectoryW(path.c_str());
}

bool make_shortcut(const std::wstring& link_path, const std::wstring& target,
                   const std::wstring& args, const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))))
        return false;

    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(target.c_str(), 0);

    std::wstring working = target;
    const size_t slash = working.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        working = working.substr(0, slash);
        link->SetWorkingDirectory(working.c_str());
    }

    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(link_path.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// ---- payload ------------------------------------------------------------
struct PayloadEntry {
    std::wstring name;
    uint64_t     offset;
    uint64_t     size;
};

bool read_payload_index(const std::wstring& self, std::vector<PayloadEntry>* entries) {
    HANDLE file = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart < static_cast<LONGLONG>(sizeof(OmtMiniPkgFooter))) {
        CloseHandle(file);
        return false;
    }

    LARGE_INTEGER pos{};
    pos.QuadPart = size.QuadPart - static_cast<LONGLONG>(sizeof(OmtMiniPkgFooter));
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);

    OmtMiniPkgFooter footer{};
    DWORD got = 0;
    if (!ReadFile(file, &footer, sizeof(footer), &got, nullptr) || got != sizeof(footer) ||
        memcmp(footer.magic, OMTMINI_PKG_MAGIC, OMTMINI_PKG_MAGIC_LEN) != 0) {
        CloseHandle(file);
        return false;
    }

    pos.QuadPart = static_cast<LONGLONG>(footer.payload_offset);
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);

    for (uint32_t i = 0; i < footer.entry_count; ++i) {
        uint16_t name_len = 0;
        if (!ReadFile(file, &name_len, sizeof(name_len), &got, nullptr) ||
            got != sizeof(name_len) || name_len == 0 || name_len > 512)
            break;

        std::string name(name_len, '\0');
        if (!ReadFile(file, name.data(), name_len, &got, nullptr) || got != name_len) break;

        // Entry names are written as bare file names by scripts/pack_payload.py.
        // Reject anything else rather than trust a file that may have been
        // tampered with: no separators, no drive letters, no parent traversal.
        if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos ||
            name.find(':') != std::string::npos || name.find("..") != std::string::npos) {
            entries->clear();
            break;
        }

        uint64_t data_size = 0;
        if (!ReadFile(file, &data_size, sizeof(data_size), &got, nullptr) ||
            got != sizeof(data_size))
            break;

        LARGE_INTEGER here{};
        LARGE_INTEGER zero{};
        SetFilePointerEx(file, zero, &here, FILE_CURRENT);

        PayloadEntry e;
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, name.c_str(),
                                                 static_cast<int>(name.size()), nullptr, 0);
        e.name.resize(static_cast<size_t>(wide_len));
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                            e.name.data(), wide_len);
        e.offset = static_cast<uint64_t>(here.QuadPart);
        e.size   = data_size;
        entries->push_back(std::move(e));

        LARGE_INTEGER skip{};
        skip.QuadPart = static_cast<LONGLONG>(data_size);
        SetFilePointerEx(file, skip, nullptr, FILE_CURRENT);
    }

    CloseHandle(file);
    return !entries->empty();
}

bool extract_entry(const std::wstring& self, const PayloadEntry& entry,
                   const std::wstring& dest_dir) {
    HANDLE src = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(entry.offset);
    SetFilePointerEx(src, pos, nullptr, FILE_BEGIN);

    const std::wstring out_path = dest_dir + L"\\" + entry.name;
    HANDLE dst = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        CloseHandle(src);
        return false;
    }

    std::vector<char> buffer(256 * 1024);
    uint64_t remaining = entry.size;
    bool ok = true;
    while (remaining > 0) {
        const DWORD want = static_cast<DWORD>(
            remaining < buffer.size() ? remaining : buffer.size());
        DWORD got = 0, put = 0;
        if (!ReadFile(src, buffer.data(), want, &got, nullptr) || got == 0) { ok = false; break; }
        if (!WriteFile(dst, buffer.data(), got, &put, nullptr) || put != got) { ok = false; break; }
        remaining -= got;
    }

    CloseHandle(dst);
    CloseHandle(src);
    return ok;
}

// ---- install / uninstall -------------------------------------------------
bool do_install(HWND window, bool startup, bool register_vcam, bool desktop_link) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    std::vector<PayloadEntry> entries;
    if (!read_payload_index(self, &entries)) {
        MessageBoxW(window,
                    L"This setup file is incomplete: no payload was found inside it.\n\n"
                    L"Download it again, or use the portable zip instead.",
                    kAppName, MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring dir = install_dir();
    set_status(L"Creating the program folder...");
    if (!create_directories(dir)) {
        MessageBoxW(window, L"The install folder could not be created.", kAppName,
                    MB_OK | MB_ICONERROR);
        return false;
    }

    // Stop a running copy so its files can be replaced. An update installs
    // over a copy that is shutting down threads, so wait for the window to go
    // rather than guessing at a fixed delay.
    set_status(L"Closing the running copy...");
    if (HWND running = FindWindowW(L"OMTMiniTray", nullptr)) {
        PostMessageW(running, WM_CLOSE, 0, 0);
        for (int i = 0; i < 100 && FindWindowW(L"OMTMiniTray", nullptr); ++i)
            Sleep(100);
        Sleep(300);
    }

    if (g_progress) {
        SendMessageW(g_progress, PBM_SETRANGE32, 0, static_cast<LPARAM>(entries.size()));
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        std::wstring status = L"Installing " + entries[i].name + L"...";
        set_status(status.c_str());
        if (g_progress) SendMessageW(g_progress, PBM_SETPOS, static_cast<WPARAM>(i + 1), 0);

        if (!extract_entry(self, entries[i], dir)) {
            std::wstring message = L"Could not write " + entries[i].name +
                                   L".\n\nIs OMT Mini still running?";
            MessageBoxW(window, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
            return false;
        }
    }

    const std::wstring exe = dir + L"\\" + kExeName;

    set_status(L"Creating shortcuts...");
    const std::wstring start_link = start_menu_shortcut();
    if (!start_link.empty())
        make_shortcut(start_link, exe, L"", L"Open Media Transport tray tools");
    if (desktop_link) {
        const std::wstring desk = desktop_shortcut();
        if (!desk.empty())
            make_shortcut(desk, exe, L"", L"Open Media Transport tray tools");
    }

    set_status(L"Registering...");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) == ERROR_SUCCESS) {
        auto put = [&](const wchar_t* name, const std::wstring& value) {
            RegSetValueExW(key, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        };
        put(L"DisplayName", kAppName);
        put(L"DisplayVersion", OMTMINI_VERSION_W);
        put(L"Publisher", L"OMT Mini");
        put(L"DisplayIcon", exe);
        put(L"InstallLocation", dir);
        put(L"UninstallString", L"\"" + exe + L"\" --uninstall");
        put(L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --silent");
        DWORD no_modify = 1;
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&no_modify), sizeof(no_modify));
        RegCloseKey(key);
    }

    if (startup) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE,
                            nullptr, &key, nullptr) == ERROR_SUCCESS) {
            const std::wstring value = L"\"" + exe + L"\" --tray";
            RegSetValueExW(key, kAppName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }

    if (register_vcam) {
        set_status(L"Registering the virtual camera...");
        const std::wstring dll = dir + L"\\omtmini_vcam.dll";
        HMODULE mod = LoadLibraryW(dll.c_str());
        if (mod) {
            using RegFn = HRESULT(WINAPI*)();
            auto fn = reinterpret_cast<RegFn>(reinterpret_cast<void*>(
                GetProcAddress(mod, "DllRegisterServer")));
            if (fn) fn();
            FreeLibrary(mod);
        }
    }

    set_status(L"Done.");
    return true;
}

void do_uninstall(bool silent) {
    const std::wstring dir = install_dir();

    HWND running = FindWindowW(L"OMTMiniTray", nullptr);
    if (running) {
        SendMessageW(running, WM_CLOSE, 0, 0);
        Sleep(600);
    }

    const std::wstring dll = dir + L"\\omtmini_vcam.dll";
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (mod) {
        using RegFn = HRESULT(WINAPI*)();
        auto fn = reinterpret_cast<RegFn>(reinterpret_cast<void*>(
            GetProcAddress(mod, "DllUnregisterServer")));
        if (fn) fn();
        FreeLibrary(mod);
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kAppName);
        RegCloseKey(key);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

    const std::wstring start_link = start_menu_shortcut();
    if (!start_link.empty()) DeleteFileW(start_link.c_str());
    const std::wstring desk = desktop_shortcut();
    if (!desk.empty()) DeleteFileW(desk.c_str());

    if (!silent) {
        MessageBoxW(nullptr,
                    L"OMT Mini has been removed.\n\nYour settings and logs are kept in "
                    L"%APPDATA%\\OMT Mini and can be deleted by hand.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
    }
}

// ---- window --------------------------------------------------------------
HWND make_checkbox(HWND parent, int id, int x, int y, int w, const wchar_t* text,
                   bool checked) {
    HWND box = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               x, y, w, 22, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               nullptr, nullptr);
    SendMessageW(box, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return box;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const int pad = 24;
            const int w = 452;

            HWND title = CreateWindowExW(0, L"STATIC", L"Install OMT Mini",
                                         WS_CHILD | WS_VISIBLE, pad, 22, w, 28, hwnd,
                                         nullptr, nullptr, nullptr);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), TRUE);

            const std::wstring where =
                L"Installs to " + install_dir() +
                L"\nNo administrator rights are needed.";
            HWND path = CreateWindowExW(0, L"STATIC", where.c_str(),
                                        WS_CHILD | WS_VISIBLE, pad, 56, w, 40, hwnd,
                                        reinterpret_cast<HMENU>(kIdPathLabel),
                                        nullptr, nullptr);
            SendMessageW(path, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            make_checkbox(hwnd, kIdStartup, pad, 112, w,
                          L"Start OMT Mini when I sign in", true);
            make_checkbox(hwnd, kIdVcam, pad, 140, w,
                          L"Register the virtual camera for other apps", true);
            make_checkbox(hwnd, kIdDesktopShortcut, pad, 168, w,
                          L"Add a desktop shortcut", false);

            g_progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                         WS_CHILD | WS_VISIBLE, pad, 208, w, 6, hwnd,
                                         reinterpret_cast<HMENU>(kIdProgress),
                                         nullptr, nullptr);

            g_status = CreateWindowExW(0, L"STATIC", L"Ready to install.",
                                       WS_CHILD | WS_VISIBLE, pad, 222, w, 20, hwnd,
                                       reinterpret_cast<HMENU>(kIdStatus),
                                       nullptr, nullptr);
            SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            HWND install = CreateWindowExW(0, L"BUTTON", L"Install",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                               BS_DEFPUSHBUTTON,
                                           pad + w - 210, 258, 100, 32, hwnd,
                                           reinterpret_cast<HMENU>(kIdInstall),
                                           nullptr, nullptr);
            SendMessageW(install, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          pad + w - 100, 258, 100, 32, hwnd,
                                          reinterpret_cast<HMENU>(kIdCancel),
                                          nullptr, nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kIdCancel && !g_busy) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (id == kIdInstall && !g_busy) {
                g_busy = true;
                EnableWindow(GetDlgItem(hwnd, kIdInstall), FALSE);
                EnableWindow(GetDlgItem(hwnd, kIdCancel), FALSE);

                const bool startup =
                    SendMessageW(GetDlgItem(hwnd, kIdStartup), BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool vcam =
                    SendMessageW(GetDlgItem(hwnd, kIdVcam), BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool desk =
                    SendMessageW(GetDlgItem(hwnd, kIdDesktopShortcut), BM_GETCHECK, 0, 0) ==
                    BST_CHECKED;

                if (do_install(hwnd, startup, vcam, desk)) {
                    const std::wstring exe = install_dir() + L"\\" + kExeName;
                    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                } else {
                    g_busy = false;
                    EnableWindow(GetDlgItem(hwnd, kIdInstall), TRUE);
                    EnableWindow(GetDlgItem(hwnd, kIdCancel), TRUE);
                    set_status(L"Installation did not complete.");
                }
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            if (g_busy) return 0;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t* cmd = GetCommandLineW();
    if (cmd && wcsstr(cmd, L"--uninstall")) {
        do_uninstall(wcsstr(cmd, L"--silent") != nullptr);
        CoUninitialize();
        return 0;
    }

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title_lf = ncm.lfMessageFont;
    title_lf.lfHeight = static_cast<LONG>(title_lf.lfHeight * 1.6);
    title_lf.lfWeight = FW_SEMIBOLD;
    g_title_font = CreateFontIndirectW(&title_lf);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"OMTMiniSetup";
    wc.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    RECT rc{ 0, 0, 500, 316 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    HWND window = CreateWindowExW(
        0, L"OMTMiniSetup", L"OMT Mini Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_font) DeleteObject(g_font);
    if (g_title_font) DeleteObject(g_title_font);
    CoUninitialize();
    return 0;
}
