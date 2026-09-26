#pragma once

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string_view>
#include <utility>

#include "color.hpp"

namespace butil {

enum class Priority { DEBUG, INFO, WARN, ERROR, CRITICAL };

class Logger {
public:
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    explicit Logger(bool show_time = true, bool show_color = true) : m_show_time(show_time), m_show_color(show_color) {}

    [[nodiscard]] Priority get_priority() const noexcept { return m_min_priority; }

    void set_priority(Priority priority) noexcept { m_min_priority = priority; }

    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        log_message(Priority::DEBUG, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        log_message(Priority::INFO, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        log_message(Priority::WARN, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        log_message(Priority::ERROR, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void critical(std::format_string<Args...> fmt, Args&&... args) const {
        log_message(Priority::CRITICAL, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void print(std::format_string<Args...> fmt, Args&&... args) const {
        std::println(fmt, std::forward<Args>(args)...);
    }

    // Info log then exit 0
    template <typename... Args>
    [[noreturn]] void quit(std::format_string<Args...> fmt, Args&&... args) const {
        info(fmt, std::forward<Args>(args)...);
        std::exit(0);
    }

    // Critical log then exit -1
    template <typename... Args>
    [[noreturn]] void die(std::format_string<Args...> fmt, Args&&... args) const {
        critical(fmt, std::forward<Args>(args)...);
        std::exit(-1);
    }

    // Critical log then abort (fault)
    template <typename... Args>
    [[noreturn]] void dbgerror(std::format_string<Args...> fmt, Args&&... args) const {
        critical(fmt, std::forward<Args>(args)...);
        std::abort();
    }

private:
    Priority m_min_priority = Priority::DEBUG;
    bool m_show_time        = true;
    bool m_show_color       = true;

    struct LevelFormatting {
        std::string_view tag;
        std::string_view color;
    };

    static constexpr LevelFormatting get_level_formatting(Priority priority) noexcept {
        switch (priority) {
            case Priority::DEBUG: return {"[DEBUG]", DIM};                                // Gray / Dim
            case Priority::INFO: return {"[INFO] ", FG_BLUE};                             // Blue
            case Priority::WARN: return {"[WARN] ", FG_YELLOW};                           // Yellow
            case Priority::ERROR: return {"[ERROR]", FG_RED};                             // Red
            case Priority::CRITICAL: return {"[CRIT] ", std::string_view("\033[1;31m")};  // Bold Red
        }
        return {"[LOG]  ", RESET};
    }

    template <typename... Args>
    void log_message(Priority priority, std::format_string<Args...> fmt, Args&&... args) const {
        if (priority < m_min_priority)
            return;

        const bool enable_color        = m_show_color && USE_COLOR;
        const auto [tag, color]        = get_level_formatting(priority);
        std::string formatted_user_msg = std::format(fmt, std::forward<Args>(args)...);

        std::string line;
        if (m_show_time) {
            auto now = std::chrono::system_clock::now();
            if (enable_color)
                line = std::format("{}{:%H:%M:%S}{} {}{}{} {}", DIM, now, RESET, color, tag, RESET, formatted_user_msg);
            else
                line = std::format("{:%H:%M:%S} {} {}", now, tag, formatted_user_msg);
        } else {
            if (enable_color)
                line = std::format("{}{}{} {}", color, tag, RESET, formatted_user_msg);
            else
                line = std::format("{} {}", tag, formatted_user_msg);
        }
        std::println("{}", line);
    }
};

inline Logger tlog{true, true};
inline Logger log{false, true};

}  // namespace butil
