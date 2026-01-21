// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Manages an optional contained value, i.e. a value that may or may not be present.
 */

#pragma once

#include "azure/core/azure_assert.hpp"

#include <new> // for placement new
#include <type_traits>
#include <utility> // for swap and move

namespace Azure {
namespace _detail {
  struct NontrivialEmptyType final
  {
    constexpr NontrivialEmptyType() noexcept {}
  };
} // namespace _detail

/**
 * @brief Manages an optional contained value, i.e. a value that may or may not be present.
 *
 * @tparam T A type to represent contained values.
 */
template <class T> class Nullable final {
  union
  {
    _detail::NontrivialEmptyType m_disengaged; // due to constexpr rules for the default constructor
    T m_value;
  };

  bool m_hasValue;

public:
  /**
   * @brief Constructs a `%Nullable` that represents the absence of value.
   *
   */
  constexpr Nullable() : m_disengaged{}, m_hasValue(false) {}

  /**
   * @brief Constructs a `%Nullable` having an \p initialValue.
   *
   * @param initialValue A non-absent value to initialize with.
   */
  constexpr Nullable(T initialValue) noexcept(std::is_nothrow_move_constructible<T>::value)
      : m_value(std::move(initialValue)), m_hasValue(true)
  {
  }

  /**
   * @brief Constructs a `%Nullable` by copying another `%Nullable`.
   *
   * @param other Another `%Nullable` instance to copy.
   */
  Nullable(const Nullable& other) noexcept(std::is_nothrow_copy_constructible<T>::value)
      : m_disengaged{}, m_hasValue(other.m_hasValue)
  {
    if (m_hasValue)
    {
      ::new (static_cast<void*>(&m_value)) T(other.m_value);
    }
  }

  /**
   * @brief Constructs a `%Nullable` by moving in another `%Nullable`.
   *
   * @param other A `%Nullable` instance to move into the instance being constructed.
   */
  Nullable(Nullable&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
      : m_disengaged{}, m_hasValue(other.m_hasValue)
  {
    if (m_hasValue)
    {
      ::new (static_cast<void*>(&m_value)) T(std::move(other.m_value));
    }
  }

  /**
   * @brief Destructs the `%Nullable`, calling the destructor for the contained value if there is
   * one.
   *
   */
  ~Nullable()
  {
    if (m_hasValue)
    {
      m_value.~T();
    }
  }

  /**
   * @brief Destructs the contained value, if there is one.
   *
   */
  void Reset() noexcept(std::is_nothrow_destructible<T>::value) /* enforces termination */
  {
    if (m_hasValue)
    {
      m_hasValue = false;
      m_value.~T();
    }
  }

  /**
   * @brief Exchanges the contents.
   *
   * @param other An instance to exchange the contents with.
   */
  // this assumes that swap can't throw if T is nothrow move constructible because
  // is_nothrow_swappable is added in C++17
  void Swap(Nullable& other) noexcept(std::is_nothrow_move_constructible<T>::value)
  {
    if (m_hasValue)
    {
      if (other.m_hasValue)
      {
        using std::swap;
        swap(m_value, other.m_value);
      }
      else
      {
        ::new (static_cast<void*>(&other.m_value)) T(std::move(m_value)); // throws
        other.m_hasValue = true;
        Reset();
      }
    }
    else if (other.m_hasValue)
    {
      ::new (static_cast<void*>(&m_value)) T(std::move(other.m_value)); // throws
      m_hasValue = true;
      other.Reset();
    }
  }

  /**
   * @brief Invokes #Azure::Nullable::Swap while having a lowercase name that satisfies
   * `swappable` requirements (see details).
   *
   * @details Swappable requirements: https://en.cppreference.com/w/cpp/named_req/Swappable
   */
  friend void swap(Nullable& lhs, Nullable& rhs) noexcept(
      std::is_nothrow_move_constructible<T>::value)
  {
    lhs.Swap(rhs);
  }

  /// Assignment operator.
  Nullable& operator=(const Nullable& other)
  {
    // this copy and swap may be inefficient for some Ts but
    // it's a lot less code than the standard implementation :)
    Nullable{other}.Swap(*this);
    return *this;
  }

  /// Assignment operator with move semantics.
  Nullable& operator=(Nullable&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
  {
    // this move and swap may be inefficient for some Ts but
    // it's a lot less code than the standard implementation :)
    Nullable{std::move(other)}.Swap(*this);
    return *this;
  }

  /**
   * @brief Assignment operator from another type.
   *
   * @tparam U Type of \p other.
   *
   * @param other Other #Azure::Nullable.
   */
  template <
      class U = T,
      typename std::enable_if<
          !std::is_same<
              Nullable,
              typename std::remove_cv<typename std::remove_reference<U>::type>::type>::
                  value // Avoid repeated assignment
              && !(
                  std::is_scalar<U>::value
                  && std::is_same<T, typename std::decay<U>::type>::value) // Avoid repeated
                                                                           // assignment of
                                                                           // equivalent scalar
                                                                           // types
              && std::is_constructible<T, U>::value // Ensure the type is constructible
              && std::is_assignable<T&, U>::value, // Ensure the type is assignable
          int>::type
      = 0>
  Nullable& operator=(U&& other) noexcept(
      std::is_nothrow_constructible<T, U>::value&& std::is_nothrow_assignable<T&, U>::value)
  {
    if (m_hasValue)
    {
      m_value = std::forward<U>(other);
    }
    else
    {
      ::new (static_cast<void*>(&m_value)) T(std::forward<U>(other));
      m_hasValue = true;
    }
    return *this;
  }

  /**
   * @brief Construct the contained value in-place.
   *
   * @details If this instance already contains a value before the call, the contained value is
   * destroyed by calling its destructor.
   */
  template <class... U>
  T& Emplace(U&&... Args) noexcept(std::is_nothrow_constructible<T, U...>::value)
  {
    Reset();
    ::new (static_cast<void*>(&m_value)) T(std::forward<U>(Args)...);
    m_hasValue = true;
    return m_value;
  }

  /**
   * @brief Check whether a value is contained.
   *
   * @return `true` If a value is contained, `false` if value is absent.
   */
  bool HasValue() const noexcept { return m_hasValue; }

  /**
   * @brief Get the contained value.
   *
   */
  const T& Value() const& noexcept
  {
    AZURE_ASSERT_MSG(m_hasValue, "Empty Nullable, check HasValue() first.");

    return m_value;
  }

  /**
   * @brief Get the contained value reference.
   *
   */
  T& Value() & noexcept
  {
    AZURE_ASSERT_MSG(m_hasValue, "Empty Nullable, check HasValue() first.");

    return m_value;
  }

  /**
   * @brief Get the contained value (as rvalue reference).
   *
   */
  T&& Value() && noexcept
  {
    AZURE_ASSERT_MSG(m_hasValue, "Empty Nullable, check HasValue() first.");

    return std::move(m_value);
  }

  // observers

  /**
   * @brief `operator bool` on the condition of #Azure::Nullable::HasValue.
   *
   */
  constexpr explicit operator bool() const noexcept { return HasValue(); }

  /**
   * @brief Accesses the contained value.
   * @return Returns a pointer to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr const T* operator->() const { return std::addressof(m_value); }

  /**
   * @brief Accesses the contained value.
   * @return Returns a pointer to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr T* operator->() { return std::addressof(m_value); }

  /**
   * @brief Accesses the contained value.
   * @return Returns a reference to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr const T& operator*() const& { return m_value; }

  /**
   * @brief Accesses the contained value.
   * @return Returns a reference to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr T& operator*() & { return m_value; }

  /**
   * @brief Accesses the contained value.
   * @return Returns a reference to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr T&& operator*() && { return std::move(m_value); }

  /**
   * @brief Accesses the contained value.
   * @return Returns a reference to the contained value.
   * @warning The behavior is undefined if `*this` does not contain a value.
   * @note This operator does not check whether the #Nullable contains a value!
           You can do so manually by using #HasValue() or simply #operator bool().
           Alternatively, if checked access is needed, #Value() or #ValueOr() may be used.
   */
  constexpr const T&& operator*() const&& { return std::move(m_value); }

  /**
   * @brief Get the contained value, returns \p other if value is absent.
   * @param other A value to return when no value is contained.
   * @return A contained value (when present), or \p other.
   */
  template <
      class U = T,
      typename std::enable_if<
          std::is_convertible<const T&, typename std::remove_cv<T>::type>::value
              && std::is_convertible<U, T>::value,
          int>::type
      = 0>
  constexpr typename std::remove_cv<T>::type ValueOr(U&& other) const&
  {
    if (m_hasValue)
    {
      return m_value;
    }

    return static_cast<typename std::remove_cv<T>::type>(std::forward<U>(other));
  }

  /**
   * @brief Get the contained value, returns \p other if value is absent.
   * @param other A value to return when no value is contained.
   * @return A contained value (when present), or \p other.
   */
  template <
      class U = T,
      typename std::enable_if<
          std::is_convertible<T, typename std::remove_cv<T>::type>::value
              && std::is_convertible<U, T>::value,
          int>::type
      = 0>
  constexpr typename std::remove_cv<T>::type ValueOr(U&& other) &&
  {
    if (m_hasValue)
    {
      return std::move(m_value);
    }

    return static_cast<typename std::remove_cv<T>::type>(std::forward<U>(other));
  }
};
} // namespace Azure
