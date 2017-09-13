/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Compatibility with std::numeric_limits<char16_t>. */

#ifndef mozilla_NumericLimits_h
#define mozilla_NumericLimits_h

#include "mozilla/Char16.h"

#include <limits>
#include <stdint.h>

namespace mozilla {

/**
 * The NumericLimits class provides a compatibility layer with
 * std::numeric_limits for char16_t, otherwise it is exactly the same as
 * std::numeric_limits.  Code which does not need std::numeric_limits<char16_t>
 * should avoid using NumericLimits.
 */
template<typename T>
class NumericLimits : public std::numeric_limits<T>
{
};

#ifdef MOZ_CHAR16_IS_NOT_WCHAR
template<>
class NumericLimits<char16_t> : public std::numeric_limits<uint16_t>
{
  // char16_t and uint16_t numeric limits should be exactly the same.
};
#endif

} // namespace mozilla

#endif /* mozilla_NumericLimits_h */
