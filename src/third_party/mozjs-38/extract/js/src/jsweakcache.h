/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsweakcache_h
#define jsweakcache_h

#include "jscntxt.h"
#include "gc/Marking.h"
#include "js/HashTable.h"
#include "vm/Runtime.h"

namespace js {

// A WeakCache is used to map a key to a value similar to an HashMap except
// that its entries are garbage collected.  An entry is kept as long as
// both the key and value are marked.
//
// No mark function is provided with this weak container.  However, this weak
// container should take part in the sweep phase.
template <class Key, class Value,
          class HashPolicy = DefaultHasher<Key>,
          class AllocPolicy = RuntimeAllocPolicy>
class WeakCache : public HashMap<Key, Value, HashPolicy, AllocPolicy> {
  private:
    typedef HashMap<Key, Value, HashPolicy, AllocPolicy> Base;
    typedef typename Base::Range Range;
    typedef typename Base::Enum Enum;

  public:
    explicit WeakCache(JSRuntime* rt) : Base(rt) { }
    explicit WeakCache(JSContext* cx) : Base(cx->runtime()) { }

  public:
    // Sweep all entries which have unmarked key or value.
    void sweep(FreeOp* fop) {
        // Remove all entries whose keys/values remain unmarked.
        for (Enum e(*this); !e.empty(); e.popFront()) {
            // Checking IsMarked() may update the location of the Key (or Value).
            // Pass in a stack local, then manually update the backing heap store.
            Key k(e.front().key);
            bool isKeyDying = gc::IsAboutToBeFinalized(&k);

            if (isKeyDying || gc::IsAboutToBeFinalized(e.front().value)) {
                e.removeFront();
            } else {
                // Potentially update the location of the Key.
                // The Value had its heap addresses correctly passed to IsMarked(),
                // and therefore has already been updated if necessary.
                // e.rekeyFront(k);
            }
        }

#if DEBUG
        // Once we've swept, all remaining edges should stay within the
        // known-live part of the graph.
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            Key k(r.front().key);

            MOZ_ASSERT(!gc::IsAboutToBeFinalized(&k));
            MOZ_ASSERT(!gc::IsAboutToBeFinalized(r.front().value));

            // Assert that IsMarked() did not perform relocation.
            MOZ_ASSERT(k == r.front().key);
        }
#endif
    }
};

// A WeakValueCache is similar to a WeakCache, except keys are never marked.
// This is useful for weak maps where the keys are primitive values such as uint32_t.
template <class Key, class Value,
          class HashPolicy = DefaultHasher<Key>,
          class AllocPolicy = RuntimeAllocPolicy>
class WeakValueCache : public HashMap<Key, Value, HashPolicy, AllocPolicy>
{
  public:
    typedef HashMap<Key, Value, HashPolicy, AllocPolicy> Base;
    typedef typename Base::Range Range;
    typedef typename Base::Enum Enum;

    explicit WeakValueCache(JSRuntime* rt) : Base(rt) { }
    explicit WeakValueCache(JSContext* cx) : Base(cx->runtime()) { }

  public:
    // Sweep all entries which have unmarked key or value.
    void sweep(FreeOp* fop) {
        // Remove all entries whose values remain unmarked.
        for (Enum e(*this); !e.empty(); e.popFront()) {
            if (gc::IsAboutToBeFinalized(e.front().value()))
                e.removeFront();
        }

#if DEBUG
        // Once we've swept, all remaining edges should stay within the
        // known-live part of the graph.
        for (Range r = Base::all(); !r.empty(); r.popFront())
            MOZ_ASSERT(!gc::IsAboutToBeFinalized(r.front().value()));
#endif
    }
};

} // namespace js

#endif /* jsweakcache_h */
