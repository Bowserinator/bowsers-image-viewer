#pragma once
#include <string_view>

#include "platform.hpp"

namespace butil {

constexpr std::string_view RESET      = "\033[0m";
constexpr std::string_view BOLD       = "\033[1m";
constexpr std::string_view DIM        = "\033[2m";
constexpr std::string_view UNDERLINE  = "\033[4m";
constexpr std::string_view FG_RED     = "\033[31m";
constexpr std::string_view FG_GREEN   = "\033[32m";
constexpr std::string_view FG_YELLOW  = "\033[33m";
constexpr std::string_view FG_BLUE    = "\033[34m";
constexpr std::string_view FG_MAGENTA = "\033[35m";
constexpr std::string_view FG_CYAN    = "\033[36m";
constexpr std::string_view FG_WHITE   = "\033[37m";
constexpr std::string_view BG_RED     = "\033[41m";
constexpr std::string_view BG_GREEN   = "\033[42m";
constexpr std::string_view BG_YELLOW  = "\033[43m";
constexpr std::string_view BG_BLUE    = "\033[44m";
constexpr std::string_view BG_MAGENTA = "\033[45m";
constexpr std::string_view BG_CYAN    = "\033[46m";
constexpr std::string_view BG_WHITE   = "\033[47m";

inline bool USE_COLOR = platform::is_tty(platform::kStdoutFd);

}  // namespace butil