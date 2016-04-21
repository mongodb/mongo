/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_GC_TRACE

#include "gc/GCTrace.h"

#include <stdio.h>
#include <string.h>

#include "gc/GCTraceFormat.h"

#include "js/HashTable.h"

using namespace js;
using namespace js::gc;

JS_STATIC_ASSERT(AllocKinds == unsigned(AllocKind::LIMIT));
JS_STATIC_ASSERT(LastObjectAllocKind == unsigned(AllocKind::OBJECT_LAST));

static FILE* gcTraceFile = nullptr;

static HashSet<const Class*, DefaultHasher<const Class*>, SystemAllocPolicy> tracedClasses;
static HashSet<const ObjectGroup*, DefaultHasher<const ObjectGroup*>, SystemAllocPolicy> tracedGroups;

static inline void
WriteWord(uint64_t data)
{
    if (gcTraceFile)
        fwrite(&data, sizeof(data), 1, gcTraceFile);
}

static inline void
TraceEvent(GCTraceEvent event, uint64_t payload = 0, uint8_t extra = 0)
{
    MOZ_ASSERT(event < GCTraceEventCount);
    MOZ_ASSERT((payload >> TracePayloadBits) == 0);
    WriteWord((uint64_t(event) << TraceEventShift) |
               (uint64_t(extra) << TraceExtraShift) | payload);
}

static inline void
TraceAddress(const void* p)
{
    TraceEvent(TraceDataAddress, uint64_t(p));
}

static inline void
TraceInt(uint32_t data)
{
    TraceEvent(TraceDataInt, data);
}

static void
TraceString(const char* string)
{
    JS_STATIC_ASSERT(sizeof(char) == 1);

    size_t length = strlen(string);
    const unsigned charsPerWord = sizeof(uint64_t);
    unsigned wordCount = (length + charsPerWord - 1) / charsPerWord;

    TraceEvent(TraceDataString, length);
    for (unsigned i = 0; i < wordCount; ++i) {
        union
        {
            uint64_t word;
            char chars[charsPerWord];
        } data;
        strncpy(data.chars, string + (i * charsPerWord), charsPerWord);
        WriteWord(data.word);
    }
}

bool
js::gc::InitTrace(GCRuntime& gc)
{
    /* This currently does not support multiple runtimes. */
    MOZ_ALWAYS_TRUE(!gcTraceFile);

    char* filename = getenv("JS_GC_TRACE");
    if (!filename)
        return true;

    if (!tracedClasses.init() || !tracedTypes.init()) {
        FinishTrace();
        return false;
    }

    gcTraceFile = fopen(filename, "w");
    if (!gcTraceFile) {
        FinishTrace();
        return false;
    }

    TraceEvent(TraceEventInit, 0, TraceFormatVersion);

    /* Trace information about thing sizes. */
    for (auto kind : AllAllocKinds())
        TraceEvent(TraceEventThingSize, Arena::thingSize(kind));

    return true;
}

void
js::gc::FinishTrace()
{
    if (gcTraceFile) {
        fclose(gcTraceFile);
        gcTraceFile = nullptr;
    }
    tracedClasses.finish();
    tracedTypes.finish();
}

bool
js::gc::TraceEnabled()
{
    return gcTraceFile != nullptr;
}

void
js::gc::TraceNurseryAlloc(Cell* thing, size_t size)
{
    if (thing) {
        /* We don't have AllocKind here, but we can work it out from size. */
        unsigned slots = (size - sizeof(JSObject)) / sizeof(JS::Value);
        AllocKind kind = GetBackgroundAllocKind(GetGCObjectKind(slots));
        TraceEvent(TraceEventNurseryAlloc, uint64_t(thing), kind);
    }
}

void
js::gc::TraceTenuredAlloc(Cell* thing, AllocKind kind)
{
    if (thing)
        TraceEvent(TraceEventTenuredAlloc, uint64_t(thing), kind);
}

static void
MaybeTraceClass(const Class* clasp)
{
    if (tracedClasses.has(clasp))
        return;

    TraceEvent(TraceEventClassInfo, uint64_t(clasp));
    TraceString(clasp->name);
    TraceInt(clasp->flags);
    TraceInt(clasp->finalize != nullptr);

    MOZ_ALWAYS_TRUE(tracedClasses.put(clasp));
}

static void
MaybeTraceGroup(ObjectGroup* group)
{
    if (tracedGroups.has(group))
        return;

    MaybeTraceClass(group->clasp());
    TraceEvent(TraceEventGroupInfo, uint64_t(group));
    TraceAddress(group->clasp());
    TraceInt(group->flags());

    MOZ_ALWAYS_TRUE(tracedGroups.put(group));
}

void
js::gc::TraceTypeNewScript(ObjectGroup* group)
{
    const size_t bufLength = 128;
    static char buffer[bufLength];
    MOZ_ASSERT(group->hasNewScript());
    JSAtom* funName = group->newScript()->fun->displayAtom();
    if (!funName)
        return;

    size_t length = funName->length();
    MOZ_ALWAYS_TRUE(length < bufLength);
    CopyChars(reinterpret_cast<Latin1Char*>(buffer), *funName);
    buffer[length] = 0;

    TraceEvent(TraceEventTypeNewScript, uint64_t(group));
    TraceString(buffer);
}

void
js::gc::TraceCreateObject(JSObject* object)
{
    if (!gcTraceFile)
        return;

    ObjectGroup* group = object->group();
    MaybeTraceGroup(group);
    TraceEvent(TraceEventCreateObject, uint64_t(object));
    TraceAddress(group);
}

void
js::gc::TraceMinorGCStart()
{
    TraceEvent(TraceEventMinorGCStart);
}

void
js::gc::TracePromoteToTenured(Cell* src, Cell* dst)
{
    TraceEvent(TraceEventPromoteToTenured, uint64_t(src));
    TraceAddress(dst);
}

void
js::gc::TraceMinorGCEnd()
{
    TraceEvent(TraceEventMinorGCEnd);
}

void
js::gc::TraceMajorGCStart()
{
    TraceEvent(TraceEventMajorGCStart);
}

void
js::gc::TraceTenuredFinalize(Cell* thing)
{
    if (!gcTraceFile)
        return;
    if (thing->tenuredGetAllocKind() == AllocKind::OBJECT_GROUP)
        tracedGroups.remove(static_cast<const ObjectGroup*>(thing));
    TraceEvent(TraceEventTenuredFinalize, uint64_t(thing));
}

void
js::gc::TraceMajorGCEnd()
{
    TraceEvent(TraceEventMajorGCEnd);
}

#endif
