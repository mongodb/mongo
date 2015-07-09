/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DebuggerMemory.h"

#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/Vector.h"

#include <stdlib.h>

#include "jscompartment.h"

#include "gc/Marking.h"
#include "js/Debug.h"
#include "js/TracingAPI.h"
#include "js/UbiNode.h"
#include "js/UbiNodeTraverse.h"
#include "vm/Debugger.h"
#include "vm/GlobalObject.h"
#include "vm/SavedStacks.h"

#include "vm/Debugger-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::ubi::BreadthFirst;
using JS::ubi::Edge;
using JS::ubi::Node;

using mozilla::Maybe;
using mozilla::Move;
using mozilla::Nothing;

/* static */ DebuggerMemory*
DebuggerMemory::create(JSContext* cx, Debugger* dbg)
{
    Value memoryProtoValue = dbg->object->getReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_PROTO);
    RootedObject memoryProto(cx, &memoryProtoValue.toObject());
    RootedNativeObject memory(cx, NewNativeObjectWithGivenProto(cx, &class_, memoryProto,
                                                                NullPtr()));
    if (!memory)
        return nullptr;

    dbg->object->setReservedSlot(Debugger::JSSLOT_DEBUG_MEMORY_INSTANCE, ObjectValue(*memory));
    memory->setReservedSlot(JSSLOT_DEBUGGER, ObjectValue(*dbg->object));

    return &memory->as<DebuggerMemory>();
}

Debugger*
DebuggerMemory::getDebugger()
{
    const Value& dbgVal = getReservedSlot(JSSLOT_DEBUGGER);
    return Debugger::fromJSObject(&dbgVal.toObject());
}

/* static */ bool
DebuggerMemory::construct(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                         "Debugger.Memory");
    return false;
}

/* static */ const Class DebuggerMemory::class_ = {
    "Memory",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_COUNT)
};

/* static */ DebuggerMemory*
DebuggerMemory::checkThis(JSContext* cx, CallArgs& args, const char* fnName)
{
    const Value& thisValue = args.thisv();

    if (!thisValue.isObject()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NOT_NONNULL_OBJECT);
        return nullptr;
    }

    JSObject& thisObject = thisValue.toObject();
    if (!thisObject.is<DebuggerMemory>()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             class_.name, fnName, thisObject.getClass()->name);
        return nullptr;
    }

    // Check for Debugger.Memory.prototype, which has the same class as
    // Debugger.Memory instances, however doesn't actually represent an instance
    // of Debugger.Memory. It is the only object that is<DebuggerMemory>() but
    // doesn't have a Debugger instance.
    if (thisObject.as<DebuggerMemory>().getReservedSlot(JSSLOT_DEBUGGER).isUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             class_.name, fnName, "prototype object");
        return nullptr;
    }

    return &thisObject.as<DebuggerMemory>();
}

/**
 * Get the |DebuggerMemory*| from the current this value and handle any errors
 * that might occur therein.
 *
 * These parameters must already exist when calling this macro:
 * - JSContext* cx
 * - unsigned argc
 * - Value* vp
 * - const char* fnName
 * These parameters will be defined after calling this macro:
 * - CallArgs args
 * - DebuggerMemory* memory (will be non-null)
 */
#define THIS_DEBUGGER_MEMORY(cx, argc, vp, fnName, args, memory)        \
    CallArgs args = CallArgsFromVp(argc, vp);                           \
    Rooted<DebuggerMemory*> memory(cx, checkThis(cx, args, fnName));   \
    if (!memory)                                                        \
        return false

/* static */ bool
DebuggerMemory::setTrackingAllocationSites(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set trackingAllocationSites)", args, memory);
    if (!args.requireAtLeast(cx, "(set trackingAllocationSites)", 1))
        return false;

    Debugger* dbg = memory->getDebugger();
    bool enabling = ToBoolean(args[0]);

    if (enabling == dbg->trackingAllocationSites) {
        // Nothing to do here...
        args.rval().setUndefined();
        return true;
    }

    if (enabling) {
        for (GlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront()) {
            JSCompartment* compartment = r.front()->compartment();
            if (compartment->hasObjectMetadataCallback()) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                     JSMSG_OBJECT_METADATA_CALLBACK_ALREADY_SET);
                return false;
            }
        }
    }

    for (GlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront()) {
        if (enabling) {
            r.front()->compartment()->setObjectMetadataCallback(SavedStacksMetadataCallback);
        } else {
            r.front()->compartment()->forgetObjectMetadataCallback();
        }
    }

    if (!enabling)
        dbg->emptyAllocationsLog();

    dbg->trackingAllocationSites = enabling;
    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getTrackingAllocationSites(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get trackingAllocationSites)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->trackingAllocationSites);
    return true;
}

/* static */ bool
DebuggerMemory::drainAllocationsLog(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "drainAllocationsLog", args, memory);
    Debugger* dbg = memory->getDebugger();

    if (!dbg->trackingAllocationSites) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NOT_TRACKING_ALLOCATIONS,
                             "drainAllocationsLog");
        return false;
    }

    size_t length = dbg->allocationsLogLength;

    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++) {
        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        // Don't pop the AllocationSite yet. The queue's links are followed by
        // the GC to find the AllocationSite, but are not barried, so we must
        // edit them with great care. Use the queue entry in place, and then
        // pop and delete together.
        Debugger::AllocationSite* allocSite = dbg->allocationsLog.getFirst();
        RootedValue frame(cx, ObjectOrNullValue(allocSite->frame));
        if (!DefineProperty(cx, obj, cx->names().frame, frame))
            return false;

        RootedValue timestampValue(cx, NumberValue(allocSite->when));
        if (!DefineProperty(cx, obj, cx->names().timestamp, timestampValue))
            return false;

        result->setDenseElement(i, ObjectValue(*obj));

        // Pop the front queue entry, and delete it immediately, so that
        // the GC sees the AllocationSite's RelocatablePtr barriers run
        // atomically with the change to the graph (the queue link).
        MOZ_ALWAYS_TRUE(dbg->allocationsLog.popFirst() == allocSite);
        js_delete(allocSite);
    }

    dbg->allocationsLogOverflowed = false;
    dbg->allocationsLogLength = 0;
    args.rval().setObject(*result);
    return true;
}

/* static */ bool
DebuggerMemory::getMaxAllocationsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get maxAllocationsLogLength)", args, memory);
    args.rval().setInt32(memory->getDebugger()->maxAllocationsLogLength);
    return true;
}

/* static */ bool
DebuggerMemory::setMaxAllocationsLogLength(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set maxAllocationsLogLength)", args, memory);
    if (!args.requireAtLeast(cx, "(set maxAllocationsLogLength)", 1))
        return false;

    int32_t max;
    if (!ToInt32(cx, args[0], &max))
        return false;

    if (max < 1) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             "(set maxAllocationsLogLength)'s parameter",
                             "not a positive integer");
        return false;
    }

    Debugger* dbg = memory->getDebugger();
    dbg->maxAllocationsLogLength = max;

    while (dbg->allocationsLogLength > dbg->maxAllocationsLogLength) {
        js_delete(dbg->allocationsLog.getFirst());
        dbg->allocationsLogLength--;
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getAllocationSamplingProbability(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get allocationSamplingProbability)", args, memory);
    args.rval().setDouble(memory->getDebugger()->allocationSamplingProbability);
    return true;
}

/* static */ bool
DebuggerMemory::setAllocationSamplingProbability(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(set allocationSamplingProbability)", args, memory);
    if (!args.requireAtLeast(cx, "(set allocationSamplingProbability)", 1))
        return false;

    double probability;
    if (!ToNumber(cx, args[0], &probability))
        return false;

    if (probability < 0.0 || probability > 1.0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             "(set allocationSamplingProbability)'s parameter",
                             "not a number between 0 and 1");
        return false;
    }

    memory->getDebugger()->allocationSamplingProbability = probability;
    args.rval().setUndefined();
    return true;
}

/* static */ bool
DebuggerMemory::getAllocationsLogOverflowed(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "(get allocationsLogOverflowed)", args, memory);
    args.rval().setBoolean(memory->getDebugger()->allocationsLogOverflowed);
    return true;
}


/* Debugger.Memory.prototype.takeCensus */

void
JS::dbg::SetDebuggerMallocSizeOf(JSRuntime* rt, mozilla::MallocSizeOf mallocSizeOf) {
    rt->debuggerMallocSizeOf = mallocSizeOf;
}

namespace js {
namespace dbg {

// Common data for census traversals.
struct Census {
    JSContext * const cx;
    JS::ZoneSet debuggeeZones;
    Zone* atomsZone;

    explicit Census(JSContext* cx) : cx(cx), atomsZone(nullptr) { }

    bool init() {
        AutoLockForExclusiveAccess lock(cx);
        atomsZone = cx->runtime()->atomsCompartment()->zone();
        return debuggeeZones.init();
    }
};

// An *assorter* class is one with the following constructors, destructor,
// and member functions:
//
//   Assorter(Census& census);
//   Assorter(Assorter&&)
//   Assorter& operator=(Assorter&&)
//   ~Assorter()
//      Construction given a Census; move construction and assignment, for being
//      stored in containers; and destruction.
//
//   bool init(Census& census);
//      A fallible initializer.
//
//   bool count(Census& census, const Node& node);
//      Categorize and count |node| as appropriate for this kind of assorter.
//
//   size_t total() const;
//      Return the number of times 'count' has been called.
//
//   bool report(Census& census, MutableHandleValue report);
//      Construct a JavaScript object reporting the counts this assorter has
//      seen, and store it in |report|.
//
// In each of these, |census| provides ambient information needed for assorting,
// like a JSContext for reporting errors.

// The simplest assorter: count everything, and return a tally form.
class Tally {
    size_t total_;

  public:
    explicit Tally(Census& census) : total_(0) { }
    Tally(Tally&& rhs) : total_(rhs.total_) { }
    Tally& operator=(Tally&& rhs) { total_ = rhs.total_; return *this; }

    bool init(Census& census) { return true; }

    bool count(Census& census, const Node& node) {
        total_++;
        return true;
    }

    size_t total() const { return total_; }

    bool report(Census& census, MutableHandleValue report) {
        RootedPlainObject obj(census.cx, NewBuiltinClassInstance<PlainObject>(census.cx));
        RootedValue countValue(census.cx, NumberValue(total_));
        if (!obj ||
            !DefineProperty(census.cx, obj, census.cx->names().count, countValue))
        {
            return false;
        }
        report.setObject(*obj);
        return true;
    }
};

// An assorter that breaks nodes down by their JavaScript type --- 'objects',
// 'strings', 'scripts', and 'other' --- and then passes the nodes to
// sub-assorters. The template arguments must themselves be assorter types.
//
// Implementation details of scripts like jitted code are counted under
// 'scripts'.
template<typename EachObject = Tally,
         typename EachScript = Tally,
         typename EachString = Tally,
         typename EachOther  = Tally>
class ByJSType {
    size_t total_;
    EachObject objects;
    EachScript scripts;
    EachString strings;
    EachOther other;

  public:
    explicit ByJSType(Census& census)
      : total_(0),
        objects(census),
        scripts(census),
        strings(census),
        other(census)
    { }
    ByJSType(ByJSType&& rhs)
      : total_(rhs.total_),
        objects(Move(rhs.objects)),
        scripts(move(rhs.scripts)),
        strings(move(rhs.strings)),
        other(move(rhs.other))
    { }
    ByJSType& operator=(ByJSType&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~ByJSType();
        new (this) ByJSType(Move(rhs));
        return *this;
    }

    bool init(Census& census) {
        return objects.init(census) &&
               scripts.init(census) &&
               strings.init(census) &&
               other.init(census);
    }

    bool count(Census& census, const Node& node) {
        total_++;
        if (node.is<JSObject>())
            return objects.count(census, node);
         if (node.is<JSScript>() || node.is<LazyScript>() || node.is<jit::JitCode>())
            return scripts.count(census, node);
        if (node.is<JSString>())
            return strings.count(census, node);
        return other.count(census, node);
    }

    bool report(Census& census, MutableHandleValue report) {
        JSContext* cx = census.cx;

        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        RootedValue objectsReport(cx);
        if (!objects.report(census, &objectsReport) ||
            !DefineProperty(cx, obj, cx->names().objects, objectsReport))
            return false;

        RootedValue scriptsReport(cx);
        if (!scripts.report(census, &scriptsReport) ||
            !DefineProperty(cx, obj, cx->names().scripts, scriptsReport))
            return false;

        RootedValue stringsReport(cx);
        if (!strings.report(census, &stringsReport) ||
            !DefineProperty(cx, obj, cx->names().strings, stringsReport))
            return false;

        RootedValue otherReport(cx);
        if (!other.report(census, &otherReport) ||
            !DefineProperty(cx, obj, cx->names().other, otherReport))
            return false;

        report.setObject(*obj);
        return true;
    }
};

// An assorter that categorizes nodes that are JSObjects by their class, and
// places all other nodes in an 'other' category. The template arguments must be
// assorter types; each JSObject class gets an EachClass assorter, and the
// 'other' category gets an EachOther assorter.
template<typename EachClass = Tally,
         typename EachOther = Tally>
class ByObjectClass {
    size_t total_;

    // A hash policy that compares js::Classes by name.
    struct HashPolicy {
        typedef const js::Class* Lookup;
        static js::HashNumber hash(Lookup l) { return mozilla::HashString(l->name); }
        static bool match(const js::Class* key, Lookup lookup) {
            return strcmp(key->name, lookup->name) == 0;
        }
    };

    // A table mapping classes to their counts. Note that this table treats
    // js::Class instances with the same name as equal keys. If you have several
    // js::Classes with equal names (and we do; as of this writing there were
    // six named "Object"), you will get several different Classes being counted
    // in the same table entry.
    typedef HashMap<const js::Class*, EachClass, HashPolicy, SystemAllocPolicy> Table;
    typedef typename Table::Entry Entry;
    Table table;
    EachOther other;

    static int compareEntries(const void* lhsVoid, const void* rhsVoid) {
        size_t lhs = (*static_cast<const Entry * const*>(lhsVoid))->value().total();
        size_t rhs = (*static_cast<const Entry * const*>(rhsVoid))->value().total();

        // qsort sorts in "ascending" order, so we should describe entries with
        // smaller counts as being "greater than" entries with larger counts. We
        // don't want to just subtract the counts, as they're unsigned.
        if (lhs < rhs)
            return 1;
        if (lhs > rhs)
            return -1;
        return 0;
    }

  public:
    explicit ByObjectClass(Census& census) : total_(0), other(census) { }
    ByObjectClass(ByObjectClass&& rhs)
      : total_(rhs.total_), table(Move(rhs.table)), other(Move(rhs.other))
    { }
    ByObjectClass& operator=(ByObjectClass&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~ByObjectClass();
        new (this) ByObjectClass(Move(rhs));
        return *this;
    }

    bool init(Census& census) { return table.init() && other.init(census); }

    bool count(Census& census, const Node& node) {
        total_++;
        if (!node.is<JSObject>())
            return other.count(census, node);

        const js::Class* key = node.as<JSObject>()->getClass();
        typename Table::AddPtr p = table.lookupForAdd(key);
        if (!p) {
            if (!table.add(p, key, EachClass(census)))
                return false;
            if (!p->value().init(census))
                return false;
        }
        return p->value().count(census, node);
    }

    size_t total() const { return total_; }

    bool report(Census& census, MutableHandleValue report) {
        JSContext* cx = census.cx;

        // Build a vector of pointers to entries; sort by total; and then use
        // that to build the result object. This makes the ordering of entries
        // more interesting, and a little less non-deterministic.
        mozilla::Vector<Entry*> entries;
        if (!entries.reserve(table.count()))
            return false;
        for (typename Table::Range r = table.all(); !r.empty(); r.popFront())
            entries.infallibleAppend(&r.front());
        qsort(entries.begin(), entries.length(), sizeof(*entries.begin()), compareEntries);

        // Now build the result by iterating over the sorted vector.
        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;
        for (Entry** entryPtr = entries.begin(); entryPtr < entries.end(); entryPtr++) {
            Entry& entry = **entryPtr;
            EachClass& assorter = entry.value();
            RootedValue assorterReport(cx);
            if (!assorter.report(census, &assorterReport))
                return false;

            const char* name = entry.key()->name;
            MOZ_ASSERT(name);
            JSAtom* atom = Atomize(census.cx, name, strlen(name));
            if (!atom)
                return false;
            RootedId entryId(cx, AtomToId(atom));

#ifdef DEBUG
            // We have multiple js::Classes out there with the same name (for
            // example, JSObject::class_, Debugger.Object, and CollatorClass are
            // all "Object"), so let's make sure our hash table treats them all
            // as equivalent.
            bool has;
            if (!HasOwnProperty(cx, obj, entryId, &has))
                return false;
            if (has) {
                fprintf(stderr, "already has own property '%s'\n", name);
                MOZ_ASSERT(!has);
            }
#endif

            if (!DefineProperty(cx, obj, entryId, assorterReport))
                return false;
        }

        report.setObject(*obj);
        return true;
    }
};


// An assorter that categorizes nodes by their ubi::Node::typeName.
template<typename EachType = Tally>
class ByUbinodeType {
    size_t total_;

    // Note that, because ubi::Node::typeName promises to return a specific
    // pointer, not just any string whose contents are correct, we can use their
    // addresses as hash table keys.
    typedef HashMap<const char16_t*, EachType, DefaultHasher<const char16_t*>,
                    SystemAllocPolicy> Table;
    typedef typename Table::Entry Entry;
    Table table;

  public:
    explicit ByUbinodeType(Census& census) : total_(0) { }
    ByUbinodeType(ByUbinodeType&& rhs) : total_(rhs.total_), table(Move(rhs.table)) { }
    ByUbinodeType& operator=(ByUbinodeType&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~ByUbinodeType();
        new (this) ByUbinodeType(Move(rhs));
        return *this;
    }

    bool init(Census& census) { return table.init(); }

    bool count(Census& census, const Node& node) {
        total_++;
        const char16_t* key = node.typeName();
        typename Table::AddPtr p = table.lookupForAdd(key);
        if (!p) {
            if (!table.add(p, key, EachType(census)))
                return false;
            if (!p->value().init(census))
                return false;
        }
        return p->value().count(census, node);
    }

    size_t total() const { return total_; }

    static int compareEntries(const void* lhsVoid, const void* rhsVoid) {
        size_t lhs = (*static_cast<const Entry * const*>(lhsVoid))->value().total();
        size_t rhs = (*static_cast<const Entry * const*>(rhsVoid))->value().total();

        // qsort sorts in "ascending" order, so we should describe entries with
        // smaller counts as being "greater than" entries with larger counts. We
        // don't want to just subtract the counts, as they're unsigned.
        if (lhs < rhs)
            return 1;
        if (lhs > rhs)
            return -1;
        return 0;
    }

    bool report(Census& census, MutableHandleValue report) {
        JSContext* cx = census.cx;

        // Build a vector of pointers to entries; sort by total; and then use
        // that to build the result object. This makes the ordering of entries
        // more interesting, and a little less non-deterministic.
        mozilla::Vector<Entry*> entries;
        if (!entries.reserve(table.count()))
            return false;
        for (typename Table::Range r = table.all(); !r.empty(); r.popFront())
            entries.infallibleAppend(&r.front());
        qsort(entries.begin(), entries.length(), sizeof(*entries.begin()), compareEntries);

        // Now build the result by iterating over the sorted vector.
        RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;
        for (Entry** entryPtr = entries.begin(); entryPtr < entries.end(); entryPtr++) {
            Entry& entry = **entryPtr;
            EachType& assorter = entry.value();
            RootedValue assorterReport(cx);
            if (!assorter.report(census, &assorterReport))
                return false;

            const char16_t* name = entry.key();
            MOZ_ASSERT(name);
            JSAtom* atom = AtomizeChars(cx, name, js_strlen(name));
            if (!atom)
                return false;
            RootedId entryId(cx, AtomToId(atom));

            if (!DefineProperty(cx, obj, entryId, assorterReport))
                return false;
        }

        report.setObject(*obj);
        return true;
    }
};


// A BreadthFirst handler type that conducts a census, using Assorter
// to categorize and count each node.
template<typename Assorter>
class CensusHandler {
    Census& census;
    Assorter assorter;

  public:
    explicit CensusHandler(Census& census) : census(census), assorter(census) { }

    bool init(Census& census) { return assorter.init(census); }
    bool report(Census& census, MutableHandleValue report) {
        return assorter.report(census, report);
    }

    // This class needs to retain no per-node data.
    class NodeData { };

    bool operator() (BreadthFirst<CensusHandler>& traversal,
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

        if (census.debuggeeZones.has(zone)) {
            return assorter.count(census, referent);
        }

        if (zone == census.atomsZone) {
            traversal.abandonReferent();
            return assorter.count(census, referent);
        }

        traversal.abandonReferent();
        return true;
    }
};

// The assorter that Debugger.Memory.prototype.takeCensus uses by default.
// (Eventually, we hope to add parameters that let you specify dynamically how
// the census should assort the nodes it finds.) Categorize nodes by JS type,
// and then objects by object class.
typedef ByJSType<ByObjectClass<Tally>, Tally, Tally, ByUbinodeType<Tally> > DefaultAssorter;

// A traversal that conducts a census using DefaultAssorter.
typedef CensusHandler<DefaultAssorter> DefaultCensusHandler;
typedef BreadthFirst<DefaultCensusHandler> DefaultCensusTraversal;

} // namespace dbg
} // namespace js

bool
DebuggerMemory::takeCensus(JSContext* cx, unsigned argc, Value* vp)
{
    THIS_DEBUGGER_MEMORY(cx, argc, vp, "Debugger.Memory.prototype.census", args, memory);

    dbg::Census census(cx);
    if (!census.init())
        return false;

    dbg::DefaultCensusHandler handler(census);
    if (!handler.init(census))
        return false;

    Debugger* dbg = memory->getDebugger();
    RootedObject dbgObj(cx, dbg->object);

    // Populate our target set of debuggee zones.
    for (GlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty(); r.popFront()) {
        if (!census.debuggeeZones.put(r.front()->zone()))
            return false;
    }

    {
        Maybe<JS::AutoCheckCannotGC> maybeNoGC;
        JS::ubi::RootList rootList(cx, maybeNoGC);
        if (!rootList.init(dbgObj))
            return false;

        dbg::DefaultCensusTraversal traversal(cx, handler, maybeNoGC.ref());
        if (!traversal.init())
            return false;
        traversal.wantNames = false;

        if (!traversal.addStart(JS::ubi::Node(&rootList)) ||
            !traversal.traverse())
        {
            return false;
        }
    }

    return handler.report(census, args.rval());
}



/* Debugger.Memory property and method tables. */


/* static */ const JSPropertySpec DebuggerMemory::properties[] = {
    JS_PSGS("trackingAllocationSites", getTrackingAllocationSites, setTrackingAllocationSites, 0),
    JS_PSGS("maxAllocationsLogLength", getMaxAllocationsLogLength, setMaxAllocationsLogLength, 0),
    JS_PSGS("allocationSamplingProbability", getAllocationSamplingProbability, setAllocationSamplingProbability, 0),
    JS_PSG("allocationsLogOverflowed", getAllocationsLogOverflowed, 0),
    JS_PS_END
};

/* static */ const JSFunctionSpec DebuggerMemory::methods[] = {
    JS_FN("drainAllocationsLog", DebuggerMemory::drainAllocationsLog, 0, 0),
    JS_FN("takeCensus", takeCensus, 0, 0),
    JS_FS_END
};
