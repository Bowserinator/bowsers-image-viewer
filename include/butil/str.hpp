#pragma once

#include <cctype>
#include <charconv>
#include <generator>
#include <optional>
#include <ranges>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace butil {

// ========= Str basic utils ==============

/// Universal to_string function
template <class T>
std::string to_string(T&& val) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::string>) {
        return std::forward<T>(val);
    } else if constexpr (std::is_same_v<Decayed, char*> || std::is_same_v<Decayed, const char*> ||
                         std::is_same_v<Decayed, std::string_view>) {
        return std::string(val);
    } else if constexpr (std::is_arithmetic_v<Decayed>) {
        return std::to_string(val);
    } else {
        static_assert(sizeof(T) == 0, "Type not supported by generic to_string");
        return {};
    }
}

// Check if a string/string_view represents a valid integer
inline bool is_int(std::string_view s) {
    int value      = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

// Check if a string/string_view represents a valid float/double
inline bool is_float(std::string_view s) {
    double value   = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

// Join string with sep
template <std::ranges::input_range R>
inline std::string join(R&& items, std::string_view sep) {
    // ranges::to: unlike the iterator-pair constructor it also accepts
    // non-common ranges (e.g. a transform_view of strings).
    return std::ranges::to<std::string>(items | std::views::join_with(sep));
}

constexpr std::string_view WHITESPACE = " \t\n\r\f\v";

constexpr std::string_view ltrim(std::string_view s) {
    auto start = s.find_first_not_of(WHITESPACE);
    return (start == std::string_view::npos) ? "" : s.substr(start);
}

constexpr std::string_view rtrim(std::string_view s) {
    auto end = s.find_last_not_of(WHITESPACE);
    return (end == std::string_view::npos) ? "" : s.substr(0, end + 1);
}

constexpr std::string_view trim(std::string_view s) {
    return rtrim(ltrim(s));
}

// str replace (old, new)
inline std::string replace(std::string_view s, std::string_view old_sub, std::string_view new_sub) {
    if (old_sub.empty())
        return std::string(s);
    std::string result;
    result.reserve(s.size());

    size_t pos  = 0;
    size_t prev = 0;
    while ((pos = s.find(old_sub, prev)) != std::string_view::npos) {
        result.append(s, prev, pos - prev);
        result.append(new_sub);
        prev = pos + old_sub.size();
    }
    result.append(s, prev, std::string_view::npos);
    return result;
}

// str.split() view
inline std::generator<std::string_view> split_gen(std::string_view s, std::string_view sep) {
    if (sep.empty()) {
        if (!s.empty())
            co_yield s;
        co_return;
    }

    size_t pos  = 0;
    size_t prev = 0;
    while ((pos = s.find(sep, prev)) != std::string_view::npos) {
        co_yield s.substr(prev, pos - prev);
        prev = pos + sep.size();
    }
    co_yield s.substr(prev);
}

inline std::vector<std::string_view> split(std::string_view s, std::string_view sep) {
    return split_gen(s, sep) | std::ranges::to<std::vector>();
}

inline std::generator<std::string_view> regex_split_gen(std::string_view s, const std::regex& re) {
    std::cregex_token_iterator it(s.data(), s.data() + s.size(), re, -1);
    std::cregex_token_iterator last;

    while (it != last) {
        if (it->matched)
            co_yield std::string_view(it->first, it->second);
        ++it;
    }
}

inline std::generator<std::string_view> regex_split_gen(std::string_view s, std::string_view pattern) {
    std::regex re(pattern.begin(), pattern.end());
    for (auto token : regex_split_gen(s, re))
        co_yield token;
}

inline std::vector<std::string_view> regex_split(std::string_view s, const std::regex& re) {
    return regex_split_gen(s, re) | std::ranges::to<std::vector>();
}

inline std::vector<std::string_view> regex_split(std::string_view s, std::string_view pattern) {
    return regex_split_gen(s, pattern) | std::ranges::to<std::vector>();
}

// str.lower()
inline std::string lower(std::string_view s) {
    auto v = s | std::views::transform([](unsigned char c) { return std::tolower(c); });
    return std::string(v.begin(), v.end());
}

// str.upper()
inline std::string upper(std::string_view s) {
    auto v = s | std::views::transform([](unsigned char c) { return std::toupper(c); });
    return std::string(v.begin(), v.end());
}

inline std::string line_wrap(std::string_view text, size_t max_width) {
    if (max_width == 0 || text.empty())
        return std::string(text);

    std::string result;
    result.reserve(text.size());
    size_t current_line_length = 0;
    bool first_word_in_line    = true;

    for (std::string_view word : split_gen(text, " ")) {
        if (word.empty())
            continue;

        if (first_word_in_line) {
            result.append(word);
            current_line_length = word.size();
            first_word_in_line  = false;
        } else if (current_line_length + 1 + word.size() <= max_width) {
            result.push_back(' ');
            result.append(word);
            current_line_length += 1 + word.size();
        } else {  // Wrap to new line
            result.push_back('\n');
            result.append(word);
            current_line_length = word.size();
        }
    }

    return result;
}

inline std::string ensure_prefix(std::string_view s, std::string_view prefix) {
    if (s.starts_with(prefix))
        return std::string(s);
    std::string result;
    result.reserve(prefix.size() + s.size());
    result.append(prefix);
    result.append(s);
    return result;
}

inline std::string ensure_suffix(std::string_view s, std::string_view suffix) {
    if (s.ends_with(suffix))
        return std::string(s);
    std::string result;
    result.reserve(s.size() + suffix.size());
    result.append(s);
    result.append(suffix);
    return result;
}

// ========= Str stripping ===============

inline std::string remove_ansi(std::string_view s) {
    static const std::regex ansi_regex(R"(\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~]))");
    return std::regex_replace(std::string(s), ansi_regex, "");
}

inline std::string remove_unprintable(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s)
        if (std::isprint(c))
            result.push_back(c);
    return result;
}

inline std::string remove_nonalphanumeric(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s)
        if (std::isalnum(c))
            result.push_back(c);
    return result;
}

inline std::string remove_nonascii(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s)
        if (c <= 127)
            result.push_back(c);
    return result;
}

// ========= Str conversion ==============

struct ParseError : public std::runtime_error {
    explicit ParseError(std::string_view m) : std::runtime_error(std::string(m)) {}
};

template <typename T>
struct str_converter;

template <typename T>
auto str_to(std::string_view sv) -> std::optional<T> {
    return str_converter<T>::coerce(sv);
}

template <typename T>
auto str_to(std::string_view sv, std::size_t& consumed) -> std::optional<T> {
    consumed = 0;
    auto r   = str_converter<T>::coerce(sv);
    if (r)
        consumed = sv.size();
    return r;
}

template <typename T>
auto str_to_required(std::string_view sv) -> T {
    if (auto r = str_to<T>(sv); r)
        return *r;
    throw ParseError("cannot convert '" + std::string(sv) + "'");
}

template <typename T>
auto try_str_to(std::string_view sv) -> std::optional<T> {
    return str_to<T>(sv);
}

// Built-in specializations

template <>
struct str_converter<int> {
    static auto coerce(std::string_view sv) -> std::optional<int> {
        if (sv.empty())
            return std::nullopt;
        int v          = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        // Require the whole string to be consumed (matches is_int()'s
        // semantics); std::stoi would silently accept "123abc" as 123.
        if (ec != std::errc{} || ptr != sv.data() + sv.size())
            return std::nullopt;
        return v;
    }
};

template <>
struct str_converter<double> {
    static auto coerce(std::string_view sv) -> std::optional<double> {
        if (sv.empty())
            return std::nullopt;
        double v       = 0.0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (ec != std::errc{} || ptr != sv.data() + sv.size())
            return std::nullopt;
        return v;
    }
};

template <>
struct str_converter<std::string> {
    static auto coerce(std::string_view sv) -> std::optional<std::string> { return std::string(sv); }
};

template <>
struct str_converter<std::string_view> {
    static auto coerce(std::string_view sv) -> std::optional<std::string_view> { return sv; }
};

template <>
struct str_converter<bool> {
    static auto coerce(std::string_view sv) -> std::optional<bool> {
        if (sv.empty())
            return std::nullopt;
        std::string s = std::string(sv);
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "1" || s == "true" || s == "yes")
            return true;
        if (s == "0" || s == "false" || s == "no")
            return false;
        return std::nullopt;
    }
};

}  // namespace butil