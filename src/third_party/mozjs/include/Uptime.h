/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Uptime_h
#define mozilla_Uptime_h

#include <stdint.h>

#include "mozilla/Maybe.h"

namespace mozilla {

// Called at the beginning of the process from TimeStamp::Startup.
MFBT_API void InitializeUptime();
// Returns the number of milliseconds the calling process has lived for.
MFBT_API Maybe<uint64_t> ProcessUptimeMs();
// Returns the number of milliseconds the calling process has lived for,
// excluding the time period the system was suspended.
MFBT_API Maybe<uint64_t> ProcessUptimeExcludingSuspendMs();

};  // namespace mozilla

#endif  // mozilla_Uptime_h
