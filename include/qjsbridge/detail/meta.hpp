// SPDX-License-Identifier: MIT
// qjsbridge – template metaprogramming utilities
#pragma once

#include <type_traits>
#include <tuple>
#include <cstddef>

namespace qjsb {
namespace detail {

// C++20 remove_cvref backport
template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

// is_specialization<T, Template>: true if T is Template<...>
template <typename T, template <typename...> class Tmpl>
struct is_specialization : std::false_type {};

template <template <typename...> class Tmpl, typename... Args>
struct is_specialization<Tmpl<Args...>, Tmpl> : std::true_type {};

template <typename T, template <typename...> class Tmpl>
inline constexpr bool is_specialization_v = is_specialization<T, Tmpl>::value;

// ---------------------------------------------------------------------------
// function_traits<F>: decompose any callable into return_type / args_tuple
// ---------------------------------------------------------------------------
template <typename F, typename = void>
struct function_traits;

// Free function type R(Args...)
template <typename R, typename... Args>
struct function_traits<R(Args...), void> {
    using return_type = R;
    using args_tuple  = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);
    template <std::size_t I>
    using arg_t = std::tuple_element_t<I, args_tuple>;
};

// Pointer to free function (with / without noexcept)
template <typename R, typename... Args>
struct function_traits<R (*)(Args...), void> : function_traits<R(Args...)> {};

template <typename R, typename... Args>
struct function_traits<R (*)(Args...) noexcept, void> : function_traits<R(Args...)> {};

// Reference to free function
template <typename R, typename... Args>
struct function_traits<R (&)(Args...), void> : function_traits<R(Args...)> {};

// Non-const member function pointer
template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...), void> {
    using return_type = R;
    using class_type  = C;
    using args_tuple  = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);
    template <std::size_t I>
    using arg_t = std::tuple_element_t<I, args_tuple>;
};

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) noexcept, void>
    : function_traits<R (C::*)(Args...)> {};

// Const member function pointer
template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const, void> {
    using return_type = R;
    using class_type  = const C;
    using args_tuple  = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);
    template <std::size_t I>
    using arg_t = std::tuple_element_t<I, args_tuple>;
};

template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const noexcept, void>
    : function_traits<R (C::*)(Args...) const> {};

// Functor / lambda – delegate to operator()
template <typename F>
struct function_traits<F, std::void_t<decltype(&F::operator())>>
    : function_traits<decltype(&F::operator())> {};

// Convenience aliases
template <typename F>
using return_type_t = typename function_traits<remove_cvref_t<F>>::return_type;

template <typename F>
using args_tuple_t = typename function_traits<remove_cvref_t<F>>::args_tuple;

template <typename F>
inline constexpr std::size_t arity_v = function_traits<remove_cvref_t<F>>::arity;

// ---------------------------------------------------------------------------
// member_ptr_traits<M C::*>
// ---------------------------------------------------------------------------
template <typename MP>
struct member_ptr_traits;

template <typename C, typename M>
struct member_ptr_traits<M C::*> {
    using class_type  = C;
    using member_type = M;
};

template <typename MP>
using member_class_t = typename member_ptr_traits<MP>::class_type;

template <typename MP>
using member_value_t = typename member_ptr_traits<MP>::member_type;

// is_std_array: true for std::array<T, N>
template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_array_v = is_std_array<T>::value;

} // namespace detail
} // namespace qjsb
