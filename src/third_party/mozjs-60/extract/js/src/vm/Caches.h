/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Caches_h
#define vm_Caches_h

#include "jsmath.h"

#include "frontend/SourceNotes.h"
#include "gc/Tracer.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
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
    typedef HashMap<jsbytecode*,
                    jssrcnote*,
                    PointerHasher<jsbytecode*>,
                    SystemAllocPolicy> Map;

    jsbytecode*     code;
    Map             map;

    GSNCache() : code(nullptr) { }

    void purge();
};

/*
 * EnvironmentCoordinateName cache to avoid O(n^2) growth in finding the name
 * associated with a given aliasedvar operation.
 */
struct EnvironmentCoordinateNameCache {
    typedef HashMap<uint32_t,
                    jsid,
                    DefaultHasher<uint32_t>,
                    SystemAllocPolicy> Map;

    Shape* shape;
    Map map;

    EnvironmentCoordinateNameCache() : shape(nullptr) {}
    void purge();
};

struct EvalCacheEntry
{
    JSLinearString* str;
    JSScript* script;
    JSScript* callerScript;
    jsbytecode* pc;

    // We sweep this cache before a nursery collection to remove entries with
    // string keys in the nursery.
    //
    // The entire cache is purged on a major GC, so we don't need to sweep it
    // then.
    bool needsSweep() {
        return !str->isTenured();
    }
};

struct EvalCacheLookup
{
    explicit EvalCacheLookup(JSContext* cx) : str(cx), callerScript(cx) {}
    RootedLinearString str;
    RootedScript callerScript;
    jsbytecode* pc;
};

struct EvalCacheHashPolicy
{
    typedef EvalCacheLookup Lookup;

    static HashNumber hash(const Lookup& l);
    static bool match(const EvalCacheEntry& entry, const EvalCacheLookup& l);
};

typedef GCHashSet<EvalCacheEntry, EvalCacheHashPolicy, SystemAllocPolicy> EvalCache;

/*
 * Cache for speeding up repetitive creation of objects in the VM.
 * When an object is created which matches the criteria in the 'key' section
 * below, an entry is filled with the resulting object.
 */
class NewObjectCache
{
    /* Statically asserted to be equal to sizeof(JSObject_Slots16) */
    static const unsigned MAX_OBJ_SIZE = 4 * sizeof(void*) + 16 * sizeof(Value);

    static void staticAsserts() {
        JS_STATIC_ASSERT(NewObjectCache::MAX_OBJ_SIZE == sizeof(JSObject_Slots16));
        JS_STATIC_ASSERT(gc::AllocKind::OBJECT_LAST == gc::AllocKind::OBJECT16_BACKGROUND);
    }

    struct Entry
    {
        /* Class of the constructed object. */
        const Class* clasp;

        /*
         * Key with one of three possible values:
         *
         * - Global for the object. The object must have a standard class for
         *   which the global's prototype can be determined, and the object's
         *   parent will be the global.
         *
         * - Prototype for the object (cannot be global). The object's parent
         *   will be the prototype's parent.
         *
         * - Type for the object. The object's parent will be the type's
         *   prototype's parent.
         */
        gc::Cell* key;

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

    Entry entries[41];  // TODO: reconsider size

  public:

    typedef int EntryIndex;

    NewObjectCache() { mozilla::PodZero(this); }
    void purge() { mozilla::PodZero(this); }

    /* Remove any cached items keyed on moved objects. */
    void clearNurseryObjects(JSRuntime* rt);

    /*
     * Get the entry index for the given lookup, return whether there was a hit
     * on an existing entry.
     */
    inline bool lookupProto(const Class* clasp, JSObject* proto, gc::AllocKind kind, EntryIndex* pentry);
    inline bool lookupGlobal(const Class* clasp, js::GlobalObject* global, gc::AllocKind kind,
                             EntryIndex* pentry);

    bool lookupGroup(js::ObjectGroup* group, gc::AllocKind kind, EntryIndex* pentry) {
        return lookup(group->clasp(), group, kind, pentry);
    }

    /*
     * Return a new object from a cache hit produced by a lookup method, or
     * nullptr if returning the object could possibly trigger GC (does not
     * indicate failure).
     */
    inline NativeObject* newObjectFromHit(JSContext* cx, EntryIndex entry, js::gc::InitialHeap heap);

    /* Fill an entry after a cache miss. */
    void fillProto(EntryIndex entry, const Class* clasp, js::TaggedProto proto,
                   gc::AllocKind kind, NativeObject* obj);

    inline void fillGlobal(EntryIndex entry, const Class* clasp, js::GlobalObject* global,
                           gc::AllocKind kind, NativeObject* obj);

    void fillGroup(EntryIndex entry, js::ObjectGroup* group, gc::AllocKind kind,
                   NativeObject* obj)
    {
        MOZ_ASSERT(obj->group() == group);
        return fill(entry, group->clasp(), group, kind, obj);
    }

    /* Invalidate any entries which might produce an object with shape/proto. */
    void invalidateEntriesForShape(JSContext* cx, HandleShape shape, HandleObject proto);

  private:
    EntryIndex makeIndex(const Class* clasp, gc::Cell* key, gc::AllocKind kind) {
        uintptr_t hash = (uintptr_t(clasp) ^ uintptr_t(key)) + size_t(kind);
        return hash % mozilla::ArrayLength(entries);
    }

    bool lookup(const Class* clasp, gc::Cell* key, gc::AllocKind kind, EntryIndex* pentry) {
        *pentry = makeIndex(clasp, key, kind);
        Entry* entry = &entries[*pentry];

        /* N.B. Lookups with the same clasp/key but different kinds map to different entries. */
        return entry->clasp == clasp && entry->key == key;
    }

    void fill(EntryIndex entry_, const Class* clasp, gc::Cell* key, gc::AllocKind kind,
              NativeObject* obj) {
        MOZ_ASSERT(unsigned(entry_) < mozilla::ArrayLength(entries));
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

    static void copyCachedToObject(NativeObject* dst, NativeObject* src, gc::AllocKind kind) {
        js_memcpy(dst, src, gc::Arena::thingSize(kind));

        // Initialize with barriers
        dst->initGroup(src->group());
        dst->initShape(src->shape());
    }
};

class RuntimeCaches
{
    UniquePtr<js::MathCache> mathCache_;

    js::MathCache* createMathCache(JSContext* cx);

  public:
    js::GSNCache gsnCache;
    js::EnvironmentCoordinateNameCache envCoordinateNameCache;
    js::NewObjectCache newObjectCache;
    js::UncompressedSourceCache uncompressedSourceCache;
    js::EvalCache evalCache;

    bool init();

    js::MathCache* getMathCache(JSContext* cx) {
        return mathCache_ ? mathCache_.get() : createMathCache(cx);
    }
    js::MathCache* maybeGetMathCache() {
        return mathCache_.get();
    }

    void purgeForMinorGC(JSRuntime* rt) {
        newObjectCache.clearNurseryObjects(rt);
        evalCache.sweep();
    }

    void purgeForCompaction() {
        newObjectCache.purge();
        if (evalCache.initialized())
            evalCache.clear();
    }

    void purge() {
        purgeForCompaction();
        gsnCache.purge();
        envCoordinateNameCache.purge();
        uncompressedSourceCache.purge();
    }
};

} // namespace js

#endif /* vm_Caches_h */
