/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/AutoProfilerLabel.h"

namespace mozilla {

static ProfilerLabelEnter sEnter = nullptr;
static ProfilerLabelExit sExit = nullptr;

void
RegisterProfilerLabelEnterExit(ProfilerLabelEnter aEnter,
                               ProfilerLabelExit aExit)
{
  sEnter = aEnter;
  sExit = aExit;
}

AutoProfilerLabel::AutoProfilerLabel(const char* aLabel,
                                     const char* aDynamicString,
                                     uint32_t aLine
                                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
{
  MOZ_GUARD_OBJECT_NOTIFIER_INIT;

  mPseudoStack = sEnter ? sEnter(aLabel, aDynamicString, this, aLine) : nullptr;
}

AutoProfilerLabel::~AutoProfilerLabel()
{
  if (sExit && mPseudoStack) {
    sExit(mPseudoStack);
  }
}

} // namespace mozilla

