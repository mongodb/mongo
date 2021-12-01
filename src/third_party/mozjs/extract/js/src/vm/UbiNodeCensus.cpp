/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/UbiNodeCensus.h"

#include "util/Text.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

namespace JS {
namespace ubi {

JS_PUBLIC_API(void)
CountDeleter::operator()(CountBase* ptr)
{
    if (!ptr)
        return;

    // Downcast to our true type and destruct, as guided by our CountType
    // pointer.
    ptr->destruct();
    js_free(ptr);
}

JS_PUBLIC_API(bool)
Census::init() {
    AutoLockForExclusiveAccess lock(cx);
    atomsZone = cx->runtime()->atomsCompartment(lock)->zone();
    return targetZones.init();
}


/*** Count Types ***********************************************************************************/

// The simplest type: just count everything.
class SimpleCount : public CountType {

    struct Count : CountBase {
        size_t totalBytes_;

        explicit Count(SimpleCount& count)
          : CountBase(count),
            totalBytes_(0)
        { }
    };

    UniqueTwoByteChars label;
    bool reportCount : 1;
    bool reportBytes : 1;

  public:
    explicit SimpleCount(UniqueTwoByteChars& label,
                         bool reportCount=true,
                         bool reportBytes=true)
      : CountType(),
        label(Move(label)),
        reportCount(reportCount),
        reportBytes(reportBytes)
    { }

    explicit SimpleCount()
        : CountType(),
          label(nullptr),
          reportCount(true),
          reportBytes(true)
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override { return CountBasePtr(js_new<Count>(*this)); }
    void traceCount(CountBase& countBase, JSTracer* trc) override { }
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

bool
SimpleCount::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);
    if (reportBytes)
        count.totalBytes_ += node.size(mallocSizeOf);
    return true;
}

bool
SimpleCount::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        return false;

    RootedValue countValue(cx, NumberValue(count.total_));
    if (reportCount && !DefineDataProperty(cx, obj, cx->names().count, countValue))
        return false;

    RootedValue bytesValue(cx, NumberValue(count.totalBytes_));
    if (reportBytes && !DefineDataProperty(cx, obj, cx->names().bytes, bytesValue))
        return false;

    if (label) {
        JSString* labelString = JS_NewUCStringCopyZ(cx, label.get());
        if (!labelString)
            return false;
        RootedValue labelValue(cx, StringValue(labelString));
        if (!DefineDataProperty(cx, obj, cx->names().label, labelValue))
            return false;
    }

    report.setObject(*obj);
    return true;
}


// A count type that collects all matching nodes in a bucket.
class BucketCount : public CountType {

    struct Count : CountBase {
        JS::ubi::Vector<JS::ubi::Node::Id> ids_;

        explicit Count(BucketCount& count)
          : CountBase(count),
            ids_()
        { }
    };

  public:
    explicit BucketCount()
      : CountType()
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override { return CountBasePtr(js_new<Count>(*this)); }
    void traceCount(CountBase& countBase, JSTracer* trc) final { }
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

bool
BucketCount::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);
    return count.ids_.append(node.identifier());
}

bool
BucketCount::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    size_t length = count.ids_.length();
    RootedArrayObject arr(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!arr)
        return false;
    arr->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++)
        arr->setDenseElement(i, NumberValue(count.ids_[i]));

    report.setObject(*arr);
    return true;
}


// A type that categorizes nodes by their JavaScript type -- 'objects',
// 'strings', 'scripts', and 'other' -- and then passes the nodes to child
// types.
//
// Implementation details of scripts like jitted code are counted under
// 'scripts'.
class ByCoarseType : public CountType {
    CountTypePtr objects;
    CountTypePtr scripts;
    CountTypePtr strings;
    CountTypePtr other;

    struct Count : CountBase {
        Count(CountType& type,
              CountBasePtr& objects,
              CountBasePtr& scripts,
              CountBasePtr& strings,
              CountBasePtr& other)
          : CountBase(type),
            objects(Move(objects)),
            scripts(Move(scripts)),
            strings(Move(strings)),
            other(Move(other))
        { }

        CountBasePtr objects;
        CountBasePtr scripts;
        CountBasePtr strings;
        CountBasePtr other;
    };

  public:
    ByCoarseType(CountTypePtr& objects,
                 CountTypePtr& scripts,
                 CountTypePtr& strings,
                 CountTypePtr& other)
      : CountType(),
        objects(Move(objects)),
        scripts(Move(scripts)),
        strings(Move(strings)),
        other(Move(other))
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override;
    void traceCount(CountBase& countBase, JSTracer* trc) override;
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

CountBasePtr
ByCoarseType::makeCount()
{
    CountBasePtr objectsCount(objects->makeCount());
    CountBasePtr scriptsCount(scripts->makeCount());
    CountBasePtr stringsCount(strings->makeCount());
    CountBasePtr otherCount(other->makeCount());

    if (!objectsCount || !scriptsCount || !stringsCount || !otherCount)
        return CountBasePtr(nullptr);

    return CountBasePtr(js_new<Count>(*this,
                                      objectsCount,
                                      scriptsCount,
                                      stringsCount,
                                      otherCount));
}

void
ByCoarseType::traceCount(CountBase& countBase, JSTracer* trc)
{
    Count& count = static_cast<Count&>(countBase);
    count.objects->trace(trc);
    count.scripts->trace(trc);
    count.strings->trace(trc);
    count.other->trace(trc);
}

bool
ByCoarseType::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);

    switch (node.coarseType()) {
      case JS::ubi::CoarseType::Object:
        return count.objects->count(mallocSizeOf, node);
      case JS::ubi::CoarseType::Script:
        return count.scripts->count(mallocSizeOf, node);
      case JS::ubi::CoarseType::String:
        return count.strings->count(mallocSizeOf, node);
      case JS::ubi::CoarseType::Other:
        return count.other->count(mallocSizeOf, node);
      default:
        MOZ_CRASH("bad JS::ubi::CoarseType in JS::ubi::ByCoarseType::count");
        return false;
    }
}

bool
ByCoarseType::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        return false;

    RootedValue objectsReport(cx);
    if (!count.objects->report(cx, &objectsReport) ||
        !DefineDataProperty(cx, obj, cx->names().objects, objectsReport))
        return false;

    RootedValue scriptsReport(cx);
    if (!count.scripts->report(cx, &scriptsReport) ||
        !DefineDataProperty(cx, obj, cx->names().scripts, scriptsReport))
        return false;

    RootedValue stringsReport(cx);
    if (!count.strings->report(cx, &stringsReport) ||
        !DefineDataProperty(cx, obj, cx->names().strings, stringsReport))
        return false;

    RootedValue otherReport(cx);
    if (!count.other->report(cx, &otherReport) ||
        !DefineDataProperty(cx, obj, cx->names().other, otherReport))
        return false;

    report.setObject(*obj);
    return true;
}


// Comparison function for sorting hash table entries by the smallest node ID
// they counted. Node IDs are stable and unique, which ensures ordering of
// results never depends on hash table placement or sort algorithm vagaries. The
// arguments are doubly indirect: they're pointers to elements in an array of
// pointers to table entries.
template<typename Entry>
static int compareEntries(const void* lhsVoid, const void* rhsVoid) {
    auto lhs = (*static_cast<const Entry* const*>(lhsVoid))->value()->smallestNodeIdCounted_;
    auto rhs = (*static_cast<const Entry* const*>(rhsVoid))->value()->smallestNodeIdCounted_;

    // We don't want to just subtract the values, as they're unsigned.
    if (lhs < rhs)
        return 1;
    if (lhs > rhs)
        return -1;
    return 0;
}

// A hash map mapping from C strings to counts.
using CStringCountMap = HashMap<const char*, CountBasePtr, CStringHasher, SystemAllocPolicy>;

// Convert a HashMap into an object with each key one of the entries from the
// map and each value the associated count's report. For use during census
// reporting.
//
// `Map` must be a `HashMap` from some key type to a `CountBasePtr`.
//
// `GetName` must be a callable type which takes `const Map::Key&` and returns
// `const char*`.
template <class Map, class GetName>
static PlainObject*
countMapToObject(JSContext* cx, Map& map, GetName getName) {
    // Build a vector of pointers to entries; sort by total; and then use
    // that to build the result object. This makes the ordering of entries
    // more interesting, and a little less non-deterministic.

    JS::ubi::Vector<typename Map::Entry*> entries;
    if (!entries.reserve(map.count())) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    for (auto r = map.all(); !r.empty(); r.popFront())
        entries.infallibleAppend(&r.front());

    if (entries.length()) {
        qsort(entries.begin(), entries.length(), sizeof(*entries.begin()),
              compareEntries<typename Map::Entry>);
    }

    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        return nullptr;

    for (auto& entry : entries) {
        CountBasePtr& thenCount = entry->value();
        RootedValue thenReport(cx);
        if (!thenCount->report(cx, &thenReport))
            return nullptr;

        const char* name = getName(entry->key());
        MOZ_ASSERT(name);
        JSAtom* atom = Atomize(cx, name, strlen(name));
        if (!atom)
            return nullptr;

        RootedId entryId(cx, AtomToId(atom));
        if (!DefineDataProperty(cx, obj, entryId, thenReport))
            return nullptr;
    }

    return obj;
}


// A type that categorizes nodes that are JSObjects by their class name,
// and places all other nodes in an 'other' category.
class ByObjectClass : public CountType {
    // A table mapping class names to their counts. Note that we treat js::Class
    // instances with the same name as equal keys. If you have several
    // js::Classes with equal names (and we do; as of this writing there were
    // six named "Object"), you will get several different js::Classes being
    // counted in the same table entry.
    using Table = CStringCountMap;
    using Entry = Table::Entry;

    struct Count : public CountBase {
        Table table;
        CountBasePtr other;

        Count(CountType& type, CountBasePtr& other)
          : CountBase(type),
            other(Move(other))
        { }

        bool init() { return table.init(); }
    };

    CountTypePtr classesType;
    CountTypePtr otherType;

  public:
    ByObjectClass(CountTypePtr& classesType, CountTypePtr& otherType)
        : CountType(),
          classesType(Move(classesType)),
          otherType(Move(otherType))
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override;
    void traceCount(CountBase& countBase, JSTracer* trc) override;
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

CountBasePtr
ByObjectClass::makeCount()
{
    CountBasePtr otherCount(otherType->makeCount());
    if (!otherCount)
        return nullptr;

    UniquePtr<Count> count(js_new<Count>(*this, otherCount));
    if (!count || !count->init())
        return nullptr;

    return CountBasePtr(count.release());
}

void
ByObjectClass::traceCount(CountBase& countBase, JSTracer* trc)
{
    Count& count = static_cast<Count&>(countBase);
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront())
        r.front().value()->trace(trc);
    count.other->trace(trc);
}

bool
ByObjectClass::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);

    const char* className = node.jsObjectClassName();
    if (!className)
        return count.other->count(mallocSizeOf, node);

    Table::AddPtr p = count.table.lookupForAdd(className);
    if (!p) {
        CountBasePtr classCount(classesType->makeCount());
        if (!classCount || !count.table.add(p, className, Move(classCount)))
            return false;
    }
    return p->value()->count(mallocSizeOf, node);
}

bool
ByObjectClass::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    RootedPlainObject obj(cx, countMapToObject(cx, count.table, [](const char* key) {
        return key;
    }));
    if (!obj)
        return false;

    RootedValue otherReport(cx);
    if (!count.other->report(cx, &otherReport) ||
        !DefineDataProperty(cx, obj, cx->names().other, otherReport))
        return false;

    report.setObject(*obj);
    return true;
}


// A count type that categorizes nodes by their ubi::Node::typeName.
class ByUbinodeType : public CountType {
    // Note that, because ubi::Node::typeName promises to return a specific
    // pointer, not just any string whose contents are correct, we can use their
    // addresses as hash table keys.
    using Table = HashMap<const char16_t*, CountBasePtr, DefaultHasher<const char16_t*>,
                          SystemAllocPolicy>;
    using Entry = Table::Entry;

    struct Count: public CountBase {
        Table table;

        explicit Count(CountType& type) : CountBase(type) { }

        bool init() { return table.init(); }
    };

    CountTypePtr entryType;

  public:
    explicit ByUbinodeType(CountTypePtr& entryType)
      : CountType(),
        entryType(Move(entryType))
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override;
    void traceCount(CountBase& countBase, JSTracer* trc) override;
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

CountBasePtr
ByUbinodeType::makeCount()
{
    UniquePtr<Count> count(js_new<Count>(*this));
    if (!count || !count->init())
        return nullptr;

    return CountBasePtr(count.release());
}

void
ByUbinodeType::traceCount(CountBase& countBase, JSTracer* trc)
{
    Count& count = static_cast<Count&>(countBase);
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront())
        r.front().value()->trace(trc);
}

bool
ByUbinodeType::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);

    const char16_t* key = node.typeName();
    MOZ_ASSERT(key);
    Table::AddPtr p = count.table.lookupForAdd(key);
    if (!p) {
        CountBasePtr typesCount(entryType->makeCount());
        if (!typesCount || !count.table.add(p, key, Move(typesCount)))
            return false;
    }
    return p->value()->count(mallocSizeOf, node);
}

bool
ByUbinodeType::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    // Build a vector of pointers to entries; sort by total; and then use
    // that to build the result object. This makes the ordering of entries
    // more interesting, and a little less non-deterministic.
    JS::ubi::Vector<Entry*> entries;
    if (!entries.reserve(count.table.count()))
        return false;
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront())
        entries.infallibleAppend(&r.front());
    if (entries.length())
        qsort(entries.begin(), entries.length(), sizeof(*entries.begin()), compareEntries<Entry>);

    // Now build the result by iterating over the sorted vector.
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        return false;
    for (Entry** entryPtr = entries.begin(); entryPtr < entries.end(); entryPtr++) {
        Entry& entry = **entryPtr;
        CountBasePtr& typeCount = entry.value();
        RootedValue typeReport(cx);
        if (!typeCount->report(cx, &typeReport))
            return false;

        const char16_t* name = entry.key();
        MOZ_ASSERT(name);
        JSAtom* atom = AtomizeChars(cx, name, js_strlen(name));
        if (!atom)
            return false;
        RootedId entryId(cx, AtomToId(atom));

        if (!DefineDataProperty(cx, obj, entryId, typeReport))
            return false;
    }

    report.setObject(*obj);
    return true;
}


// A count type that categorizes nodes by the JS stack under which they were
// allocated.
class ByAllocationStack : public CountType {
    using Table = HashMap<StackFrame, CountBasePtr, DefaultHasher<StackFrame>,
                          SystemAllocPolicy>;
    using Entry = Table::Entry;

    struct Count : public CountBase {
        // NOTE: You may look up entries in this table by JS::ubi::StackFrame
        // key only during traversal, NOT ONCE TRAVERSAL IS COMPLETE. Once
        // traversal is complete, you may only iterate over it.
        //
        // In this hash table, keys are JSObjects (with some indirection), and
        // we use JSObject identity (that is, address identity) as key
        // identity. The normal way to support such a table is to make the trace
        // function notice keys that have moved and re-key them in the
        // table. However, our trace function does *not* rehash; the first GC
        // may render the hash table unsearchable.
        //
        // This is as it should be:
        //
        // First, the heap traversal phase needs lookups by key to work. But no
        // GC may ever occur during a traversal; this is enforced by the
        // JS::ubi::BreadthFirst template. So the traceCount function doesn't
        // need to do anything to help traversal; it never even runs then.
        //
        // Second, the report phase needs iteration over the table to work, but
        // never looks up entries by key. GC may well occur during this phase:
        // we allocate a Map object, and probably cross-compartment wrappers for
        // SavedFrame instances as well. If a GC were to occur, it would call
        // our traceCount function; if traceCount were to re-key, that would
        // ruin the traversal in progress.
        //
        // So depending on the phase, we either don't need re-keying, or
        // can't abide it.
        Table table;
        CountBasePtr noStack;

        Count(CountType& type, CountBasePtr& noStack)
          : CountBase(type),
            noStack(Move(noStack))
        { }
        bool init() { return table.init(); }
    };

    CountTypePtr entryType;
    CountTypePtr noStackType;

  public:
    ByAllocationStack(CountTypePtr& entryType, CountTypePtr& noStackType)
      : CountType(),
        entryType(Move(entryType)),
        noStackType(Move(noStackType))
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override;
    void traceCount(CountBase& countBase, JSTracer* trc) override;
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

CountBasePtr
ByAllocationStack::makeCount()
{
    CountBasePtr noStackCount(noStackType->makeCount());
    if (!noStackCount)
        return nullptr;

    UniquePtr<Count> count(js_new<Count>(*this, noStackCount));
    if (!count || !count->init())
        return nullptr;
    return CountBasePtr(count.release());
}

void
ByAllocationStack::traceCount(CountBase& countBase, JSTracer* trc)
{
    Count& count = static_cast<Count&>(countBase);
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront()) {
        // Trace our child Counts.
        r.front().value()->trace(trc);

        // Trace the StackFrame that is this entry's key. Do not re-key if
        // it has moved; see comments for ByAllocationStack::Count::table.
        const StackFrame* key = &r.front().key();
        auto& k = *const_cast<StackFrame*>(key);
        k.trace(trc);
    }
    count.noStack->trace(trc);
}

bool
ByAllocationStack::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);

    // If we do have an allocation stack for this node, include it in the
    // count for that stack.
    if (node.hasAllocationStack()) {
        auto allocationStack = node.allocationStack();
        auto p = count.table.lookupForAdd(allocationStack);
        if (!p) {
            CountBasePtr stackCount(entryType->makeCount());
            if (!stackCount || !count.table.add(p, allocationStack, Move(stackCount)))
                return false;
        }
        MOZ_ASSERT(p);
        return p->value()->count(mallocSizeOf, node);
    }

    // Otherwise, count it in the "no stack" category.
    return count.noStack->count(mallocSizeOf, node);
}

bool
ByAllocationStack::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

#ifdef DEBUG
    // Check that nothing rehashes our table while we hold pointers into it.
    Generation generation = count.table.generation();
#endif

    // Build a vector of pointers to entries; sort by total; and then use
    // that to build the result object. This makes the ordering of entries
    // more interesting, and a little less non-deterministic.
    JS::ubi::Vector<Entry*> entries;
    if (!entries.reserve(count.table.count()))
        return false;
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront())
        entries.infallibleAppend(&r.front());
    if (entries.length())
        qsort(entries.begin(), entries.length(), sizeof(*entries.begin()), compareEntries<Entry>);

    // Now build the result by iterating over the sorted vector.
    Rooted<MapObject*> map(cx, MapObject::create(cx));
    if (!map)
        return false;
    for (Entry** entryPtr = entries.begin(); entryPtr < entries.end(); entryPtr++) {
        Entry& entry = **entryPtr;
        MOZ_ASSERT(entry.key());

        RootedObject stack(cx);
        if (!entry.key().constructSavedFrameStack(cx, &stack) ||
            !cx->compartment()->wrap(cx, &stack))
            {
                return false;
            }
        RootedValue stackVal(cx, ObjectValue(*stack));

        CountBasePtr& stackCount = entry.value();
        RootedValue stackReport(cx);
        if (!stackCount->report(cx, &stackReport))
            return false;

        if (!MapObject::set(cx, map, stackVal, stackReport))
            return false;
    }

    if (count.noStack->total_ > 0) {
        RootedValue noStackReport(cx);
        if (!count.noStack->report(cx, &noStackReport))
            return false;
        RootedValue noStack(cx, StringValue(cx->names().noStack));
        if (!MapObject::set(cx, map, noStack, noStackReport))
            return false;
    }

    MOZ_ASSERT(generation == count.table.generation());

    report.setObject(*map);
    return true;
}

// A count type that categorizes nodes by their script's filename.
class ByFilename : public CountType {
    using UniqueCString = UniquePtr<char, JS::FreePolicy>;

    struct UniqueCStringHasher {
        using Lookup = UniqueCString;

        static js::HashNumber hash(const Lookup& lookup) {
            return CStringHasher::hash(lookup.get());
        }

        static bool match(const UniqueCString& key, const Lookup& lookup) {
            return CStringHasher::match(key.get(), lookup.get());
        }
    };

    // A table mapping filenames to their counts. Note that we treat scripts
    // with the same filename as equivalent. If you have several sources with
    // the same filename, then all their scripts will get bucketed together.
    using Table = HashMap<UniqueCString, CountBasePtr, UniqueCStringHasher,
                          SystemAllocPolicy>;
    using Entry = Table::Entry;

    struct Count : public CountBase {
        Table table;
        CountBasePtr then;
        CountBasePtr noFilename;

        Count(CountType& type, CountBasePtr&& then, CountBasePtr&& noFilename)
          : CountBase(type)
          , then(Move(then))
          , noFilename(Move(noFilename))
        { }

        bool init() { return table.init(); }
    };

    CountTypePtr thenType;
    CountTypePtr noFilenameType;

  public:
    ByFilename(CountTypePtr&& thenType, CountTypePtr&& noFilenameType)
        : CountType(),
          thenType(Move(thenType)),
          noFilenameType(Move(noFilenameType))
    { }

    void destructCount(CountBase& countBase) override {
        Count& count = static_cast<Count&>(countBase);
        count.~Count();
    }

    CountBasePtr makeCount() override;
    void traceCount(CountBase& countBase, JSTracer* trc) override;
    bool count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node) override;
    bool report(JSContext* cx, CountBase& countBase, MutableHandleValue report) override;
};

CountBasePtr
ByFilename::makeCount()
{
    CountBasePtr thenCount(thenType->makeCount());
    if (!thenCount)
        return nullptr;

    CountBasePtr noFilenameCount(noFilenameType->makeCount());
    if (!noFilenameCount)
        return nullptr;

    UniquePtr<Count> count(js_new<Count>(*this, Move(thenCount), Move(noFilenameCount)));
    if (!count || !count->init())
        return nullptr;

    return CountBasePtr(count.release());
}

void
ByFilename::traceCount(CountBase& countBase, JSTracer* trc)
{
    Count& count = static_cast<Count&>(countBase);
    for (Table::Range r = count.table.all(); !r.empty(); r.popFront())
        r.front().value()->trace(trc);
    count.noFilename->trace(trc);
}

bool
ByFilename::count(CountBase& countBase, mozilla::MallocSizeOf mallocSizeOf, const Node& node)
{
    Count& count = static_cast<Count&>(countBase);

    const char* filename = node.scriptFilename();
    if (!filename)
        return count.noFilename->count(mallocSizeOf, node);

    UniqueCString myFilename(js_strdup(filename));
    if (!myFilename)
        return false;

    Table::AddPtr p = count.table.lookupForAdd(myFilename);
    if (!p) {
        CountBasePtr thenCount(thenType->makeCount());
        if (!thenCount || !count.table.add(p, Move(myFilename), Move(thenCount)))
            return false;
    }
    return p->value()->count(mallocSizeOf, node);
}

bool
ByFilename::report(JSContext* cx, CountBase& countBase, MutableHandleValue report)
{
    Count& count = static_cast<Count&>(countBase);

    RootedPlainObject obj(cx, countMapToObject(cx, count.table, [](const UniqueCString& key) {
        return key.get();
    }));
    if (!obj)
        return false;

    RootedValue noFilenameReport(cx);
    if (!count.noFilename->report(cx, &noFilenameReport) ||
        !DefineDataProperty(cx, obj, cx->names().noFilename, noFilenameReport))
    {
        return false;
    }

    report.setObject(*obj);
    return true;
}


/*** Census Handler *******************************************************************************/

JS_PUBLIC_API(bool)
CensusHandler::operator() (BreadthFirst<CensusHandler>& traversal,
                           Node origin, const Edge& edge,
                           NodeData* referentData, bool first)
{
    // We're only interested in the first time we reach edge.referent, not
    // in every edge arriving at that node.
    if (!first)
        return true;

    // Don't count nodes outside the debuggee zones. Do count things in the
    // special atoms zone, but don't traverse their outgoing edges, on the
    // assumption that they are shared resources that debuggee is using.
    // Symbols are always allocated in the atoms zone, even if they were
    // created for exactly one compartment and never shared; this rule will
    // include such nodes in the count.
    const Node& referent = edge.referent;
    Zone* zone = referent.zone();

    if (census.targetZones.count() == 0 || census.targetZones.has(zone))
        return rootCount->count(mallocSizeOf, referent);

    if (zone == census.atomsZone) {
        traversal.abandonReferent();
        return rootCount->count(mallocSizeOf, referent);
    }

    traversal.abandonReferent();
    return true;
}


/*** Parsing Breakdowns ***************************************************************************/

static CountTypePtr
ParseChildBreakdown(JSContext* cx, HandleObject breakdown, PropertyName* prop)
{
    RootedValue v(cx);
    if (!GetProperty(cx, breakdown, breakdown, prop, &v))
        return nullptr;
    return ParseBreakdown(cx, v);
}

JS_PUBLIC_API(CountTypePtr)
ParseBreakdown(JSContext* cx, HandleValue breakdownValue)
{
    if (breakdownValue.isUndefined()) {
        // Construct the default type, { by: 'count' }
        CountTypePtr simple(cx->new_<SimpleCount>());
        return simple;
    }

    RootedObject breakdown(cx, ToObject(cx, breakdownValue));
    if (!breakdown)
        return nullptr;

    RootedValue byValue(cx);
    if (!GetProperty(cx, breakdown, breakdown, cx->names().by, &byValue))
        return nullptr;
    RootedString byString(cx, ToString(cx, byValue));
    if (!byString)
        return nullptr;
    RootedLinearString by(cx, byString->ensureLinear(cx));
    if (!by)
        return nullptr;

    if (StringEqualsAscii(by, "count")) {
        RootedValue countValue(cx), bytesValue(cx);
        if (!GetProperty(cx, breakdown, breakdown, cx->names().count, &countValue) ||
            !GetProperty(cx, breakdown, breakdown, cx->names().bytes, &bytesValue))
            return nullptr;

        // Both 'count' and 'bytes' default to true if omitted, but ToBoolean
        // naturally treats 'undefined' as false; fix this up.
        if (countValue.isUndefined()) countValue.setBoolean(true);
        if (bytesValue.isUndefined()) bytesValue.setBoolean(true);

        // Undocumented feature, for testing: { by: 'count' } breakdowns can have
        // a 'label' property whose value is converted to a string and included as
        // a 'label' property on the report object.
        RootedValue label(cx);
        if (!GetProperty(cx, breakdown, breakdown, cx->names().label, &label))
            return nullptr;

        UniqueTwoByteChars labelUnique(nullptr);
        if (!label.isUndefined()) {
            RootedString labelString(cx, ToString(cx, label));
            if (!labelString)
                return nullptr;

            JSFlatString* flat = labelString->ensureFlat(cx);
            if (!flat)
                return nullptr;

            AutoStableStringChars chars(cx);
            if (!chars.initTwoByte(cx, flat))
                return nullptr;

            // Since flat strings are null-terminated, and AutoStableStringChars
            // null- terminates if it needs to make a copy, we know that
            // chars.twoByteChars() is null-terminated.
            labelUnique = DuplicateString(cx, chars.twoByteChars());
            if (!labelUnique)
                return nullptr;
        }

        CountTypePtr simple(cx->new_<SimpleCount>(labelUnique,
                                                  ToBoolean(countValue),
                                                  ToBoolean(bytesValue)));
        return simple;
    }

    if (StringEqualsAscii(by, "bucket"))
        return CountTypePtr(cx->new_<BucketCount>());

    if (StringEqualsAscii(by, "objectClass")) {
        CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, cx->names().then));
        if (!thenType)
            return nullptr;

        CountTypePtr otherType(ParseChildBreakdown(cx, breakdown, cx->names().other));
        if (!otherType)
            return nullptr;

        return CountTypePtr(cx->new_<ByObjectClass>(thenType, otherType));
    }

    if (StringEqualsAscii(by, "coarseType")) {
        CountTypePtr objectsType(ParseChildBreakdown(cx, breakdown, cx->names().objects));
        if (!objectsType)
            return nullptr;
        CountTypePtr scriptsType(ParseChildBreakdown(cx, breakdown, cx->names().scripts));
        if (!scriptsType)
            return nullptr;
        CountTypePtr stringsType(ParseChildBreakdown(cx, breakdown, cx->names().strings));
        if (!stringsType)
            return nullptr;
        CountTypePtr otherType(ParseChildBreakdown(cx, breakdown, cx->names().other));
        if (!otherType)
            return nullptr;

        return CountTypePtr(cx->new_<ByCoarseType>(objectsType,
                                                   scriptsType,
                                                   stringsType,
                                                   otherType));
    }

    if (StringEqualsAscii(by, "internalType")) {
        CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, cx->names().then));
        if (!thenType)
            return nullptr;

        return CountTypePtr(cx->new_<ByUbinodeType>(thenType));
    }

    if (StringEqualsAscii(by, "allocationStack")) {
        CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, cx->names().then));
        if (!thenType)
            return nullptr;
        CountTypePtr noStackType(ParseChildBreakdown(cx, breakdown, cx->names().noStack));
        if (!noStackType)
            return nullptr;

        return CountTypePtr(cx->new_<ByAllocationStack>(thenType, noStackType));
    }

    if (StringEqualsAscii(by, "filename")) {
        CountTypePtr thenType(ParseChildBreakdown(cx, breakdown, cx->names().then));
        if (!thenType)
            return nullptr;

        CountTypePtr noFilenameType(ParseChildBreakdown(cx, breakdown, cx->names().noFilename));
        if (!noFilenameType)
            return nullptr;

        return CountTypePtr(cx->new_<ByFilename>(Move(thenType), Move(noFilenameType)));
    }

    // We didn't recognize the breakdown type; complain.
    RootedString bySource(cx, ValueToSource(cx, byValue));
    if (!bySource)
        return nullptr;

    JSAutoByteString byBytes(cx, bySource);
    if (!byBytes)
        return nullptr;

    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_DEBUG_CENSUS_BREAKDOWN,
                               byBytes.ptr());
    return nullptr;
}

// Get the default census breakdown:
//
// { by: "coarseType",
//   objects: { by: "objectClass" },
//   other:   { by: "internalType" }
// }
static CountTypePtr
GetDefaultBreakdown(JSContext* cx)
{
    CountTypePtr byClass(cx->new_<SimpleCount>());
    if (!byClass)
        return nullptr;

    CountTypePtr byClassElse(cx->new_<SimpleCount>());
    if (!byClassElse)
        return nullptr;

    CountTypePtr objects(cx->new_<ByObjectClass>(byClass, byClassElse));
    if (!objects)
        return nullptr;

    CountTypePtr scripts(cx->new_<SimpleCount>());
    if (!scripts)
        return nullptr;

    CountTypePtr strings(cx->new_<SimpleCount>());
    if (!strings)
        return nullptr;

    CountTypePtr byType(cx->new_<SimpleCount>());
    if (!byType)
        return nullptr;

    CountTypePtr other(cx->new_<ByUbinodeType>(byType));
    if (!other)
        return nullptr;

    return CountTypePtr(cx->new_<ByCoarseType>(objects,
                                               scripts,
                                               strings,
                                               other));
}

JS_PUBLIC_API(bool)
ParseCensusOptions(JSContext* cx, Census& census, HandleObject options, CountTypePtr& outResult)
{
    RootedValue breakdown(cx, UndefinedValue());
    if (options && !GetProperty(cx, options, options, cx->names().breakdown, &breakdown))
        return false;

    outResult = breakdown.isUndefined()
        ? GetDefaultBreakdown(cx)
        : ParseBreakdown(cx, breakdown);
    return !!outResult;
}

} // namespace ubi
} // namespace JS
