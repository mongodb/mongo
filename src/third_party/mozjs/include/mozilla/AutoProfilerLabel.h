/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AutoProfilerLabel_h
#define mozilla_AutoProfilerLabel_h

#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/Types.h"

// The Gecko Profiler defines AutoProfilerLabel, an RAII class for
// pushing/popping entries to/from the PseudoStack.
//
// This file defines a class of the same name that does much the same thing,
// but which can be used in (and only in) mozglue. A different class is
// necessary because mozglue cannot directly access sPseudoStack.
//
// Note that this class is slightly slower than the other AutoProfilerLabel,
// and it lacks the macro wrappers. It also is effectively hardwired to use
// js::ProfileEntry::Kind::CPP_NORMAL as the kind, and
// js::ProfileEntry::Category::OTHER as the category, because that's what the
// callbacks provided by the profiler use. (Specifying the kind or category in
// this file would require #including ProfilingStack.h in mozglue, which we
// don't want to do.)

class PseudoStack;

namespace mozilla {

typedef PseudoStack* (*ProfilerLabelEnter)(const char*, const char*, void*,
                                           uint32_t);
typedef void (*ProfilerLabelExit)(PseudoStack*);

// Register callbacks that do the entry/exit work involving sPseudoStack.
MFBT_API void RegisterProfilerLabelEnterExit(ProfilerLabelEnter aEnter,
                                             ProfilerLabelExit aExit);

// This #ifdef prevents this AutoProfilerLabel from being defined in libxul,
// which would conflict with the one in the profiler.
#ifdef IMPL_MFBT

class MOZ_RAII AutoProfilerLabel
{
public:
  AutoProfilerLabel(const char* aLabel, const char* aDynamicString,
                    uint32_t aLine
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
  ~AutoProfilerLabel();

private:
  MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
  PseudoStack* mPseudoStack;
};

#endif

} // namespace mozilla

#endif // mozilla_AutoProfilerLabel_h
