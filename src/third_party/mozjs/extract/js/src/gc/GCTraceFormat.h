/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCTraceFormat_h
#define gc_GCTraceFormat_h

/*
 * Each trace is stored as a 64-bit word with the following format:
 *
 *                 56              48                                         0
 * +----------------+----------------+-----------------------------------------+
 * | Event type     | Optional extra | Optional payload                        |
 * +----------------+----------------+-----------------------------------------+
 */

enum GCTraceEvent {
    // Events
    TraceEventInit,
    TraceEventThingSize,
    TraceEventNurseryAlloc,
    TraceEventTenuredAlloc,
    TraceEventClassInfo,
    TraceEventTypeInfo,
    TraceEventTypeNewScript,
    TraceEventCreateObject,
    TraceEventMinorGCStart,
    TraceEventPromoteToTenured,
    TraceEventMinorGCEnd,
    TraceEventMajorGCStart,
    TraceEventTenuredFinalize,
    TraceEventMajorGCEnd,

    TraceDataAddress,  // following TraceEventPromote
    TraceDataInt,      // following TraceEventClassInfo
    TraceDataString,   // following TraceEventClassInfo

    GCTraceEventCount
};

const unsigned TraceFormatVersion = 1;

const unsigned TracePayloadBits = 48;

const unsigned TraceExtraShift = 48;
const unsigned TraceExtraBits = 8;

const unsigned TraceEventShift = 56;
const unsigned TraceEventBits = 8;

const unsigned AllocKinds = 22;
const unsigned LastObjectAllocKind = 11;

#endif
