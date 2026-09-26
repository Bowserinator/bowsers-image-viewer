#pragma once

#include <concepts>
#include <format>
#include <iostream>
#include <type_traits>
#include <variant>
#include <vector>

#include "vx.hpp"

namespace butil {

template <class... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overload(Ts...) -> overload<Ts...>;

template <typename T>
concept is_numeric = (std::integral<T> || std::floating_point<T>) && !std::same_as<std::remove_cvref_t<T>, bool>;

template <typename T>
concept is_printable = requires(std::ostream& os, const std::remove_reference_t<T>& t) {
    { os << t } -> std::same_as<std::ostream&>;
};

template <typename T>
concept is_formattable = std::formattable<T, char>;

template <typename T, typename... Args>
concept has_find = requires(T& t, Args&&... args) { t.find(std::forward<Args>(args)...); };

template <typename T, typename... Args>
concept has_contains = requires(T& t, Args&&... args) { t.contains(std::forward<Args>(args)...); };

namespace detail {
template <typename T>
struct is_vector_impl : std::false_type {};

template <typename T, typename Alloc>
struct is_vector_impl<std::vector<T, Alloc>> : std::true_type {};
}  // namespace detail

template <typename T>
concept is_vector = detail::is_vector_impl<std::remove_cvref_t<T>>::value;

namespace detail {
template <typename T>
struct is_variant_impl : std::false_type {};

template <typename... Ts>
struct is_variant_impl<std::variant<Ts...>> : std::true_type {};
}  // namespace detail

template <typename T>
concept is_variant = detail::is_variant_impl<std::remove_cvref_t<T>>::value;

}  // namespace butil