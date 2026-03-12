/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Uptime.h"

#include "mozilla/Now.h"

using namespace mozilla;

namespace {

static Maybe<uint64_t> mStartExcludingSuspendMs;
static Maybe<uint64_t> mStartIncludingSuspendMs;

};  // anonymous namespace

namespace mozilla {

void InitializeUptime() {
  MOZ_RELEASE_ASSERT(mStartIncludingSuspendMs.isNothing() &&
                         mStartExcludingSuspendMs.isNothing(),
                     "Must not be called more than once");
  mStartIncludingSuspendMs = NowIncludingSuspendMs();
  mStartExcludingSuspendMs = NowExcludingSuspendMs();
}

Maybe<uint64_t> ProcessUptimeMs() {
  if (!mStartIncludingSuspendMs) {
    return Nothing();
  }
  Maybe<uint64_t> maybeNow = NowIncludingSuspendMs();
  if (!maybeNow) {
    return Nothing();
  }
  return Some(maybeNow.value() - mStartIncludingSuspendMs.value());
}

Maybe<uint64_t> ProcessUptimeExcludingSuspendMs() {
  if (!mStartExcludingSuspendMs) {
    return Nothing();
  }
  Maybe<uint64_t> maybeNow = NowExcludingSuspendMs();
  if (!maybeNow) {
    return Nothing();
  }
  return Some(maybeNow.value() - mStartExcludingSuspendMs.value());
}

};  // namespace mozilla
