#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <fstream>
#include <generator>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "concepts.hpp"
#include "platform.hpp"
#include "str.hpp"

namespace butil {

// --------------------- Process -------------------------
struct PopenUniquePtrDeleter {
    void operator()(FILE* fp) const {
        if (platform::pclose_raw(fp) == -1)
            std::cerr << "Error in pclose()\n";
    }
};

inline bool is_aty() {
    return platform::is_tty(platform::kStdoutFd) && platform::is_tty(platform::kStderrFd);
}

inline void sleep(int seconds) {
    if (seconds > 0)
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
}

struct XArgs {
    bool dryrun                        = false;
    bool die_on_err                    = true;
    std::optional<std::string> err_msg = std::nullopt;
    bool silent                        = false;
    bool pr_cmd_to_stdout              = false;
};

inline int x(const std::string& cmd, XArgs args = {}) {
    if (!args.silent)
        (args.pr_cmd_to_stdout ? std::cout : std::cerr) << "--- " << cmd << "\n";
    if (args.dryrun)
        return 0;

    FILE* raw_pipe = platform::popen_read(cmd);
    if (!raw_pipe) {
        if (args.die_on_err)
            throw std::runtime_error(args.err_msg.value_or("Failed to popen(): " + cmd));
        return -1;
    }

    int exit_code = -1;
    {
        std::unique_ptr<FILE, decltype([](FILE* fp) {
            if (fp)
                platform::pclose_raw(fp);
        })>
            pipe(raw_pipe);

        char buf[128];
        while (fgets(buf, sizeof(buf), pipe.get())) {}
        exit_code = platform::exit_code_from_pclose(platform::pclose_raw(pipe.release()));
    }

    if (exit_code != 0 && args.die_on_err)
        throw std::runtime_error(
            args.err_msg.value_or("Command failed with exit code " + std::to_string(exit_code) + ": " + cmd));
    return exit_code;
}

// --------------- File line iteration ----------------------

struct FileIterArgs {
    size_t skip                = 0;      // Skip first N lines
    bool skip_empty            = false;  // Skip empty lines
    bool ignore_comments       = false;  // Skip lines starting with '#' (after optional trimming)
    std::string comment_prefix = "#";
};

namespace detail {

inline std::generator<std::tuple<std::string, size_t>> iter_lines_stream(std::istream& is, FileIterArgs args) {
    std::string line;
    size_t line_number      = 0;
    size_t current_raw_line = 0;

    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r')  // handle windows
            line.pop_back();

        if (current_raw_line < args.skip) {
            ++current_raw_line;
            continue;
        }
        ++current_raw_line;

        if (args.skip_empty && line.empty())
            continue;

        if (args.ignore_comments) {
            std::string_view sv = ltrim(line);  // Uses existing ltrim helper
            if (sv.starts_with(args.comment_prefix))
                continue;
        }
        co_yield {line, line_number++};
    }
}

}  // namespace detail

inline std::generator<std::tuple<std::string, size_t>> iter_lines(std::istream& stream, FileIterArgs args = {}) {
    for (auto&& item : detail::iter_lines_stream(stream, args))
        co_yield item;
}

inline std::generator<std::tuple<std::string, size_t>> iter_lines(const std::string& filename, FileIterArgs args = {}) {
    std::ifstream file(filename);
    if (!file.is_open())
        co_return;
    for (auto&& item : detail::iter_lines_stream(file, args))
        co_yield item;
}

inline std::vector<std::string> read_lines(std::istream& stream, FileIterArgs args = {}) {
    return iter_lines(stream, args) |
           std::views::transform([](auto&& tuple) { return std::get<0>(std::move(tuple)); }) |
           std::ranges::to<std::vector>();
}

inline std::vector<std::string> read_lines(const std::string& filename, FileIterArgs args = {}) {
    return iter_lines(filename, args) |
           std::views::transform([](auto&& tuple) { return std::get<0>(std::move(tuple)); }) |
           std::ranges::to<std::vector>();
}

// ------------------- Universal contains --------------

template <class A, class B>
bool contains(const A& container, const B& item) {
    using CVDecay = std::remove_cv_t<A>;
    if constexpr (std::is_same_v<CVDecay, std::string>)
        return container.find(item) != std::string::npos;
    else if constexpr (std::is_same_v<CVDecay, std::string_view>)
        return container.find(item) != std::string_view::npos;
    else if constexpr (has_contains<CVDecay, B>)
        return container.contains(item);
    else if constexpr (has_find<CVDecay, B>)
        return container.find(item) != std::end(container);
    else
        return std::find(std::begin(container), std::end(container), item) != std::end(container);
}

// -------------- Get from map with default ----------------
template <class T, class K, class V>
V try_get(T& obj, const K& key, const V& def) {
    auto itr = obj.find(key);
    if (itr == std::end(obj))
        return def;
    static_assert(std::same_as<decltype(itr->second), V>);
    return itr->second;
}

// NOTE: `def` must be an lvalue that outlives the call (e.g. a local
// variable), not a temporary -- this function returns a reference to it when
// `key` isn't found, and a temporary's lifetime ends at the end of the full
// expression, which would leave the caller holding a dangling reference.
template <class T, class K, class V>
V& try_get_ref(T& obj, const K& key, V& def) {
    auto itr = obj.find(key);
    if (itr == std::end(obj))
        return def;
    static_assert(std::same_as<decltype(itr->second), V>);
    return itr->second;
}

// -------------- In place vector concat -----------------------
template <typename T, typename Alloc1, typename Range>
inline std::vector<T, Alloc1>& vec_concat(std::vector<T, Alloc1>& v1, const Range& v2) {
    v1.insert(v1.end(), std::ranges::begin(v2), std::ranges::end(v2));
    return v1;
}

template <typename T, typename Alloc1, typename Range>
inline std::vector<T, Alloc1> vec_concat(std::vector<T, Alloc1>&& v1, const Range& v2) {
    vec_concat(v1, v2);
    return std::move(v1);
}

template <typename T, typename Alloc1, typename Alloc2>
inline std::vector<T, Alloc1>& vec_move_concat(std::vector<T, Alloc1>& v1, std::vector<T, Alloc2>& v2) {
    v1.insert(v1.end(), std::make_move_iterator(v2.begin()), std::make_move_iterator(v2.end()));
    return v1;
}

template <typename T, typename Alloc1, typename Alloc2>
inline std::vector<T, Alloc1>& vec_move_concat(std::vector<T, Alloc1>& v1, std::vector<T, Alloc2>&& v2) {
    return vec_move_concat(v1, v2);
}

template <typename T, typename Alloc1, typename Alloc2>
inline std::vector<T, Alloc1> vec_move_concat(std::vector<T, Alloc1>&& v1, std::vector<T, Alloc2>&& v2) {
    vec_move_concat(v1, v2);
    return std::move(v1);
}

// ---------------- Null Buffer  --------------

struct NullBuffer : public std::streambuf {
    int overflow(int c) override { return c; }
};

inline NullBuffer null_buffer;
inline std::ostream devnull(&null_buffer);

}  // namespace butil