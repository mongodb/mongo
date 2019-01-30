/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Utilities for converting an object to a string representation. */

#ifndef mozilla_ToString_h
#define mozilla_ToString_h

#include <string>
#include <sstream>

namespace mozilla {

/**
 * A convenience function for converting an object to a string representation.
 * Supports any object which can be streamed to an std::ostream.
 */
template<typename T>
std::string
ToString(const T& aValue)
{
  std::ostringstream stream;
  stream << aValue;
  return stream.str();
}

} // namespace mozilla

#endif /* mozilla_ToString_h */
