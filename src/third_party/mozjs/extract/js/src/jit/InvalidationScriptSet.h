/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InvalidationScriptSet_h
#define jit_InvalidationScriptSet_h

#include "gc/Barrier.h"
#include "jit/IonTypes.h"
#include "js/AllocPolicy.h"
#include "js/GCVector.h"
#include "js/SweepingAPI.h"

class JSScript;

namespace js::jit {

using WeakScriptSet =
    GCHashSet<WeakHeapPtr<JSScript*>, StableCellHasher<WeakHeapPtr<JSScript*>>,
              js::SystemAllocPolicy>;

// A weak cache of scripts all of which will be invalidated simultaneously.
using WeakScriptCache = JS::WeakCache<WeakScriptSet>;

void InvalidateAndClearScriptSet(JSContext* cx, WeakScriptCache& scripts,
                                 const char* reason);
bool AddScriptToSet(WeakScriptCache& scripts, Handle<JSScript*> script);

// Remove a script from a script set if found.
void RemoveFromScriptSet(WeakScriptCache& scripts, JSScript* script);

}  // namespace js::jit

#endif /* jit_InvalidationScriptSet_h */
