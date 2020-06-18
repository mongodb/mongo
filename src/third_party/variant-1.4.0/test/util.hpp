// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/config.hpp>

enum Qual { Ptr, ConstPtr, LRef, ConstLRef, RRef, ConstRRef };

struct get_qual_t {
  constexpr Qual operator()(int *) const { return Ptr; }
  constexpr Qual operator()(const int *) const { return ConstPtr; }
  constexpr Qual operator()(int &) const { return LRef; }
  constexpr Qual operator()(const int &) const { return ConstLRef; }
  constexpr Qual operator()(int &&) const { return RRef; }
  constexpr Qual operator()(const int &&) const { return ConstRRef; }
};

constexpr get_qual_t get_qual{};

#ifdef MPARK_EXCEPTIONS
struct CopyConstruction : std::exception {};
struct CopyAssignment : std::exception {};
struct MoveConstruction : std::exception {};
struct MoveAssignment : std::exception {};

struct copy_thrower_t {
  constexpr copy_thrower_t() {}
  [[noreturn]] copy_thrower_t(const copy_thrower_t &) {
    throw CopyConstruction{};
  }
  copy_thrower_t(copy_thrower_t &&) = default;
  copy_thrower_t &operator=(const copy_thrower_t &) { throw CopyAssignment{}; }
  copy_thrower_t &operator=(copy_thrower_t &&) = default;
};

inline bool operator<(const copy_thrower_t &,
                      const copy_thrower_t &) noexcept {
  return false;
}

inline bool operator>(const copy_thrower_t &,
                      const copy_thrower_t &) noexcept {
  return false;
}

inline bool operator<=(const copy_thrower_t &,
                       const copy_thrower_t &) noexcept {
  return true;
}

inline bool operator>=(const copy_thrower_t &,
                       const copy_thrower_t &) noexcept {
  return true;
}

inline bool operator==(const copy_thrower_t &,
                       const copy_thrower_t &) noexcept {
  return true;
}

inline bool operator!=(const copy_thrower_t &,
                       const copy_thrower_t &) noexcept {
  return false;
}

struct move_thrower_t {
  constexpr move_thrower_t() {}
  move_thrower_t(const move_thrower_t &) = default;
  [[noreturn]] move_thrower_t(move_thrower_t &&) { throw MoveConstruction{}; }
  move_thrower_t &operator=(const move_thrower_t &) = default;
  move_thrower_t &operator=(move_thrower_t &&) { throw MoveAssignment{}; }
};

inline bool operator<(const move_thrower_t &,
                      const move_thrower_t &) noexcept {
  return false;
}

inline bool operator>(const move_thrower_t &,
                      const move_thrower_t &) noexcept {
  return false;
}

inline bool operator<=(const move_thrower_t &,
                       const move_thrower_t &) noexcept {
  return true;
}

inline bool operator>=(const move_thrower_t &,
                       const move_thrower_t &) noexcept {
  return true;
}

inline bool operator==(const move_thrower_t &,
                       const move_thrower_t &) noexcept {
  return true;
}

inline bool operator!=(const move_thrower_t &,
                       const move_thrower_t &) noexcept {
  return false;
}

#endif
