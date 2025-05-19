/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/AutoProfilerLabel.h"

#include "mozilla/Assertions.h"
#include "mozilla/PlatformMutex.h"

namespace mozilla {

// RAII class that encapsulates all shared static data, and enforces locking
// when accessing this data.
class MOZ_RAII AutoProfilerLabelData {
 public:
  AutoProfilerLabelData() { sAPLMutex.Lock(); }

  ~AutoProfilerLabelData() { sAPLMutex.Unlock(); }

  AutoProfilerLabelData(const AutoProfilerLabelData&) = delete;
  void operator=(const AutoProfilerLabelData&) = delete;

  const ProfilerLabelEnter& EnterCRef() const { return sEnter; }
  ProfilerLabelEnter& EnterRef() { return sEnter; }

  const ProfilerLabelExit& ExitCRef() const { return sExit; }
  ProfilerLabelExit& ExitRef() { return sExit; }

  const uint32_t& GenerationCRef() const { return sGeneration; }
  uint32_t& GenerationRef() { return sGeneration; }

  static bool RacyIsProfilerPresent() { return !!sGeneration; }

 private:
  // Thin shell around mozglue PlatformMutex, for local internal use.
  // Does not preserve behavior in JS record/replay.
  class Mutex : private mozilla::detail::MutexImpl {
   public:
    Mutex() = default;
    void Lock() { mozilla::detail::MutexImpl::lock(); }
    void Unlock() { mozilla::detail::MutexImpl::unlock(); }
  };

  // Mutex protecting access to the following static members.
  static Mutex sAPLMutex MOZ_UNANNOTATED;

  static ProfilerLabelEnter sEnter;
  static ProfilerLabelExit sExit;

  // Current "generation" of RegisterProfilerLabelEnterExit calls.
  static uint32_t sGeneration;
};

/* static */ AutoProfilerLabelData::Mutex AutoProfilerLabelData::sAPLMutex;
/* static */ ProfilerLabelEnter AutoProfilerLabelData::sEnter = nullptr;
/* static */ ProfilerLabelExit AutoProfilerLabelData::sExit = nullptr;
/* static */ uint32_t AutoProfilerLabelData::sGeneration = 0;

void RegisterProfilerLabelEnterExit(ProfilerLabelEnter aEnter,
                                    ProfilerLabelExit aExit) {
  MOZ_ASSERT(!aEnter == !aExit, "Must provide both null or both non-null");

  AutoProfilerLabelData data;
  MOZ_ASSERT(!aEnter != !data.EnterRef(),
             "Must go from null to non-null, or from non-null to null");
  data.EnterRef() = aEnter;
  data.ExitRef() = aExit;
  ++data.GenerationRef();
}

bool IsProfilerPresent() {
  return AutoProfilerLabelData::RacyIsProfilerPresent();
}

ProfilerLabel ProfilerLabelBegin(const char* aLabelName,
                                 const char* aDynamicString, void* aSp) {
  const AutoProfilerLabelData data;
  void* entryContext = (data.EnterCRef())
                           ? data.EnterCRef()(aLabelName, aDynamicString, aSp)
                           : nullptr;
  uint32_t generation = data.GenerationCRef();

  return std::make_tuple(entryContext, generation);
}

void ProfilerLabelEnd(const ProfilerLabel& aLabel) {
  if (!IsValidProfilerLabel(aLabel)) {
    return;
  }

  const AutoProfilerLabelData data;
  if (data.ExitCRef() && (std::get<1>(aLabel) == data.GenerationCRef())) {
    data.ExitCRef()(std::get<0>(aLabel));
  }
}

AutoProfilerLabel::AutoProfilerLabel(const char* aLabel,
                                     const char* aDynamicString) {
  std::tie(mEntryContext, mGeneration) =
      ProfilerLabelBegin(aLabel, aDynamicString, this);
}

AutoProfilerLabel::~AutoProfilerLabel() {
  ProfilerLabelEnd(std::make_tuple(mEntryContext, mGeneration));
}

}  // namespace mozilla
