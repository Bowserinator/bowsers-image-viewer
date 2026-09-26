#pragma once

#include <optional>
#include <string>
#include <vector>

namespace biv::platform {

// The C runtime's narrow `argv` passed to main() is decoded from the real
// (UTF-16) command line using the process's ANSI code page on Windows. Any
// argument containing a character outside that code page -- including a
// file or folder path opened via double-click, drag-and-drop, or "Open
// with", which is exactly how most non-ASCII paths reach this app on
// Windows -- comes through mangled (typically replaced with '?') and can no
// longer be recovered from argv itself.
//
// This re-reads the process's real command line as UTF-16 and re-encodes
// each argument as UTF-8, which round-trips losslessly through
// std::filesystem::path on every platform (see util/file.hpp's
// path_from_utf8()). Callers should use the returned argv in place of the
// one passed to main() for anything that ends up as a path.
//
// Returns std::nullopt on non-Windows platforms (argv there is already
// locale/UTF-8 bytes, nothing to fix), or if re-reading the command line
// failed for some reason; callers should fall back to the original argv in
// that case.
std::optional<std::vector<std::string>> windows_utf8_argv();

}  // namespace biv::platform
