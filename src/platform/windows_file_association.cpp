#include "platform/windows_file_association.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "util/store.hpp"  // default_config_dir()

namespace {

bool set_string_value(HKEY root, const std::wstring& subkey, std::wstring_view name, const std::wstring& value) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const wchar_t* value_name = name.empty() ? nullptr : name.data();
    const auto* bytes = reinterpret_cast<const BYTE*>(value.c_str());
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const bool ok = RegSetValueExW(key, value_name, 0, REG_SZ, bytes, size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool set_empty_value(HKEY root, const std::wstring& subkey, std::wstring_view name) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const wchar_t* value_name = name.empty() ? nullptr : name.data();
    const bool ok = RegSetValueExW(key, value_name, 0, REG_SZ, nullptr, 0) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

std::wstring executable_path() noexcept {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return std::wstring(buffer.data(), length);
}

// config.hpp's extensions are plain ASCII (".png", ".jpg", ...), so a
// byte-for-byte widen is exact -- no locale/codepage conversion needed.
std::vector<std::wstring> widen_ascii_extensions(std::span<const std::string_view> exts) {
    std::vector<std::wstring> out;
    out.reserve(exts.size());
    for (auto ext : exts)
        out.emplace_back(ext.begin(), ext.end());
    return out;
}

std::string utf8_from_wide(const std::wstring& w) noexcept {
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

// Marker file recording the exe path + app version we last registered
// associations for, so a normal launch (nothing changed) doesn't repeat
// ~10 registry writes every single time the app starts. Re-registers
// whenever the install location moves or the app is upgraded.
std::filesystem::path registration_marker_path() {
    return biv::default_config_dir() / "file_associations.registered";
}

bool already_registered(const std::string& stamp) noexcept {
    std::ifstream in(registration_marker_path(), std::ios::binary);
    if (!in)
        return false;
    const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return existing == stamp;
}

void write_registration_marker(const std::string& stamp) noexcept {
    std::error_code ec;
    std::filesystem::create_directories(registration_marker_path().parent_path(), ec);
    std::ofstream out(registration_marker_path(), std::ios::binary | std::ios::trunc);
    if (out)
        out << stamp;
}

}  // namespace

namespace biv::platform {

void register_windows_file_associations() noexcept {
    const std::wstring exe = executable_path();
    if (exe.empty())
        return;

    // Nothing that would change the registration (install location, app
    // version) has changed since last time -- skip the ~10 registry writes.
    const std::string stamp = utf8_from_wide(exe) + "|" + std::string(biv::kAPP_VERSION);
    if (already_registered(stamp))
        return;

    constexpr std::wstring_view kProgId = L"BowsersImageViewer.Image";
    constexpr std::wstring_view kFriendlyName = L"Bowsers Image Viewer";
    const std::vector<std::wstring> kExtensions = widen_ascii_extensions(biv::kIMAGE_EXTENSIONS);

    const auto slash = exe.find_last_of(L"\\/");
    const std::wstring exe_name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    const std::wstring icon = exe + L",0";
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";

    // Per-user ProgID. We deliberately do not overwrite the user's default
    // application choice; the ProgID is exposed through OpenWithProgIds below.
    const std::wstring progid_key = L"Software\\Classes\\" + std::wstring(kProgId);
    set_string_value(HKEY_CURRENT_USER, progid_key, L"", std::wstring(kFriendlyName));
    set_string_value(HKEY_CURRENT_USER, progid_key + L"\\DefaultIcon", L"", icon);
    set_string_value(HKEY_CURRENT_USER, progid_key + L"\\shell\\open\\command", L"", command);

    // Advertise the executable for each supported extension in the normal
    // Windows "Open with" chooser without taking over the default handler.
    const std::wstring app_key = L"Software\\Classes\\Applications\\" + exe_name;
    set_string_value(HKEY_CURRENT_USER, app_key + L"\\shell\\open\\command", L"", command);
    for (const auto& ext : kExtensions)
        set_empty_value(HKEY_CURRENT_USER, app_key + L"\\SupportedTypes", ext);

    for (const auto& ext : kExtensions) {
        const std::wstring open_with = L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids";
        set_empty_value(HKEY_CURRENT_USER, open_with, kProgId);
    }

    // Explicit Explorer context-menu entry for image files. On Windows 11 it
    // may live behind "Show more options" depending on the user's shell mode.
    const std::wstring shell_key =
        L"Software\\Classes\\SystemFileAssociations\\image\\shell\\BowsersImageViewer";
    set_string_value(HKEY_CURRENT_USER, shell_key, L"MUIVerb", std::wstring(L"Open with ") + std::wstring(kFriendlyName));
    set_string_value(HKEY_CURRENT_USER, shell_key, L"Icon", icon);
    set_string_value(HKEY_CURRENT_USER, shell_key + L"\\command", L"", command);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    write_registration_marker(stamp);
}

}  // namespace biv::platform

#else

namespace biv::platform {

void register_windows_file_associations() noexcept {}

}  // namespace biv::platform

#endif
