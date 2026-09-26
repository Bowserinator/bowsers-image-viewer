#include "platform/windows_folder_sort.hpp"

#if defined(_WIN32)

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
// clang-format off
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propkey.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
// clang-format on

namespace {

constexpr DWORD kMaxDepth            = 64;    
constexpr DWORD kMaxChildrenPerLevel = 4096;  

std::optional<DWORD> read_dword(HKEY key, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return std::nullopt;
    return value;
}

std::optional<std::vector<BYTE>> as_single_item_idlist(const std::vector<BYTE>& raw) {
    if (raw.size() < sizeof(USHORT))
        return std::nullopt;
    std::vector<BYTE> buf(raw.size() + sizeof(USHORT), 0);
    std::memcpy(buf.data(), raw.data(), raw.size());
    return buf;
}

std::optional<std::vector<uint8_t>> read_binary_value(HKEY key, const wchar_t* value_name) {
    DWORD data_size = 0;
    if (RegQueryValueExW(key, value_name, nullptr, nullptr, nullptr, &data_size) != ERROR_SUCCESS || data_size == 0)
        return std::nullopt;

    std::vector<uint8_t> buffer(data_size);
    if (RegQueryValueExW(key, value_name, nullptr, nullptr, buffer.data(), &data_size) != ERROR_SUCCESS)
        return std::nullopt;

    return buffer;
}

// Safely compare PIDLs without passing relative PIDLs directly to SHGetFileInfoW
bool is_shellbag_node_match(LPCITEMIDLIST candidate_pidl, LPCITEMIDLIST target_node) {
    if (!candidate_pidl || !target_node)
        return false;

    // 1. Direct byte comparison via Shell API
    if (ILIsEqual(candidate_pidl, target_node))
        return true;

    // 2. Fallback: Parse display string from single-item shell extension blocks if available
    STRRET sr_cand{}, sr_targ{};
    IShellFolder* desktop = nullptr;
    bool matched = false;

    if (SUCCEEDED(SHGetDesktopFolder(&desktop))) {
        wchar_t name_cand[MAX_PATH] = {0};
        wchar_t name_targ[MAX_PATH] = {0};

        if (SUCCEEDED(desktop->GetDisplayNameOf(candidate_pidl, SHGDN_INFOLDER, &sr_cand)) &&
            SUCCEEDED(StrRetToBufW(&sr_cand, candidate_pidl, name_cand, MAX_PATH)) &&
            SUCCEEDED(desktop->GetDisplayNameOf(target_node, SHGDN_INFOLDER, &sr_targ)) &&
            SUCCEEDED(StrRetToBufW(&sr_targ, target_node, name_targ, MAX_PATH))) {
            matched = (_wcsicmp(name_cand, name_targ) == 0);
        }
        desktop->Release();
    }

    return matched;
}

std::optional<DWORD> walk_bagmru(HKEY parent, LPCITEMIDLIST remaining, DWORD depth) {
    if (!remaining || ILIsEmpty(remaining) || depth >= kMaxDepth)
        return std::nullopt;

    DWORD child_count = 0;
    if (RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &child_count, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        return std::nullopt;

    child_count = std::min(child_count, kMaxChildrenPerLevel);

    LPITEMIDLIST this_node = ILCloneFirst(remaining);
    if (!this_node)
        return std::nullopt;

    LPCITEMIDLIST next = ILGetNext(remaining);
    const bool is_terminal = (!next || ILIsEmpty(next));

    for (DWORD i = 0; i < child_count; ++i) {
        wchar_t name[16];
        DWORD name_len = std::size(name);
        if (RegEnumKeyExW(parent, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            continue;

        const auto raw_pidl = read_binary_value(parent, name);
        if (!raw_pidl)
            continue;

        const auto idlist = as_single_item_idlist(*raw_pidl);
        if (!idlist)
            continue;

        const auto* candidate_pidl = reinterpret_cast<LPCITEMIDLIST>(idlist->data());

        if (is_shellbag_node_match(candidate_pidl, this_node)) {
            HKEY child = nullptr;
            if (RegOpenKeyExW(parent, name, 0, KEY_READ, &child) == ERROR_SUCCESS) {
                // If we've hit the target end node, grab NodeSlot
                if (is_terminal) {
                    auto slot = read_dword(child, L"NodeSlot");
                    RegCloseKey(child);
                    ILFree(this_node);
                    return slot;
                }
                auto deeper = walk_bagmru(child, next, depth + 1);
                RegCloseKey(child);
                if (deeper) {
                    ILFree(this_node);
                    return deeper;
                }
            }
        }
    }

    ILFree(this_node);
    return std::nullopt;
}

}  // namespace

namespace biv::platform {

std::vector<uint8_t> read_sort_blob(HKEY key) {
    static const wchar_t* value_names[] = {L"SortColumns", L"Sort"};
    constexpr size_t kSortHeaderSize = 0x14;

    for (const auto* name : value_names) {
        DWORD type      = 0;
        DWORD data_size = 0;

        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &data_size) == ERROR_SUCCESS && 
            type == REG_BINARY && data_size >= (kSortHeaderSize + sizeof(SORTCOLUMN))) {

            std::vector<uint8_t> buffer(data_size);
            if (RegQueryValueExW(key, name, nullptr, nullptr, buffer.data(), &data_size) == ERROR_SUCCESS)
                return buffer;
        }
    }
    return {};
}

bool is_date_sort_key(const PROPERTYKEY& key) {
    static const PROPERTYKEY* const kDateKeys[] = {
        &PKEY_DateModified,
        &PKEY_DateCreated,
        &PKEY_DateAccessed,
        &PKEY_ItemDate,
        &PKEY_Photo_DateTaken,
        &PKEY_Media_DateEncoded,
        &PKEY_Media_DateReleased,
        &PKEY_DateImported,
        &PKEY_Document_DateCreated,
        &PKEY_Document_DateSaved,
    };

    for (const PROPERTYKEY* k : kDateKeys)
        if (IsEqualPropertyKey(key, *k))
            return true;

    if (IsEqualGUID(key.fmtid, PKEY_DateModified.fmtid) && key.pid >= 14 && key.pid <= 16)
        return true;

    return false;
}

std::optional<ExplorerFolderSort> detect_explorer_folder_sort(const std::filesystem::path& folder) {
    if (folder.empty())
        return std::nullopt;

    std::error_code ec;
    auto abs = std::filesystem::absolute(folder, ec);
    abs.make_preferred();

    if (ec)
        return std::nullopt;

    LPITEMIDLIST target = ILCreateFromPathW(abs.c_str());
    if (!target)
        return std::nullopt;

    HKEY bagmru = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU",
                      0, KEY_READ, &bagmru) != ERROR_SUCCESS) {
        ILFree(target);
        return std::nullopt;
    }

    const auto slot = walk_bagmru(bagmru, target, 0);
    RegCloseKey(bagmru);
    ILFree(target);

    if (!slot)
        return std::nullopt;

    const auto shell_path = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\" +
                            std::to_wstring(*slot) + L"\\Shell";

    HKEY shell = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, shell_path.c_str(), 0, KEY_READ, &shell) != ERROR_SUCCESS)
        return std::nullopt;

    static const wchar_t* kPreferredViewGuids[] = {
        L"{5C4F28B5-465D-4FCE-0FA8-643FEA795A2C}", // Details View Mode
        L"{B36E2A10-7201-468D-912A-17A300A52D4D}", // Tiles / Icon View
    };

    std::optional<ExplorerFolderSort> result = std::nullopt;

    // Step A: Check known primary GUIDs first
    for (const auto* guid : kPreferredViewGuids) {
        HKEY view_key = nullptr;
        if (RegOpenKeyExW(shell, guid, 0, KEY_READ, &view_key) == ERROR_SUCCESS) {
            auto sort_columns = read_sort_blob(view_key);
            RegCloseKey(view_key);

            constexpr size_t kHeaderSize  = 0x14;
            constexpr size_t kCountOffset = 0x10;

            if (sort_columns.size() >= kHeaderSize + sizeof(SORTCOLUMN)) {
                uint32_t col_count = 0;
                std::memcpy(&col_count, sort_columns.data() + kCountOffset, sizeof(uint32_t));

                if (col_count > 0) {
                    const SORTCOLUMN* sort_col = reinterpret_cast<const SORTCOLUMN*>(sort_columns.data() + kHeaderSize);
                    std::optional<SortMode> mode;

                    if (IsEqualPropertyKey(sort_col->propkey, PKEY_ItemNameDisplay) ||
                        IsEqualPropertyKey(sort_col->propkey, PKEY_FileName)) {
                        mode = SortMode::Name;
                    } else if (is_date_sort_key(sort_col->propkey)) {
                        mode = SortMode::MTime;
                    } else if (IsEqualPropertyKey(sort_col->propkey, PKEY_Size)) {
                        mode = SortMode::Size;
                    }

                    if (mode) {
                        result = ExplorerFolderSort{*mode, sort_col->direction == SORT_DESCENDING};
                        break;
                    }
                }
            }
        }
    }

	// Step B: Fall back to iterating subkeys if preferred GUIDs aren't present
    if (!result) {
        DWORD subkey_index = 0;
        wchar_t guid_subkey[256];
        DWORD name_len = std::size(guid_subkey);

        while (RegEnumKeyExW(shell, subkey_index++, guid_subkey, &name_len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            name_len = std::size(guid_subkey); // Reset size for next iteration
            HKEY view_key = nullptr;
            if (RegOpenKeyExW(shell, guid_subkey, 0, KEY_READ, &view_key) == ERROR_SUCCESS) {
                auto sort_columns = read_sort_blob(view_key);
                RegCloseKey(view_key);

                constexpr size_t kHeaderSize = 0x14;
                constexpr size_t kCountOffset = 0x10;

                if (sort_columns.size() >= kHeaderSize + sizeof(SORTCOLUMN)) {
                    uint32_t col_count = 0;
                    std::memcpy(&col_count, sort_columns.data() + kCountOffset, sizeof(uint32_t));

                    if (col_count > 0) {
                        const SORTCOLUMN* sort_col = reinterpret_cast<const SORTCOLUMN*>(sort_columns.data() + kHeaderSize);
                        std::optional<SortMode> mode;

                        if (IsEqualPropertyKey(sort_col->propkey, PKEY_ItemNameDisplay) ||
                            IsEqualPropertyKey(sort_col->propkey, PKEY_FileName)) {
                            mode = SortMode::Name;
                        } else if (is_date_sort_key(sort_col->propkey)) {
                            mode = SortMode::MTime;
                        } else if (IsEqualPropertyKey(sort_col->propkey, PKEY_Size)) {
                            mode = SortMode::Size;
                        }

                        if (mode) {
                            result = ExplorerFolderSort{*mode, sort_col->direction == SORT_DESCENDING};
                            break;
                        }
                    }
                }
            }
        }
    }

    RegCloseKey(shell);
    return result;
}

}  // namespace biv::platform

#else

namespace biv::platform {

std::optional<ExplorerFolderSort> detect_explorer_folder_sort(const std::filesystem::path&) {
    return std::nullopt;
}

}  // namespace biv::platform

#endif