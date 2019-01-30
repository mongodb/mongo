/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_HashUtil_h
#define gc_HashUtil_h

#include "gc/Zone.h"
#include "vm/JSContext.h"

namespace js {

/*
 * Used to add entries to a js::HashMap or HashSet where the key depends on a GC
 * thing that may be moved by generational or compacting GC between the call to
 * lookupForAdd() and relookupOrAdd().
 */
template <class T>
struct DependentAddPtr
{
    typedef typename T::AddPtr AddPtr;
    typedef typename T::Entry Entry;

    template <class Lookup>
    DependentAddPtr(const JSContext* cx, const T& table, const Lookup& lookup)
      : addPtr(table.lookupForAdd(lookup))
      , originalGcNumber(cx->zone()->gcNumber())
    {}

    DependentAddPtr(DependentAddPtr&& other)
      : addPtr(other.addPtr)
      , originalGcNumber(other.originalGcNumber)
    {}

    template <class KeyInput, class ValueInput>
    bool add(JSContext* cx, T& table, const KeyInput& key, const ValueInput& value) {
        refreshAddPtr(cx, table, key);
        if (!table.relookupOrAdd(addPtr, key, value)) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    template <class KeyInput>
    void remove(JSContext* cx, T& table, const KeyInput& key) {
        refreshAddPtr(cx, table, key);
        table.remove(addPtr);
    }

    bool found() const                 { return addPtr.found(); }
    explicit operator bool() const     { return found(); }
    const Entry& operator*() const     { return *addPtr; }
    const Entry* operator->() const    { return &*addPtr; }

  private:
    AddPtr addPtr ;
    const uint64_t originalGcNumber;

    template <class KeyInput>
    void refreshAddPtr(JSContext* cx, T& table, const KeyInput& key) {
        bool gcHappened = originalGcNumber != cx->zone()->gcNumber();
        if (gcHappened)
            addPtr = table.lookupForAdd(key);
    }

    DependentAddPtr() = delete;
    DependentAddPtr(const DependentAddPtr&) = delete;
    DependentAddPtr& operator=(const DependentAddPtr&) = delete;
};

template <typename T, typename Lookup>
inline auto
MakeDependentAddPtr(const JSContext* cx, T& table, const Lookup& lookup)
  -> DependentAddPtr<typename mozilla::RemoveReference<decltype(table)>::Type>
{
    using Ptr = DependentAddPtr<typename mozilla::RemoveReference<decltype(table)>::Type>;
    return Ptr(cx, table, lookup);
}

} // namespace js

#endif
