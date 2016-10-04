/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Small helper class for asserting uses of a class are non-reentrant. */

#ifndef mozilla_ReentrancyGuard_h
#define mozilla_ReentrancyGuard_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"

namespace mozilla {

/* Useful for implementing containers that assert non-reentrancy */
class MOZ_RAII ReentrancyGuard
{
  MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
#ifdef DEBUG
  bool& mEntered;
#endif

public:
  template<class T>
#ifdef DEBUG
  explicit ReentrancyGuard(T& aObj
                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    : mEntered(aObj.mEntered)
#else
  explicit ReentrancyGuard(T&
                           MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
#endif
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
#ifdef DEBUG
    MOZ_ASSERT(!mEntered);
    mEntered = true;
#endif
  }
  ~ReentrancyGuard()
  {
#ifdef DEBUG
    mEntered = false;
#endif
  }

private:
  ReentrancyGuard(const ReentrancyGuard&) = delete;
  void operator=(const ReentrancyGuard&) = delete;
};

} // namespace mozilla

#endif /* mozilla_ReentrancyGuard_h */
