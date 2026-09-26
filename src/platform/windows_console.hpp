#pragma once

namespace biv::platform {

// The Windows build is linked as a GUI-subsystem app so double-clicking it does
// not open a terminal window. Such a process has no console, so --help,
// --version and log output would vanish when it is started from cmd /
// PowerShell / Windows Terminal.
//
// Call this first thing in main(): if the process was started from a console
// it attaches to it and reconnects stdin/stdout/stderr. Streams that are
// already redirected (`> file`, `| more`) are left alone. No-op on other
// platforms, and when launched from Explorer (there is no console to attach to).
//
// Note: the parent shell does not wait for GUI-subsystem programs, so its
// prompt may reappear before the output finishes. That is normal Windows behaviour.
void attach_parent_console() noexcept;

}  // namespace biv::platform
