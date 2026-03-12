/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerMarkerTypes_h
#define BaseProfilerMarkerTypes_h

// This header contains common marker type definitions.
//
// It #include's "mozilla/BaseProfilerMarkers.h", see that file for how to
// define other marker types, and how to add markers to the profiler buffers.
//
// If you don't need to use these common types, #include
// "mozilla/BaseProfilerMarkers.h" instead.
//
// Types in this files can be defined without relying on xpcom.
// Others are defined in "ProfilerMarkerTypes.h".

// !!!                       /!\ WORK IN PROGRESS /!\                       !!!
// This file contains draft marker definitions, but most are not used yet.
// Further work is needed to complete these definitions, and use them to convert
// existing PROFILER_ADD_MARKER calls. See meta bug 1661394.

#include "mozilla/BaseProfilerMarkers.h"

namespace mozilla::baseprofiler::markers {

struct MediaSampleMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("MediaSample");
  }
  static void StreamJSONMarkerData(SpliceableJSONWriter& aWriter,
                                   int64_t aSampleStartTimeUs,
                                   int64_t aSampleEndTimeUs,
                                   int64_t aQueueLength) {
    aWriter.IntProperty("sampleStartTimeUs", aSampleStartTimeUs);
    aWriter.IntProperty("sampleEndTimeUs", aSampleEndTimeUs);
    aWriter.IntProperty("queueLength", aQueueLength);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("sampleStartTimeUs", "Sample start time",
                             MS::Format::Microseconds);
    schema.AddKeyLabelFormat("sampleEndTimeUs", "Sample end time",
                             MS::Format::Microseconds);
    schema.AddKeyLabelFormat("queueLength", "Queue length",
                             MS::Format::Integer);
    return schema;
  }
};

struct VideoFallingBehindMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("VideoFallingBehind");
  }
  static void StreamJSONMarkerData(SpliceableJSONWriter& aWriter,
                                   int64_t aVideoFrameStartTimeUs,
                                   int64_t aMediaCurrentTimeUs) {
    aWriter.IntProperty("videoFrameStartTimeUs", aVideoFrameStartTimeUs);
    aWriter.IntProperty("mediaCurrentTimeUs", aMediaCurrentTimeUs);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("videoFrameStartTimeUs", "Video frame start time",
                             MS::Format::Microseconds);
    schema.AddKeyLabelFormat("mediaCurrentTimeUs", "Media current time",
                             MS::Format::Microseconds);
    return schema;
  }
};

struct ContentBuildMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("CONTENT_FULL_PAINT_TIME");
  }
  static void StreamJSONMarkerData(SpliceableJSONWriter& aWriter) {}
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    return schema;
  }
};

struct MediaEngineMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("MediaEngine");
  }
  static void StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter& aWriter,
                                   uint64_t aMediaEngineId) {
    aWriter.IntProperty("id", aMediaEngineId);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("id", "Id", MS::Format::Integer);
    return schema;
  }
};

struct MediaEngineTextMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("MediaEngineText");
  }
  static void StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter& aWriter,
                                   uint64_t aMediaEngineId,
                                   const ProfilerString8View& aText) {
    aWriter.IntProperty("id", aMediaEngineId);
    aWriter.StringProperty("text", aText);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("id", "Id", MS::Format::Integer);
    schema.AddKeyLabelFormat("text", "Details", MS::Format::String);
    return schema;
  }
};

struct VideoSinkRenderMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("VideoSinkRender");
  }
  static void StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter& aWriter,
                                   int64_t aClockTimeUs) {
    aWriter.IntProperty("clockTimeUs", aClockTimeUs);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("clockTimeUs", "Clock time",
                             MS::Format::Microseconds);
    return schema;
  }
};

}  // namespace mozilla::baseprofiler::markers

#endif  // BaseProfilerMarkerTypes_h
