/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Now_h
#define mozilla_Now_h

#include <stdint.h>

#include "mozilla/Maybe.h"

namespace mozilla {

// Returns monotonic milliseconds elapsed since an arbitrary reference point,
// excluding time when the system was suspended.
MFBT_API Maybe<uint64_t> NowExcludingSuspendMs();
// Returns monotonic milliseconds elapsed since an arbitrary reference point,
// including time when the system was suspended.
MFBT_API Maybe<uint64_t> NowIncludingSuspendMs();

};  // namespace mozilla

#endif  // mozilla_Now_h
