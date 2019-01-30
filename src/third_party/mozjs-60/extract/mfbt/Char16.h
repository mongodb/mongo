/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implements a UTF-16 character type. */

#ifndef mozilla_Char16_h
#define mozilla_Char16_h

#ifdef __cplusplus

/*
 * C++11 introduces a char16_t type and support for UTF-16 string and character
 * literals. C++11's char16_t is a distinct builtin type. Technically, char16_t
 * is a 16-bit code unit of a Unicode code point, not a "character".
 */

#ifdef WIN32
# define MOZ_USE_CHAR16_WRAPPER
# include <cstdint>
# include "mozilla/Attributes.h"
  /**
   * Win32 API extensively uses wchar_t, which is represented by a separated
   * builtin type than char16_t per spec. It's not the case for MSVC prior to
   * MSVC 2015, but other compilers follow the spec. We want to mix wchar_t and
   * char16_t on Windows builds. This class is supposed to make it easier. It
   * stores char16_t const pointer, but provides implicit casts for wchar_t as
   * well. On other platforms, we simply use
   * |typedef const char16_t* char16ptr_t|. Here, we want to make the class as
   * similar to this typedef, including providing some casts that are allowed
   * by the typedef.
   */
class char16ptr_t
{
private:
  const char16_t* mPtr;
  static_assert(sizeof(char16_t) == sizeof(wchar_t),
                "char16_t and wchar_t sizes differ");

public:
  MOZ_IMPLICIT char16ptr_t(const char16_t* aPtr) : mPtr(aPtr) {}
  MOZ_IMPLICIT char16ptr_t(const wchar_t* aPtr) :
    mPtr(reinterpret_cast<const char16_t*>(aPtr))
  {}

  /* Without this, nullptr assignment would be ambiguous. */
  constexpr MOZ_IMPLICIT char16ptr_t(decltype(nullptr)) : mPtr(nullptr) {}

  operator const char16_t*() const
  {
    return mPtr;
  }
  operator const wchar_t*() const
  {
    return reinterpret_cast<const wchar_t*>(mPtr);
  }

  operator wchar_t*()
  {
    return const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(mPtr));
  }

  operator const void*() const
  {
    return mPtr;
  }
  explicit operator bool() const
  {
    return mPtr != nullptr;
  }

  /* Explicit cast operators to allow things like (char16_t*)str. */
  explicit operator char16_t*() const
  {
    return const_cast<char16_t*>(mPtr);
  }
  explicit operator wchar_t*() const
  {
    return const_cast<wchar_t*>(static_cast<const wchar_t*>(*this));
  }
  explicit operator int() const
  {
    return reinterpret_cast<intptr_t>(mPtr);
  }
  explicit operator unsigned int() const
  {
    return reinterpret_cast<uintptr_t>(mPtr);
  }
  explicit operator long() const
  {
    return reinterpret_cast<intptr_t>(mPtr);
  }
  explicit operator unsigned long() const
  {
    return reinterpret_cast<uintptr_t>(mPtr);
  }
  explicit operator long long() const
  {
    return reinterpret_cast<intptr_t>(mPtr);
  }
  explicit operator unsigned long long() const
  {
    return reinterpret_cast<uintptr_t>(mPtr);
  }

  /**
   * Some Windows API calls accept BYTE* but require that data actually be
   * WCHAR*.  Supporting this requires explicit operators to support the
   * requisite explicit casts.
   */
  explicit operator const char*() const
  {
    return reinterpret_cast<const char*>(mPtr);
  }
  explicit operator const unsigned char*() const
  {
    return reinterpret_cast<const unsigned char*>(mPtr);
  }
  explicit operator unsigned char*() const
  {
    return
      const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(mPtr));
  }
  explicit operator void*() const
  {
    return const_cast<char16_t*>(mPtr);
  }

  /* Some operators used on pointers. */
  char16_t operator[](size_t aIndex) const
  {
    return mPtr[aIndex];
  }
  bool operator==(const char16ptr_t& aOther) const
  {
    return mPtr == aOther.mPtr;
  }
  bool operator==(decltype(nullptr)) const
  {
    return mPtr == nullptr;
  }
  bool operator!=(const char16ptr_t& aOther) const
  {
    return mPtr != aOther.mPtr;
  }
  bool operator!=(decltype(nullptr)) const
  {
    return mPtr != nullptr;
  }
  char16ptr_t operator+(int aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  char16ptr_t operator+(unsigned int aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  char16ptr_t operator+(long aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  char16ptr_t operator+(unsigned long aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  char16ptr_t operator+(long long aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  char16ptr_t operator+(unsigned long long aValue) const
  {
    return char16ptr_t(mPtr + aValue);
  }
  ptrdiff_t operator-(const char16ptr_t& aOther) const
  {
    return mPtr - aOther.mPtr;
  }
};

inline decltype((char*)0-(char*)0)
operator-(const char16_t* aX, const char16ptr_t aY)
{
  return aX - static_cast<const char16_t*>(aY);
}

#else

typedef const char16_t* char16ptr_t;

#endif

static_assert(sizeof(char16_t) == 2, "Is char16_t type 16 bits?");
static_assert(char16_t(-1) > char16_t(0), "Is char16_t type unsigned?");
static_assert(sizeof(u'A') == 2, "Is unicode char literal 16 bits?");
static_assert(sizeof(u""[0]) == 2, "Is unicode string char 16 bits?");

#endif

#endif /* mozilla_Char16_h */
