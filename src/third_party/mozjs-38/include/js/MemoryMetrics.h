/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_MemoryMetrics_h
#define js_MemoryMetrics_h

// These declarations are highly likely to change in the future. Depend on them
// at your own risk.

#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"

#include <string.h>

#include "jsalloc.h"
#include "jspubtd.h"

#include "js/HashTable.h"
#include "js/Utility.h"
#include "js/Vector.h"

class nsISupports;      // Needed for ObjectPrivateVisitor.

namespace JS {

struct TabSizes
{
    enum Kind {
        Objects,
        Strings,
        Private,
        Other
    };

    TabSizes() { mozilla::PodZero(this); }

    void add(Kind kind, size_t n) {
        switch (kind) {
            case Objects: objects  += n; break;
            case Strings: strings  += n; break;
            case Private: private_ += n; break;
            case Other:   other    += n; break;
            default:      MOZ_CRASH("bad TabSizes kind");
        }
    }

    size_t objects;
    size_t strings;
    size_t private_;
    size_t other;
};

} // namespace JS

namespace js {

// In memory reporting, we have concept of "sundries", line items which are too
// small to be worth reporting individually.  Under some circumstances, a memory
// reporter gets tossed into the sundries bucket if it's smaller than
// MemoryReportingSundriesThreshold() bytes.
//
// We need to define this value here, rather than in the code which actually
// generates the memory reports, because NotableStringInfo uses this value.
JS_FRIEND_API(size_t) MemoryReportingSundriesThreshold();

// This hash policy avoids flattening ropes (which perturbs the site being
// measured and requires a JSContext) at the expense of doing a FULL ROPE COPY
// on every hash and match! Beware.
struct InefficientNonFlatteningStringHashPolicy
{
    typedef JSString* Lookup;
    static HashNumber hash(const Lookup& l);
    static bool match(const JSString* const& k, const Lookup& l);
};

struct CStringHashPolicy
{
    typedef const char* Lookup;
    static HashNumber hash(const Lookup& l);
    static bool match(const char* const& k, const Lookup& l);
};

// This file features many classes with numerous size_t fields, and each such
// class has one or more methods that need to operate on all of these fields.
// Writing these individually is error-prone -- it's easy to add a new field
// without updating all the required methods.  So we define a single macro list
// in each class to name the fields (and notable characteristics of them), and
// then use the following macros to transform those lists into the required
// methods.
//
// In some classes, one or more of the macro arguments aren't used.  We use '_'
// for those.
//
#define DECL_SIZE(kind, gc, mSize)                      size_t mSize;
#define ZERO_SIZE(kind, gc, mSize)                      mSize(0),
#define COPY_OTHER_SIZE(kind, gc, mSize)                mSize(other.mSize),
#define ADD_OTHER_SIZE(kind, gc, mSize)                 mSize += other.mSize;
#define SUB_OTHER_SIZE(kind, gc, mSize)                 MOZ_ASSERT(mSize >= other.mSize); \
                                                        mSize -= other.mSize;
#define ADD_SIZE_TO_N(kind, gc, mSize)                  n += mSize;
#define ADD_SIZE_TO_N_IF_LIVE_GC_THING(kind, gc, mSize) n += (js::gc) ? mSize : 0;
#define ADD_TO_TAB_SIZES(kind, gc, mSize)               sizes->add(JS::TabSizes::kind, mSize);

// Used to annotate which size_t fields measure live GC things and which don't.
enum {
    NotLiveGCThing = false,
    IsLiveGCThing = true
};

} // namespace js

namespace JS {

struct ClassInfo
{
#define FOR_EACH_SIZE(macro) \
    macro(Objects, IsLiveGCThing,  objectsGCHeap) \
    macro(Objects, NotLiveGCThing, objectsMallocHeapSlots) \
    macro(Objects, NotLiveGCThing, objectsMallocHeapElementsNonAsmJS) \
    macro(Objects, NotLiveGCThing, objectsMallocHeapElementsAsmJS) \
    macro(Objects, NotLiveGCThing, objectsNonHeapElementsAsmJS) \
    macro(Objects, NotLiveGCThing, objectsNonHeapElementsMapped) \
    macro(Objects, NotLiveGCThing, objectsNonHeapCodeAsmJS) \
    macro(Objects, NotLiveGCThing, objectsMallocHeapMisc) \
    \
    macro(Other,   IsLiveGCThing,  shapesGCHeapTree) \
    macro(Other,   IsLiveGCThing,  shapesGCHeapDict) \
    macro(Other,   IsLiveGCThing,  shapesGCHeapBase) \
    macro(Other,   NotLiveGCThing, shapesMallocHeapTreeTables) \
    macro(Other,   NotLiveGCThing, shapesMallocHeapDictTables) \
    macro(Other,   NotLiveGCThing, shapesMallocHeapTreeKids) \

    ClassInfo()
      : FOR_EACH_SIZE(ZERO_SIZE)
        dummy()
    {}

    void add(const ClassInfo& other) {
        FOR_EACH_SIZE(ADD_OTHER_SIZE)
    }

    void subtract(const ClassInfo& other) {
        FOR_EACH_SIZE(SUB_OTHER_SIZE)
    }

    bool isNotable() const {
        static const size_t NotabilityThreshold = 16 * 1024;
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N)
        return n >= NotabilityThreshold;
    }

    size_t sizeOfLiveGCThings() const {
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N_IF_LIVE_GC_THING)
        return n;
    }

    void addToTabSizes(TabSizes* sizes) const {
        FOR_EACH_SIZE(ADD_TO_TAB_SIZES)
    }

    FOR_EACH_SIZE(DECL_SIZE)
    int dummy;  // present just to absorb the trailing comma from FOR_EACH_SIZE(ZERO_SIZE)

#undef FOR_EACH_SIZE
};

// Holds data about a notable class (one whose combined object and shape
// instances use more than a certain amount of memory) so we can report it
// individually.
//
// The only difference between this class and ClassInfo is that this class
// holds a copy of the filename.
struct NotableClassInfo : public ClassInfo
{
    NotableClassInfo();
    NotableClassInfo(const char* className, const ClassInfo& info);
    NotableClassInfo(NotableClassInfo&& info);
    NotableClassInfo& operator=(NotableClassInfo&& info);

    ~NotableClassInfo() {
        js_free(className_);
    }

    char* className_;

  private:
    NotableClassInfo(const NotableClassInfo& info) = delete;
};

// Data for tracking JIT-code memory usage.
struct CodeSizes
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, ion) \
    macro(_, _, baseline) \
    macro(_, _, regexp) \
    macro(_, _, other) \
    macro(_, _, unused)

    CodeSizes()
      : FOR_EACH_SIZE(ZERO_SIZE)
        dummy()
    {}

    FOR_EACH_SIZE(DECL_SIZE)
    int dummy;  // present just to absorb the trailing comma from FOR_EACH_SIZE(ZERO_SIZE)

#undef FOR_EACH_SIZE
};

// Data for tracking GC memory usage.
struct GCSizes
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, marker) \
    macro(_, _, nurseryCommitted) \
    macro(_, _, nurseryDecommitted) \
    macro(_, _, nurseryHugeSlots) \
    macro(_, _, storeBufferVals) \
    macro(_, _, storeBufferCells) \
    macro(_, _, storeBufferSlots) \
    macro(_, _, storeBufferWholeCells) \
    macro(_, _, storeBufferRelocVals) \
    macro(_, _, storeBufferRelocCells) \
    macro(_, _, storeBufferGenerics)

    GCSizes()
      : FOR_EACH_SIZE(ZERO_SIZE)
        dummy()
    {}

    FOR_EACH_SIZE(DECL_SIZE)
    int dummy;  // present just to absorb the trailing comma from FOR_EACH_SIZE(ZERO_SIZE)

#undef FOR_EACH_SIZE
};

// This class holds information about the memory taken up by identical copies of
// a particular string.  Multiple JSStrings may have their sizes aggregated
// together into one StringInfo object.  Note that two strings with identical
// chars will not be aggregated together if one is a short string and the other
// is not.
struct StringInfo
{
#define FOR_EACH_SIZE(macro) \
    macro(Strings, IsLiveGCThing,  gcHeapLatin1) \
    macro(Strings, IsLiveGCThing,  gcHeapTwoByte) \
    macro(Strings, NotLiveGCThing, mallocHeapLatin1) \
    macro(Strings, NotLiveGCThing, mallocHeapTwoByte)

    StringInfo()
      : FOR_EACH_SIZE(ZERO_SIZE)
        numCopies(0)
    {}

    void add(const StringInfo& other) {
        FOR_EACH_SIZE(ADD_OTHER_SIZE);
        numCopies++;
    }

    void subtract(const StringInfo& other) {
        FOR_EACH_SIZE(SUB_OTHER_SIZE);
        numCopies--;
    }

    bool isNotable() const {
        static const size_t NotabilityThreshold = 16 * 1024;
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N)
        return n >= NotabilityThreshold;
    }

    size_t sizeOfLiveGCThings() const {
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N_IF_LIVE_GC_THING)
        return n;
    }

    void addToTabSizes(TabSizes* sizes) const {
        FOR_EACH_SIZE(ADD_TO_TAB_SIZES)
    }

    FOR_EACH_SIZE(DECL_SIZE)
    uint32_t numCopies;     // How many copies of the string have we seen?

#undef FOR_EACH_SIZE
};

// Holds data about a notable string (one which, counting all duplicates, uses
// more than a certain amount of memory) so we can report it individually.
//
// The only difference between this class and StringInfo is that
// NotableStringInfo holds a copy of some or all of the string's chars.
struct NotableStringInfo : public StringInfo
{
    static const size_t MAX_SAVED_CHARS = 1024;

    NotableStringInfo();
    NotableStringInfo(JSString* str, const StringInfo& info);
    NotableStringInfo(NotableStringInfo&& info);
    NotableStringInfo& operator=(NotableStringInfo&& info);

    ~NotableStringInfo() {
        js_free(buffer);
    }

    char* buffer;
    size_t length;

  private:
    NotableStringInfo(const NotableStringInfo& info) = delete;
};

// This class holds information about the memory taken up by script sources
// from a particular file.
struct ScriptSourceInfo
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, compressed) \
    macro(_, _, uncompressed) \
    macro(_, _, misc)

    ScriptSourceInfo()
      : FOR_EACH_SIZE(ZERO_SIZE)
        numScripts(0)
    {}

    void add(const ScriptSourceInfo& other) {
        FOR_EACH_SIZE(ADD_OTHER_SIZE)
        numScripts++;
    }

    void subtract(const ScriptSourceInfo& other) {
        FOR_EACH_SIZE(SUB_OTHER_SIZE)
        numScripts--;
    }

    bool isNotable() const {
        static const size_t NotabilityThreshold = 16 * 1024;
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N)
        return n >= NotabilityThreshold;
    }

    FOR_EACH_SIZE(DECL_SIZE)
    uint32_t numScripts;    // How many ScriptSources come from this file? (It
                            // can be more than one in XML files that have
                            // multiple scripts in CDATA sections.)
#undef FOR_EACH_SIZE
};

// Holds data about a notable script source file (one whose combined
// script sources use more than a certain amount of memory) so we can report it
// individually.
//
// The only difference between this class and ScriptSourceInfo is that this
// class holds a copy of the filename.
struct NotableScriptSourceInfo : public ScriptSourceInfo
{
    NotableScriptSourceInfo();
    NotableScriptSourceInfo(const char* filename, const ScriptSourceInfo& info);
    NotableScriptSourceInfo(NotableScriptSourceInfo&& info);
    NotableScriptSourceInfo& operator=(NotableScriptSourceInfo&& info);

    ~NotableScriptSourceInfo() {
        js_free(filename_);
    }

    char* filename_;

  private:
    NotableScriptSourceInfo(const NotableScriptSourceInfo& info) = delete;
};

// These measurements relate directly to the JSRuntime, and not to zones and
// compartments within it.
struct RuntimeSizes
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, object) \
    macro(_, _, atomsTable) \
    macro(_, _, contexts) \
    macro(_, _, dtoa) \
    macro(_, _, temporary) \
    macro(_, _, interpreterStack) \
    macro(_, _, mathCache) \
    macro(_, _, uncompressedSourceCache) \
    macro(_, _, compressedSourceSet) \
    macro(_, _, scriptData) \

    RuntimeSizes()
      : FOR_EACH_SIZE(ZERO_SIZE)
        scriptSourceInfo(),
        code(),
        gc(),
        notableScriptSources()
    {
        allScriptSources = js_new<ScriptSourcesHashMap>();
        if (!allScriptSources || !allScriptSources->init())
            MOZ_CRASH("oom");
    }

    ~RuntimeSizes() {
        // |allScriptSources| is usually deleted and set to nullptr before this
        // destructor runs. But there are failure cases due to OOMs that may
        // prevent that, so it doesn't hurt to try again here.
        js_delete(allScriptSources);
    }

    // The script source measurements in |scriptSourceInfo| are initially for
    // all script sources.  At the end, if the measurement granularity is
    // FineGrained, we subtract the measurements of the notable script sources
    // and move them into |notableScriptSources|.
    FOR_EACH_SIZE(DECL_SIZE)
    ScriptSourceInfo    scriptSourceInfo;
    CodeSizes           code;
    GCSizes             gc;

    typedef js::HashMap<const char*, ScriptSourceInfo,
                        js::CStringHashPolicy,
                        js::SystemAllocPolicy> ScriptSourcesHashMap;

    // |allScriptSources| is only used transiently.  During the reporting phase
    // it is filled with info about every script source in the runtime.  It's
    // then used to fill in |notableScriptSources| (which actually gets
    // reported), and immediately discarded afterwards.
    ScriptSourcesHashMap* allScriptSources;
    js::Vector<NotableScriptSourceInfo, 0, js::SystemAllocPolicy> notableScriptSources;

#undef FOR_EACH_SIZE
};

struct GCThingSizes
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, object) \
    macro(_, _, script) \
    macro(_, _, lazyScript) \
    macro(_, _, shape) \
    macro(_, _, baseShape) \
    macro(_, _, objectGroup) \
    macro(_, _, string) \
    macro(_, _, symbol) \
    macro(_, _, jitcode) \

    GCThingSizes()
      : FOR_EACH_SIZE(ZERO_SIZE)
        dummy()
    {}

    GCThingSizes(GCThingSizes&& other)
      : FOR_EACH_SIZE(COPY_OTHER_SIZE)
        dummy()
    {}

    void addToKind(JSGCTraceKind kind, intptr_t n) {
        switch (kind) {
          case JSTRACE_OBJECT:       object += n;      break;
          case JSTRACE_STRING:       string += n;      break;
          case JSTRACE_SYMBOL:       symbol += n;      break;
          case JSTRACE_SCRIPT:       script += n;      break;
          case JSTRACE_SHAPE:        shape += n;       break;
          case JSTRACE_BASE_SHAPE:   baseShape += n;   break;
          case JSTRACE_JITCODE:      jitcode += n;     break;
          case JSTRACE_LAZY_SCRIPT:  lazyScript += n;  break;
          case JSTRACE_OBJECT_GROUP: objectGroup += n; break;
          default:
            MOZ_CRASH("Bad trace kind for GCThingSizes");
        }
    }

    void addSizes(const GCThingSizes& other) {
        FOR_EACH_SIZE(ADD_OTHER_SIZE)
    }

    size_t totalSize() const {
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N)
        return n;
    }

    FOR_EACH_SIZE(DECL_SIZE)
    int dummy;  // present just to absorb the trailing comma from FOR_EACH_SIZE(ZERO_SIZE)

#undef FOR_EACH_SIZE
};

struct ZoneStats
{
#define FOR_EACH_SIZE(macro) \
    macro(Other,   IsLiveGCThing,  symbolsGCHeap) \
    macro(Other,   NotLiveGCThing, gcHeapArenaAdmin) \
    macro(Other,   IsLiveGCThing,  lazyScriptsGCHeap) \
    macro(Other,   NotLiveGCThing, lazyScriptsMallocHeap) \
    macro(Other,   IsLiveGCThing,  jitCodesGCHeap) \
    macro(Other,   IsLiveGCThing,  objectGroupsGCHeap) \
    macro(Other,   NotLiveGCThing, objectGroupsMallocHeap) \
    macro(Other,   NotLiveGCThing, typePool) \
    macro(Other,   NotLiveGCThing, baselineStubsOptimized) \

    ZoneStats()
      : FOR_EACH_SIZE(ZERO_SIZE)
        unusedGCThings(),
        stringInfo(),
        extra(),
        allStrings(nullptr),
        notableStrings(),
        isTotals(true)
    {}

    ZoneStats(ZoneStats&& other)
      : FOR_EACH_SIZE(COPY_OTHER_SIZE)
        unusedGCThings(mozilla::Move(other.unusedGCThings)),
        stringInfo(mozilla::Move(other.stringInfo)),
        extra(other.extra),
        allStrings(other.allStrings),
        notableStrings(mozilla::Move(other.notableStrings)),
        isTotals(other.isTotals)
    {
        other.allStrings = nullptr;
        MOZ_ASSERT(!other.isTotals);
    }

    ~ZoneStats() {
        // |allStrings| is usually deleted and set to nullptr before this
        // destructor runs. But there are failure cases due to OOMs that may
        // prevent that, so it doesn't hurt to try again here.
        js_delete(allStrings);
    }

    bool initStrings(JSRuntime* rt);

    void addSizes(const ZoneStats& other) {
        MOZ_ASSERT(isTotals);
        FOR_EACH_SIZE(ADD_OTHER_SIZE)
        unusedGCThings.addSizes(other.unusedGCThings);
        stringInfo.add(other.stringInfo);
    }

    size_t sizeOfLiveGCThings() const {
        MOZ_ASSERT(isTotals);
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N_IF_LIVE_GC_THING)
        n += stringInfo.sizeOfLiveGCThings();
        return n;
    }

    void addToTabSizes(JS::TabSizes* sizes) const {
        MOZ_ASSERT(isTotals);
        FOR_EACH_SIZE(ADD_TO_TAB_SIZES)
        sizes->add(JS::TabSizes::Other, unusedGCThings.totalSize());
        stringInfo.addToTabSizes(sizes);
    }

    // These string measurements are initially for all strings.  At the end,
    // if the measurement granularity is FineGrained, we subtract the
    // measurements of the notable script sources and move them into
    // |notableStrings|.
    FOR_EACH_SIZE(DECL_SIZE)
    GCThingSizes unusedGCThings;
    StringInfo stringInfo;
    void* extra;    // This field can be used by embedders.

    typedef js::HashMap<JSString*, StringInfo,
                        js::InefficientNonFlatteningStringHashPolicy,
                        js::SystemAllocPolicy> StringsHashMap;

    // |allStrings| is only used transiently.  During the zone traversal it is
    // filled with info about every string in the zone.  It's then used to fill
    // in |notableStrings| (which actually gets reported), and immediately
    // discarded afterwards.
    StringsHashMap* allStrings;
    js::Vector<NotableStringInfo, 0, js::SystemAllocPolicy> notableStrings;
    bool isTotals;

#undef FOR_EACH_SIZE
};

struct CompartmentStats
{
#define FOR_EACH_SIZE(macro) \
    macro(Private, NotLiveGCThing, objectsPrivate) \
    macro(Other,   IsLiveGCThing,  scriptsGCHeap) \
    macro(Other,   NotLiveGCThing, scriptsMallocHeapData) \
    macro(Other,   NotLiveGCThing, baselineData) \
    macro(Other,   NotLiveGCThing, baselineStubsFallback) \
    macro(Other,   NotLiveGCThing, ionData) \
    macro(Other,   NotLiveGCThing, typeInferenceTypeScripts) \
    macro(Other,   NotLiveGCThing, typeInferenceAllocationSiteTables) \
    macro(Other,   NotLiveGCThing, typeInferenceArrayTypeTables) \
    macro(Other,   NotLiveGCThing, typeInferenceObjectTypeTables) \
    macro(Other,   NotLiveGCThing, compartmentObject) \
    macro(Other,   NotLiveGCThing, compartmentTables) \
    macro(Other,   NotLiveGCThing, innerViewsTable) \
    macro(Other,   NotLiveGCThing, lazyArrayBuffersTable) \
    macro(Other,   NotLiveGCThing, crossCompartmentWrappersTable) \
    macro(Other,   NotLiveGCThing, regexpCompartment) \
    macro(Other,   NotLiveGCThing, savedStacksSet)

    CompartmentStats()
      : FOR_EACH_SIZE(ZERO_SIZE)
        classInfo(),
        extra(),
        allClasses(nullptr),
        notableClasses(),
        isTotals(true)
    {}

    CompartmentStats(CompartmentStats&& other)
      : FOR_EACH_SIZE(COPY_OTHER_SIZE)
        classInfo(mozilla::Move(other.classInfo)),
        extra(other.extra),
        allClasses(other.allClasses),
        notableClasses(mozilla::Move(other.notableClasses)),
        isTotals(other.isTotals)
    {
        other.allClasses = nullptr;
        MOZ_ASSERT(!other.isTotals);
    }

    ~CompartmentStats() {
        // |allClasses| is usually deleted and set to nullptr before this
        // destructor runs. But there are failure cases due to OOMs that may
        // prevent that, so it doesn't hurt to try again here.
        js_delete(allClasses);
    }

    bool initClasses(JSRuntime* rt);

    void addSizes(const CompartmentStats& other) {
        MOZ_ASSERT(isTotals);
        FOR_EACH_SIZE(ADD_OTHER_SIZE)
        classInfo.add(other.classInfo);
    }

    size_t sizeOfLiveGCThings() const {
        MOZ_ASSERT(isTotals);
        size_t n = 0;
        FOR_EACH_SIZE(ADD_SIZE_TO_N_IF_LIVE_GC_THING)
        n += classInfo.sizeOfLiveGCThings();
        return n;
    }

    void addToTabSizes(TabSizes* sizes) const {
        MOZ_ASSERT(isTotals);
        FOR_EACH_SIZE(ADD_TO_TAB_SIZES);
        classInfo.addToTabSizes(sizes);
    }

    // The class measurements in |classInfo| are initially for all classes.  At
    // the end, if the measurement granularity is FineGrained, we subtract the
    // measurements of the notable classes and move them into |notableClasses|.
    FOR_EACH_SIZE(DECL_SIZE)
    ClassInfo   classInfo;
    void*       extra;     // This field can be used by embedders.

    typedef js::HashMap<const char*, ClassInfo,
                        js::CStringHashPolicy,
                        js::SystemAllocPolicy> ClassesHashMap;

    // These are similar to |allStrings| and |notableStrings| in ZoneStats.
    ClassesHashMap* allClasses;
    js::Vector<NotableClassInfo, 0, js::SystemAllocPolicy> notableClasses;
    bool isTotals;

#undef FOR_EACH_SIZE
};

typedef js::Vector<CompartmentStats, 0, js::SystemAllocPolicy> CompartmentStatsVector;
typedef js::Vector<ZoneStats, 0, js::SystemAllocPolicy> ZoneStatsVector;

struct RuntimeStats
{
#define FOR_EACH_SIZE(macro) \
    macro(_, _, gcHeapChunkTotal) \
    macro(_, _, gcHeapDecommittedArenas) \
    macro(_, _, gcHeapUnusedChunks) \
    macro(_, _, gcHeapUnusedArenas) \
    macro(_, _, gcHeapChunkAdmin) \
    macro(_, _, gcHeapGCThings) \

    explicit RuntimeStats(mozilla::MallocSizeOf mallocSizeOf)
      : FOR_EACH_SIZE(ZERO_SIZE)
        runtime(),
        cTotals(),
        zTotals(),
        compartmentStatsVector(),
        zoneStatsVector(),
        currZoneStats(nullptr),
        mallocSizeOf_(mallocSizeOf)
    {}

    // Here's a useful breakdown of the GC heap.
    //
    // - rtStats.gcHeapChunkTotal
    //   - decommitted bytes
    //     - rtStats.gcHeapDecommittedArenas (decommitted arenas in non-empty chunks)
    //   - unused bytes
    //     - rtStats.gcHeapUnusedChunks (empty chunks)
    //     - rtStats.gcHeapUnusedArenas (empty arenas within non-empty chunks)
    //     - rtStats.zTotals.unusedGCThings (empty GC thing slots within non-empty arenas)
    //   - used bytes
    //     - rtStats.gcHeapChunkAdmin
    //     - rtStats.zTotals.gcHeapArenaAdmin
    //     - rtStats.gcHeapGCThings (in-use GC things)
    //       == rtStats.zTotals.sizeOfLiveGCThings() + rtStats.cTotals.sizeOfLiveGCThings()
    //
    // It's possible that some arenas in empty chunks may be decommitted, but
    // we don't count those under rtStats.gcHeapDecommittedArenas because (a)
    // it's rare, and (b) this means that rtStats.gcHeapUnusedChunks is a
    // multiple of the chunk size, which is good.

    FOR_EACH_SIZE(DECL_SIZE)

    RuntimeSizes runtime;

    CompartmentStats cTotals;   // The sum of this runtime's compartments' measurements.
    ZoneStats zTotals;          // The sum of this runtime's zones' measurements.

    CompartmentStatsVector compartmentStatsVector;
    ZoneStatsVector zoneStatsVector;

    ZoneStats* currZoneStats;

    mozilla::MallocSizeOf mallocSizeOf_;

    virtual void initExtraCompartmentStats(JSCompartment* c, CompartmentStats* cstats) = 0;
    virtual void initExtraZoneStats(JS::Zone* zone, ZoneStats* zstats) = 0;

#undef FOR_EACH_SIZE
};

class ObjectPrivateVisitor
{
  public:
    // Within CollectRuntimeStats, this method is called for each JS object
    // that has an nsISupports pointer.
    virtual size_t sizeOfIncludingThis(nsISupports* aSupports) = 0;

    // A callback that gets a JSObject's nsISupports pointer, if it has one.
    // Note: this function does *not* addref |iface|.
    typedef bool(*GetISupportsFun)(JSObject* obj, nsISupports** iface);
    GetISupportsFun getISupports_;

    explicit ObjectPrivateVisitor(GetISupportsFun getISupports)
      : getISupports_(getISupports)
    {}
};

extern JS_PUBLIC_API(bool)
CollectRuntimeStats(JSRuntime* rt, RuntimeStats* rtStats, ObjectPrivateVisitor* opv, bool anonymize);

extern JS_PUBLIC_API(size_t)
SystemCompartmentCount(JSRuntime* rt);

extern JS_PUBLIC_API(size_t)
UserCompartmentCount(JSRuntime* rt);

extern JS_PUBLIC_API(size_t)
PeakSizeOfTemporary(const JSRuntime* rt);

extern JS_PUBLIC_API(bool)
AddSizeOfTab(JSRuntime* rt, JS::HandleObject obj, mozilla::MallocSizeOf mallocSizeOf,
             ObjectPrivateVisitor* opv, TabSizes* sizes);

} // namespace JS

#undef DECL_SIZE
#undef ZERO_SIZE
#undef COPY_OTHER_SIZE
#undef ADD_OTHER_SIZE
#undef SUB_OTHER_SIZE
#undef ADD_SIZE_TO_N
#undef ADD_SIZE_TO_N_IF_LIVE_GC_THING
#undef ADD_TO_TAB_SIZES

#endif /* js_MemoryMetrics_h */
