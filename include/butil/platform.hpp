#pragma once

// platform.hpp
//
// Thin portability layer over the few POSIX-only APIs butil depends on, so the
// rest of the headers compile unchanged on Linux, macOS and Windows
// (MSVC and MinGW-w64 / MSYS2 UCRT64).
//
//   is_tty(fd)               isatty()  ->  _isatty()
//   popen_read(cmd)          popen()   ->  _popen()
//   pclose_raw(fp)           pclose()  ->  _pclose()
//   exit_code_from_pclose()  WIFEXITED / WEXITSTATUS on POSIX, identity on Windows
//
// <sys/wait.h> (WIFEXITED, WEXITSTATUS) and <unistd.h> are POSIX-only. MinGW
// ships a partial <unistd.h> but no <sys/wait.h> macros, and MSVC has neither.
// The Windows CRT reports a child's exit code directly from _pclose(), not
// packed into a wait-status word, so no decoding is needed there.

#include <cstdio>
#include <string>

#if defined(_WIN32)
    #include <io.h>  // _isatty
#else
    #include <sys/wait.h>  // WIFEXITED, WEXITSTATUS
    #include <unistd.h>    // isatty
#endif

namespace butil::platform {

inline constexpr int kStdoutFd = 1;
inline constexpr int kStderrFd = 2;

// True if the file descriptor refers to an interactive terminal / console.
inline bool is_tty(int fd) {
#if defined(_WIN32)
    return _isatty(fd) != 0;
#else
    return isatty(fd) != 0;
#endif
}

// Runs `cmd` through the system shell (/bin/sh -c, or cmd.exe /c) and returns
// a read-only pipe on its stdout. Returns nullptr on failure.
inline std::FILE* popen_read(const std::string& cmd) {
#if defined(_WIN32)
    // cmd.exe /c strips the first and last '"' of a command line that starts
    // with a quote, which mangles e.g.  "C:\Program Files\x.exe" "arg".
    // Wrapping the whole line in one extra pair of quotes is the standard fix.
    const std::string wrapped = "\"" + cmd + "\"";
    return _popen(wrapped.c_str(), "r");
#else
    return popen(cmd.c_str(), "r");
#endif
}

// Closes a pipe from popen_read(). Returns the raw platform status:
// a wait-status word on POSIX, the exit code on Windows, -1 on error.
inline int pclose_raw(std::FILE* fp) {
#if defined(_WIN32)
    return _pclose(fp);
#else
    return pclose(fp);
#endif
}

// Converts pclose_raw()'s result into the child's exit code.
// Returns -1 if it failed, or (POSIX only) was killed by a signal.
inline int exit_code_from_pclose(int status) {
    if (status == -1)
        return -1;
#if defined(_WIN32)
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace butil::platform
