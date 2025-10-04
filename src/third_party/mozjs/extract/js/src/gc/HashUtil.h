/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_HashUtil_h
#define gc_HashUtil_h

#include <type_traits>

#include "vm/JSContext.h"

namespace js {

/*
 * Used to add entries to a js::HashMap or HashSet where the key depends on a GC
 * thing that may be moved by generational or compacting GC between the call to
 * lookupForAdd() and relookupOrAdd().
 */
template <class T>
struct DependentAddPtr {
  using AddPtr = typename T::AddPtr;
  using Entry = typename T::Entry;

  template <class Lookup>
  DependentAddPtr(const JSContext* cx, T& table, const Lookup& lookup)
      : addPtr(table.lookupForAdd(lookup)),
        originalGcNumber(cx->runtime()->gc.gcNumber()) {}

  DependentAddPtr(DependentAddPtr&& other)
      : addPtr(other.addPtr), originalGcNumber(other.originalGcNumber) {}

  template <class KeyInput, class ValueInput>
  bool add(JSContext* cx, T& table, const KeyInput& key,
           const ValueInput& value) {
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
    if (addPtr) {
      table.remove(addPtr);
    }
  }

  bool found() const { return addPtr.found(); }
  explicit operator bool() const { return found(); }
  const Entry& operator*() const { return *addPtr; }
  const Entry* operator->() const { return &*addPtr; }

 private:
  AddPtr addPtr;
  const uint64_t originalGcNumber;

  template <class KeyInput>
  void refreshAddPtr(JSContext* cx, T& table, const KeyInput& key) {
    bool gcHappened = originalGcNumber != cx->runtime()->gc.gcNumber();
    if (gcHappened) {
      addPtr = table.lookupForAdd(key);
    }
  }

  DependentAddPtr() = delete;
  DependentAddPtr(const DependentAddPtr&) = delete;
  DependentAddPtr& operator=(const DependentAddPtr&) = delete;
};

template <typename T, typename Lookup>
inline auto MakeDependentAddPtr(const JSContext* cx, T& table,
                                const Lookup& lookup) {
  using Ptr = DependentAddPtr<std::remove_reference_t<decltype(table)>>;
  return Ptr(cx, table, lookup);
}

}  // namespace js

#endif
