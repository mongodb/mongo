/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitHints_inl_h
#define jit_JitHints_inl_h

#include "jit/JitHints.h"
#include "mozilla/HashFunctions.h"

namespace js::jit {

inline JitHintsMap::ScriptKey JitHintsMap::getScriptKey(
    JSScript* script) const {
  if (ScriptKey key = script->filenameHash()) {
    return mozilla::AddToHash(key, script->sourceStart());
  }
  return 0;
}

inline void JitHintsMap::incrementEntryCount() {
  // Clear the cache if we've exceeded the false positivity rate
  // calculated by MaxEntries.
  if (++entryCount_ > MaxEntries_) {
    map_.clear();
    entryCount_ = 0;
  }
}

inline void JitHintsMap::setEagerBaselineHint(JSScript* script) {
  ScriptKey key = getScriptKey(script);
  if (!key) {
    return;
  }

  // If the entry already exists, don't increment entryCount.
  if (map_.mightContain(key)) {
    return;
  }

  // Increment entry count, and possibly clear the cache.
  incrementEntryCount();

  script->setNoEagerBaselineHint(false);
  map_.add(key);
}

inline bool JitHintsMap::mightHaveEagerBaselineHint(JSScript* script) const {
  if (ScriptKey key = getScriptKey(script)) {
    return map_.mightContain(key);
  }
  script->setNoEagerBaselineHint(true);
  return false;
}

}  // namespace js::jit

#endif /* jit_JitHints_inl_h */
