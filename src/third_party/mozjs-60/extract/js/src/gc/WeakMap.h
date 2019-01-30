/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_WeakMap_h
#define gc_WeakMap_h

#include "mozilla/LinkedList.h"
#include "mozilla/Move.h"

#include "jsfriendapi.h"

#include "gc/DeletePolicy.h"
#include "gc/StoreBuffer.h"
#include "js/HashTable.h"
#include "vm/JSCompartment.h"
#include "vm/JSObject.h"

namespace js {

class GCMarker;
class WeakMapBase;

// A subclass template of js::HashMap whose keys and values may be garbage-collected. When
// a key is collected, the table entry disappears, dropping its reference to the value.
//
// More precisely:
//
//     A WeakMap entry is live if and only if both the WeakMap and the entry's key
//     are live. An entry holds a strong reference to its value.
//
// You must call this table's 'trace' method when its owning object is reached
// by the garbage collection tracer. Once a table is known to be live, the
// implementation takes care of the special weak marking (ie, marking through
// the implicit edges stored in the map) and of removing (sweeping) table
// entries when collection is complete.

typedef HashSet<WeakMapBase*, DefaultHasher<WeakMapBase*>, SystemAllocPolicy> WeakMapSet;

// Common base class for all WeakMap specializations, used for calling
// subclasses' GC-related methods.
class WeakMapBase : public mozilla::LinkedListElement<WeakMapBase>
{
    friend class js::GCMarker;

  public:
    WeakMapBase(JSObject* memOf, JS::Zone* zone);
    virtual ~WeakMapBase();

    Zone* zone() const { return zone_; }

    // Garbage collector entry points.

    // Unmark all weak maps in a zone.
    static void unmarkZone(JS::Zone* zone);

    // Mark all the weakmaps in a zone.
    static void traceZone(JS::Zone* zone, JSTracer* tracer);

    // Check all weak maps in a zone that have been marked as live in this garbage
    // collection, and mark the values of all entries that have become strong references
    // to them. Return true if we marked any new values, indicating that we need to make
    // another pass. In other words, mark my marked maps' marked members' mid-collection.
    static bool markZoneIteratively(JS::Zone* zone, GCMarker* marker);

    // Add zone edges for weakmaps with key delegates in a different zone.
    static bool findInterZoneEdges(JS::Zone* zone);

    // Sweep the weak maps in a zone, removing dead weak maps and removing
    // entries of live weak maps whose keys are dead.
    static void sweepZone(JS::Zone* zone);

    // Trace all delayed weak map bindings. Used by the cycle collector.
    static void traceAllMappings(WeakMapTracer* tracer);

    // Save information about which weak maps are marked for a zone.
    static bool saveZoneMarkedWeakMaps(JS::Zone* zone, WeakMapSet& markedWeakMaps);

    // Restore information about which weak maps are marked for many zones.
    static void restoreMarkedWeakMaps(WeakMapSet& markedWeakMaps);

  protected:
    // Instance member functions called by the above. Instantiations of WeakMap override
    // these with definitions appropriate for their Key and Value types.
    virtual void trace(JSTracer* tracer) = 0;
    virtual bool findZoneEdges() = 0;
    virtual void sweep() = 0;
    virtual void traceMappings(WeakMapTracer* tracer) = 0;
    virtual void finish() = 0;

    // Any weakmap key types that want to participate in the non-iterative
    // ephemeron marking must override this method.
    virtual void markEntry(GCMarker* marker, gc::Cell* markedCell, JS::GCCellPtr l) = 0;

    virtual bool markIteratively(GCMarker* marker) = 0;

  protected:
    // Object that this weak map is part of, if any.
    GCPtrObject memberOf;

    // Zone containing this weak map.
    JS::Zone* zone_;

    // Whether this object has been traced during garbage collection.
    bool marked;
};

template <typename T>
static T extractUnbarriered(const WriteBarrieredBase<T>& v)
{
    return v.get();
}
template <typename T>
static T* extractUnbarriered(T* v)
{
    return v;
}

template <class Key, class Value,
          class HashPolicy = DefaultHasher<Key> >
class WeakMap : public HashMap<Key, Value, HashPolicy, ZoneAllocPolicy>,
                public WeakMapBase
{
  public:
    typedef HashMap<Key, Value, HashPolicy, ZoneAllocPolicy> Base;
    typedef typename Base::Enum Enum;
    typedef typename Base::Lookup Lookup;
    typedef typename Base::Entry Entry;
    typedef typename Base::Range Range;
    typedef typename Base::Ptr Ptr;
    typedef typename Base::AddPtr AddPtr;

    explicit WeakMap(JSContext* cx, JSObject* memOf = nullptr)
        : Base(cx->zone()), WeakMapBase(memOf, cx->zone()) { }

    bool init(uint32_t len = 16) {
        if (!Base::init(len))
            return false;
        zone()->gcWeakMapList().insertFront(this);
        marked = JS::IsIncrementalGCInProgress(TlsContext.get());
        return true;
    }

    // Overwritten to add a read barrier to prevent an incorrectly gray value
    // from escaping the weak map. See the UnmarkGrayTracer::onChild comment in
    // gc/Marking.cpp.
    Ptr lookup(const Lookup& l) const {
        Ptr p = Base::lookup(l);
        if (p)
            exposeGCThingToActiveJS(p->value());
        return p;
    }

    AddPtr lookupForAdd(const Lookup& l) const {
        AddPtr p = Base::lookupForAdd(l);
        if (p)
            exposeGCThingToActiveJS(p->value());
        return p;
    }

    Ptr lookupWithDefault(const Key& k, const Value& defaultValue) {
        Ptr p = Base::lookupWithDefault(k, defaultValue);
        if (p)
            exposeGCThingToActiveJS(p->value());
        return p;
    }

    // Resolve ambiguity with LinkedListElement<>::remove.
    using Base::remove;

    // Trace a WeakMap entry based on 'markedCell' getting marked, where
    // 'origKey' is the key in the weakmap. These will probably be the same,
    // but can be different eg when markedCell is a delegate for origKey.
    //
    // This implementation does not use 'markedCell'; it looks up origKey and
    // checks the mark bits on everything it cares about, one of which will be
    // markedCell. But a subclass might use it to optimize the liveness check.
    void markEntry(GCMarker* marker, gc::Cell* markedCell, JS::GCCellPtr origKey) override
    {
        MOZ_ASSERT(marked);

        // If this cast fails, then you're instantiating the WeakMap with a
        // Lookup that can't be constructed from a Cell*. The WeakKeyTable
        // mechanism is indexed with a GCCellPtr, so that won't work.
        Ptr p = Base::lookup(static_cast<Lookup>(origKey.asCell()));
        MOZ_ASSERT(p.found());

        Key key(p->key());
        MOZ_ASSERT((markedCell == extractUnbarriered(key)) || (markedCell == getDelegate(key)));
        if (gc::IsMarked(marker->runtime(), &key)) {
            TraceEdge(marker, &p->value(), "ephemeron value");
        } else if (keyNeedsMark(key)) {
            TraceEdge(marker, &p->value(), "WeakMap ephemeron value");
            TraceEdge(marker, &key, "proxy-preserved WeakMap ephemeron key");
            MOZ_ASSERT(key == p->key()); // No moving
        }
        key.unsafeSet(nullptr); // Prevent destructor from running barriers.
    }

    void trace(JSTracer* trc) override {
        MOZ_ASSERT_IF(JS::CurrentThreadIsHeapBusy(), isInList());

        TraceNullableEdge(trc, &memberOf, "WeakMap owner");

        if (!Base::initialized())
            return;

        if (trc->isMarkingTracer()) {
            MOZ_ASSERT(trc->weakMapAction() == ExpandWeakMaps);
            marked = true;
            (void) markIteratively(GCMarker::fromTracer(trc));
            return;
        }

        if (trc->weakMapAction() == DoNotTraceWeakMaps)
            return;

        // Trace keys only if weakMapAction() says to.
        if (trc->weakMapAction() == TraceWeakMapKeysValues) {
            for (Enum e(*this); !e.empty(); e.popFront())
                TraceEdge(trc, &e.front().mutableKey(), "WeakMap entry key");
        }

        // Always trace all values (unless weakMapAction() is
        // DoNotTraceWeakMaps).
        for (Range r = Base::all(); !r.empty(); r.popFront())
            TraceEdge(trc, &r.front().value(), "WeakMap entry value");
    }

  protected:
    static void addWeakEntry(GCMarker* marker, JS::GCCellPtr key, gc::WeakMarkable markable)
    {
        Zone* zone = key.asCell()->asTenured().zone();

        auto p = zone->gcWeakKeys().get(key);
        if (p) {
            gc::WeakEntryVector& weakEntries = p->value;
            if (!weakEntries.append(Move(markable)))
                marker->abortLinearWeakMarking();
        } else {
            gc::WeakEntryVector weakEntries;
            MOZ_ALWAYS_TRUE(weakEntries.append(Move(markable)));
            if (!zone->gcWeakKeys().put(JS::GCCellPtr(key), Move(weakEntries)))
                marker->abortLinearWeakMarking();
        }
    }

    bool markIteratively(GCMarker* marker) override {
        MOZ_ASSERT(marked);

        bool markedAny = false;

        for (Enum e(*this); !e.empty(); e.popFront()) {
            // If the entry is live, ensure its key and value are marked.
            bool keyIsMarked = gc::IsMarked(marker->runtime(), &e.front().mutableKey());
            if (!keyIsMarked && keyNeedsMark(e.front().key())) {
                TraceEdge(marker, &e.front().mutableKey(), "proxy-preserved WeakMap entry key");
                keyIsMarked = true;
                markedAny = true;
            }

            if (keyIsMarked) {
                if (!gc::IsMarked(marker->runtime(), &e.front().value())) {
                    TraceEdge(marker, &e.front().value(), "WeakMap entry value");
                    markedAny = true;
                }
            } else if (marker->isWeakMarkingTracer()) {
                // Entry is not yet known to be live. Record this weakmap and
                // the lookup key in the list of weak keys. Also record the
                // delegate, if any, because marking the delegate also marks
                // the entry.
                JS::GCCellPtr weakKey(extractUnbarriered(e.front().key()));
                gc::WeakMarkable markable(this, weakKey);
                addWeakEntry(marker, weakKey, markable);
                if (JSObject* delegate = getDelegate(e.front().key()))
                    addWeakEntry(marker, JS::GCCellPtr(delegate), markable);
            }
        }

        return markedAny;
    }

    JSObject* getDelegate(JSObject* key) const {
        JSWeakmapKeyDelegateOp op = key->getClass()->extWeakmapKeyDelegateOp();
        if (!op)
            return nullptr;

        JSObject* obj = op(key);
        if (!obj)
            return nullptr;

        MOZ_ASSERT(obj->runtimeFromActiveCooperatingThread() == zone()->runtimeFromActiveCooperatingThread());
        return obj;
    }

    JSObject* getDelegate(JSScript* script) const {
        return nullptr;
    }

  private:
    void exposeGCThingToActiveJS(const JS::Value& v) const { JS::ExposeValueToActiveJS(v); }
    void exposeGCThingToActiveJS(JSObject* obj) const { JS::ExposeObjectToActiveJS(obj); }

    bool keyNeedsMark(JSObject* key) const {
        JSObject* delegate = getDelegate(key);
        /*
         * Check if the delegate is marked with any color to properly handle
         * gray marking when the key's delegate is black and the map is gray.
         */
        return delegate && gc::IsMarkedUnbarriered(zone()->runtimeFromActiveCooperatingThread(), &delegate);
    }

    bool keyNeedsMark(JSScript* script) const {
        return false;
    }

    bool findZoneEdges() override {
        // This is overridden by ObjectValueMap.
        return true;
    }

    void sweep() override {
        /* Remove all entries whose keys remain unmarked. */
        for (Enum e(*this); !e.empty(); e.popFront()) {
            if (gc::IsAboutToBeFinalized(&e.front().mutableKey()))
                e.removeFront();
        }
        /*
         * Once we've swept, all remaining edges should stay within the
         * known-live part of the graph.
         */
        assertEntriesNotAboutToBeFinalized();
    }

    void finish() override {
        Base::finish();
    }

    /* memberOf can be nullptr, which means that the map is not part of a JSObject. */
    void traceMappings(WeakMapTracer* tracer) override {
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            gc::Cell* key = gc::ToMarkable(r.front().key());
            gc::Cell* value = gc::ToMarkable(r.front().value());
            if (key && value) {
                tracer->trace(memberOf,
                              JS::GCCellPtr(r.front().key().get()),
                              JS::GCCellPtr(r.front().value().get()));
            }
        }
    }

  protected:
    void assertEntriesNotAboutToBeFinalized() {
#if DEBUG
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            Key k(r.front().key());
            MOZ_ASSERT(!gc::IsAboutToBeFinalized(&k));
            MOZ_ASSERT(!gc::IsAboutToBeFinalized(&r.front().value()));
            MOZ_ASSERT(k == r.front().key());
        }
#endif
    }
};


class ObjectValueMap : public WeakMap<HeapPtr<JSObject*>, HeapPtr<Value>,
                                      MovableCellHasher<HeapPtr<JSObject*>>>
{
  public:
    ObjectValueMap(JSContext* cx, JSObject* obj)
      : WeakMap<HeapPtr<JSObject*>, HeapPtr<Value>,
                MovableCellHasher<HeapPtr<JSObject*>>>(cx, obj)
    {}

    bool findZoneEdges() override;
};


// Generic weak map for mapping objects to other objects.
class ObjectWeakMap
{
    ObjectValueMap map;

  public:
    explicit ObjectWeakMap(JSContext* cx);
    bool init();

    JS::Zone* zone() const { return map.zone(); }

    JSObject* lookup(const JSObject* obj);
    bool add(JSContext* cx, JSObject* obj, JSObject* target);
    void clear();

    void trace(JSTracer* trc);
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }

#ifdef JSGC_HASH_TABLE_CHECKS
    void checkAfterMovingGC();
#endif
};

} /* namespace js */

namespace JS {

template <>
struct DeletePolicy<js::ObjectValueMap> : public js::GCManagedDeletePolicy<js::ObjectValueMap>
{};

} /* namespace JS */

#endif /* gc_WeakMap_h */
