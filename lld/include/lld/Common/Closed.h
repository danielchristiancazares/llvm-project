//===- Closed.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_CLOSED_H
#define LLD_COMMON_CLOSED_H

#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace lld {

namespace detail {

template <class... Ts> struct AreDistinct : std::true_type {};

template <class T, class... Ts>
struct AreDistinct<T, Ts...>
    : std::bool_constant<(!std::is_same_v<T, Ts> && ...) &&
                         AreDistinct<Ts...>::value> {};

template <class T, class = void>
struct HasNonTemplateCallOperator : std::false_type {};

template <class T>
struct HasNonTemplateCallOperator<
    T, std::void_t<decltype(&std::remove_reference_t<T>::operator())>>
    : std::true_type {};

template <class... Ts> struct Overload : Ts... {
  using Ts::operator()...;

  explicit Overload(Ts &&...Fns) : Ts(std::forward<Ts>(Fns))... {}
};

template <class... Ts> Overload(Ts &&...) -> Overload<Ts...>;

template <class MatchT, class... Fs>
constexpr size_t InvocableCount =
    (size_t(std::is_invocable_v<Fs &, MatchT>) + ... + 0u);

template <class MatchT, class... Fs>
constexpr bool HasExactMatch = InvocableCount<MatchT, Fs...> == 1u;

template <class MatchTuple, class... Fs> struct HasExactCoverageHelper;

template <class... MatchTs, class... Fs>
struct HasExactCoverageHelper<std::tuple<MatchTs...>, Fs...>
    : std::bool_constant<(HasExactMatch<MatchTs, Fs...> && ...)> {};

template <class MatchTuple, class... Fs>
constexpr bool HasExactCoverage =
    HasExactCoverageHelper<MatchTuple, Fs...>::value;

} // namespace detail

template <class... Ts> class Closed final {
  static_assert(sizeof...(Ts) != 0, "Closed requires at least one alternative");
  static_assert(detail::AreDistinct<Ts...>::value,
                "Closed alternatives must be pairwise distinct");
  static_assert((std::is_move_constructible_v<Ts> && ...),
                "Closed alternatives must be move constructible");

public:
  Closed() = delete;
  Closed(const Closed &) = delete;
  Closed &operator=(const Closed &) = delete;
  Closed(Closed &&) = default;
  Closed &operator=(Closed &&) = delete;

  template <class T, class... Args>
  [[nodiscard]] static Closed make(Args &&...ArgsList) {
    static_assert((std::is_same_v<T, Ts> || ...),
                  "Requested alternative is not part of this Closed type");
    return Closed(std::in_place_type<T>, std::forward<Args>(ArgsList)...);
  }

  template <class... Fs> decltype(auto) match(Fs &&...Fns) & {
    static_assert((detail::HasNonTemplateCallOperator<Fs>::value && ...),
                  "Closed::match requires concrete overloads");
    static_assert(detail::HasExactCoverage<std::tuple<Ts &...>, Fs...>,
                  "Closed::match requires exactly one overload per "
                  "alternative");
    return std::visit(
        detail::Overload<std::remove_reference_t<Fs>...>(
            std::forward<Fs>(Fns)...),
        storage);
  }

  template <class... Fs> decltype(auto) match(Fs &&...Fns) const & {
    static_assert((detail::HasNonTemplateCallOperator<Fs>::value && ...),
                  "Closed::match requires concrete overloads");
    static_assert(detail::HasExactCoverage<std::tuple<const Ts &...>, Fs...>,
                  "Closed::match requires exactly one overload per "
                  "alternative");
    return std::visit(
        detail::Overload<std::remove_reference_t<Fs>...>(
            std::forward<Fs>(Fns)...),
        storage);
  }

  template <class... Fs> decltype(auto) match(Fs &&...Fns) && {
    static_assert((detail::HasNonTemplateCallOperator<Fs>::value && ...),
                  "Closed::match requires concrete overloads");
    static_assert(detail::HasExactCoverage<std::tuple<Ts &&...>, Fs...>,
                  "Closed::match requires exactly one overload per "
                  "alternative");
    return std::visit(
        detail::Overload<std::remove_reference_t<Fs>...>(
            std::forward<Fs>(Fns)...),
        std::move(storage));
  }

private:
  template <class T, class... Args>
  explicit Closed(std::in_place_type_t<T>, Args &&...ArgsList)
      : storage(std::in_place_type<T>, std::forward<Args>(ArgsList)...) {}

  std::variant<Ts...> storage;
};

} // namespace lld

#endif
