/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Caches_h
#define vm_Caches_h

#include <iterator>
#include <new>

#include "frontend/SourceNotes.h"  // SrcNote
#include "gc/Tracer.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "util/Memory.h"
#include "vm/ArrayObject.h"
#include "vm/JSAtom.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/NativeObject.h"

namespace js {

/*
 * GetSrcNote cache to avoid O(n^2) growth in finding a source note for a
 * given pc in a script. We use the script->code pointer to tag the cache,
 * instead of the script address itself, so that source notes are always found
 * by offset from the bytecode with which they were generated.
 */
struct GSNCache {
  typedef HashMap<jsbytecode*, const SrcNote*, PointerHasher<jsbytecode*>,
                  SystemAllocPolicy>
      Map;

  jsbytecode* code;
  Map map;

  GSNCache() : code(nullptr) {}

  void purge();
};

struct EvalCacheEntry {
  JSLinearString* str;
  JSScript* script;
  JSScript* callerScript;
  jsbytecode* pc;

  // We sweep this cache before a nursery collection to remove entries with
  // string keys in the nursery.
  //
  // The entire cache is purged on a major GC, so we don't need to sweep it
  // then.
  bool needsSweep() { return !str->isTenured(); }
};

struct EvalCacheLookup {
  explicit EvalCacheLookup(JSContext* cx) : str(cx), callerScript(cx) {}
  RootedLinearString str;
  RootedScript callerScript;
  MOZ_INIT_OUTSIDE_CTOR jsbytecode* pc;
};

struct EvalCacheHashPolicy {
  using Lookup = EvalCacheLookup;

  static HashNumber hash(const Lookup& l);
  static bool match(const EvalCacheEntry& entry, const EvalCacheLookup& l);
};

typedef GCHashSet<EvalCacheEntry, EvalCacheHashPolicy, SystemAllocPolicy>
    EvalCache;

/*
 * Cache for speeding up repetitive creation of objects in the VM.
 * When an object is created which matches the criteria in the 'key' section
 * below, an entry is filled with the resulting object.
 */
class NewObjectCache {
  static constexpr unsigned MAX_OBJ_SIZE = sizeof(JSObject_Slots16);

  static void staticAsserts() {
    static_assert(gc::AllocKind::OBJECT_LAST ==
                  gc::AllocKind::OBJECT16_BACKGROUND);
  }

  struct Entry {
    /* Class of the constructed object. */
    const JSClass* clasp;

    /*
     * Key with one of two possible values:
     *
     * - Global for the object. The object must have a standard class and will
     *   have this global's builtin prototype for this class as proto.
     *
     * - Prototype for the object (non-null). Cannot be a global object because
     *   that would be ambiguous (see previous case).
     */
    JSObject* key;

    /* Allocation kind for the constructed object. */
    gc::AllocKind kind;

    /* Number of bytes to copy from the template object. */
    uint32_t nbytes;

    /*
     * Template object to copy from, with the initial values of fields,
     * fixed slots (undefined) and private data (nullptr).
     */
    char templateObject[MAX_OBJ_SIZE];
  };

  using EntryArray = Entry[41];  // TODO: reconsider size;
  EntryArray entries;

 public:
  using EntryIndex = int;

  NewObjectCache()
      : entries{}  // zeroes out the array
  {}

  void purge() {
    new (&entries) EntryArray{};  // zeroes out the array
  }

  /* Remove any cached items keyed on moved objects. */
  void clearNurseryObjects(JSRuntime* rt);

  /*
   * Get the entry index for the given lookup, return whether there was a hit
   * on an existing entry.
   */
  inline bool lookupProto(const JSClass* clasp, JSObject* proto,
                          gc::AllocKind kind, EntryIndex* pentry);
  inline bool lookupGlobal(const JSClass* clasp, js::GlobalObject* global,
                           gc::AllocKind kind, EntryIndex* pentry);

  /*
   * Return a new object from a cache hit produced by a lookup method, or
   * nullptr if returning the object could possibly trigger GC (does not
   * indicate failure).
   */
  inline NativeObject* newObjectFromHit(JSContext* cx, EntryIndex entry,
                                        js::gc::InitialHeap heap,
                                        gc::AllocSite* site = nullptr);

  /* Fill an entry after a cache miss. */
  void fillProto(EntryIndex entry, const JSClass* clasp, js::TaggedProto proto,
                 gc::AllocKind kind, NativeObject* obj);

  inline void fillGlobal(EntryIndex entry, const JSClass* clasp,
                         js::GlobalObject* global, gc::AllocKind kind,
                         NativeObject* obj);

  /* Invalidate any entries which might produce an object with shape. */
  void invalidateEntriesForShape(Shape* shape);

 private:
  EntryIndex makeIndex(const JSClass* clasp, gc::Cell* key,
                       gc::AllocKind kind) {
    uintptr_t hash = (uintptr_t(clasp) ^ uintptr_t(key)) + size_t(kind);
    return hash % std::size(entries);
  }

  bool lookup(const JSClass* clasp, JSObject* key, gc::AllocKind kind,
              EntryIndex* pentry) {
    *pentry = makeIndex(clasp, key, kind);
    Entry* entry = &entries[*pentry];

    // N.B. Lookups with the same clasp/key but different kinds map to
    // different entries.
    return entry->clasp == clasp && entry->key == key;
  }

  void fill(EntryIndex entry_, const JSClass* clasp, JSObject* key,
            gc::AllocKind kind, NativeObject* obj) {
    MOZ_ASSERT(unsigned(entry_) < std::size(entries));
    MOZ_ASSERT(entry_ == makeIndex(clasp, key, kind));
    Entry* entry = &entries[entry_];

    MOZ_ASSERT(!obj->hasDynamicSlots());
    MOZ_ASSERT(obj->hasEmptyElements() || obj->is<ArrayObject>());

    entry->clasp = clasp;
    entry->key = key;
    entry->kind = kind;

    entry->nbytes = gc::Arena::thingSize(kind);
    js_memcpy(&entry->templateObject, obj, entry->nbytes);
  }

  static void copyCachedToObject(NativeObject* dst, NativeObject* src,
                                 gc::AllocKind kind) {
    js_memcpy(dst, src, gc::Arena::thingSize(kind));

    // Initialize with barriers
    dst->initShape(src->shape());
  }
};

// Cache for AtomizeString, mapping JSLinearString* to the corresponding
// JSAtom*. Also used by nursery GC to de-duplicate strings to atoms.
// Purged on minor and major GC.
class StringToAtomCache {
  using Map = HashMap<JSLinearString*, JSAtom*, PointerHasher<JSLinearString*>,
                      SystemAllocPolicy>;
  Map map_;

 public:
  // Don't use the cache for short strings. Hashing them is less expensive.
  static constexpr size_t MinStringLength = 30;

  JSAtom* lookup(JSLinearString* s) {
    MOZ_ASSERT(!s->isAtom());
    if (!s->inStringToAtomCache()) {
      MOZ_ASSERT(!map_.lookup(s));
      return nullptr;
    }

    MOZ_ASSERT(s->length() >= MinStringLength);

    auto p = map_.lookup(s);
    JSAtom* atom = p ? p->value() : nullptr;
    MOZ_ASSERT_IF(atom, EqualStrings(s, atom));
    return atom;
  }

  void maybePut(JSLinearString* s, JSAtom* atom) {
    MOZ_ASSERT(!s->isAtom());
    if (s->length() < MinStringLength) {
      return;
    }
    if (!map_.putNew(s, atom)) {
      return;
    }
    s->setInStringToAtomCache();
  }

  void purge() { map_.clearAndCompact(); }
};

class RuntimeCaches {
 public:
  js::GSNCache gsnCache;
  js::NewObjectCache newObjectCache;
  js::UncompressedSourceCache uncompressedSourceCache;
  js::EvalCache evalCache;
  js::StringToAtomCache stringToAtomCache;

  void purgeForMinorGC(JSRuntime* rt) {
    newObjectCache.clearNurseryObjects(rt);
    evalCache.sweep();
  }

  void purgeForCompaction() {
    newObjectCache.purge();
    evalCache.clear();
    stringToAtomCache.purge();
  }

  void purge() {
    purgeForCompaction();
    gsnCache.purge();
    uncompressedSourceCache.purge();
  }
};

}  // namespace js

#endif /* vm_Caches_h */
