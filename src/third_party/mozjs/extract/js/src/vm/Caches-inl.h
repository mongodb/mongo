/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Caches_inl_h
#define vm_Caches_inl_h

#include "vm/Caches.h"

#include <iterator>

#include "gc/Allocator.h"
#include "gc/GCProbes.h"
#include "vm/Probes.h"
#include "vm/Realm.h"

#include "vm/JSObject-inl.h"

namespace js {

inline bool NewObjectCache::lookupProto(const JSClass* clasp, JSObject* proto,
                                        gc::AllocKind kind,
                                        EntryIndex* pentry) {
  MOZ_ASSERT(!proto->is<GlobalObject>());
  return lookup(clasp, proto, kind, pentry);
}

inline bool NewObjectCache::lookupGlobal(const JSClass* clasp,
                                         GlobalObject* global,
                                         gc::AllocKind kind,
                                         EntryIndex* pentry) {
  return lookup(clasp, global, kind, pentry);
}

inline void NewObjectCache::fillGlobal(EntryIndex entry, const JSClass* clasp,
                                       GlobalObject* global, gc::AllocKind kind,
                                       NativeObject* obj) {
  // MOZ_ASSERT(global == obj->getGlobal());
  return fill(entry, clasp, global, kind, obj);
}

inline NativeObject* NewObjectCache::newObjectFromHit(JSContext* cx,
                                                      EntryIndex entryIndex,
                                                      gc::InitialHeap heap,
                                                      gc::AllocSite* site) {
  MOZ_ASSERT(unsigned(entryIndex) < std::size(entries));
  Entry* entry = &entries[entryIndex];

  NativeObject* templateObj =
      reinterpret_cast<NativeObject*>(&entry->templateObject);

  // If we did the lookup based on the proto we might have a shape/object from a
  // different (same-compartment) realm, so we have to do a realm check.
  if (templateObj->shape()->realm() != cx->realm()) {
    return nullptr;
  }

  if (cx->runtime()->gc.upcomingZealousGC()) {
    return nullptr;
  }

  const JSClass* clasp = templateObj->getClass();
  NativeObject* obj = static_cast<NativeObject*>(AllocateObject<NoGC>(
      cx, entry->kind, /* nDynamicSlots = */ 0, heap, clasp, site));
  if (!obj) {
    return nullptr;
  }

  copyCachedToObject(obj, templateObj, entry->kind);

  if (clasp->shouldDelayMetadataBuilder()) {
    cx->realm()->setObjectPendingMetadata(cx, obj);
  } else {
    obj = static_cast<NativeObject*>(SetNewObjectMetadata(cx, obj));
  }

  probes::CreateObject(cx, obj);
  gc::gcprobes::CreateObject(obj);
  return obj;
}

} /* namespace js */

#endif /* vm_Caches_inl_h */
