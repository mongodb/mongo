/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerMarkersDetail_h
#define BaseProfilerMarkersDetail_h

#ifndef BaseProfilerMarkers_h
#  error "This header should only be #included by BaseProfilerMarkers.h"
#endif

#include "mozilla/BaseProfilerMarkersPrerequisites.h"

//                        ~~ HERE BE DRAGONS ~~
//
// Everything below is internal implementation detail, you shouldn't need to
// look at it unless working on the profiler code.

#include "mozilla/BaseProfileJSONWriter.h"
#include "mozilla/ProfileBufferEntryKinds.h"

#include <limits>
#include <tuple>
#include <type_traits>

namespace mozilla::baseprofiler {
// Implemented in platform.cpp
MFBT_API ProfileChunkedBuffer& profiler_get_core_buffer();
}  // namespace mozilla::baseprofiler

namespace mozilla::base_profiler_markers_detail {

struct Streaming {
  // A `MarkerDataDeserializer` is a free function that can read a serialized
  // payload from an `EntryReader` and streams it as JSON object properties.
  using MarkerDataDeserializer = void (*)(ProfileBufferEntryReader&,
                                          baseprofiler::SpliceableJSONWriter&);

  // A `MarkerTypeNameFunction` is a free function that returns the name of the
  // marker type.
  using MarkerTypeNameFunction = Span<const char> (*)();

  // A `MarkerSchemaFunction` is a free function that returns a
  // `MarkerSchema`, which contains all the information needed to stream
  // the display schema associated with a marker type.
  using MarkerSchemaFunction = MarkerSchema (*)();

  struct MarkerTypeFunctions {
    MarkerDataDeserializer mMarkerDataDeserializer = nullptr;
    MarkerTypeNameFunction mMarkerTypeNameFunction = nullptr;
    MarkerSchemaFunction mMarkerSchemaFunction = nullptr;
  };

  // A `DeserializerTag` will be added before the payload, to help select the
  // correct deserializer when reading back the payload.
  using DeserializerTag = uint8_t;

  // Store a deserializer (and other marker-type-specific functions) and get its
  // `DeserializerTag`.
  // This is intended to be only used once per deserializer when a new marker
  // type is used for the first time, so it should be called to initialize a
  // `static const` tag that will be re-used by all markers of the corresponding
  // payload type -- see use below.
  MFBT_API static DeserializerTag TagForMarkerTypeFunctions(
      MarkerDataDeserializer aDeserializer,
      MarkerTypeNameFunction aMarkerTypeNameFunction,
      MarkerSchemaFunction aMarkerSchemaFunction);

  // Get the `MarkerDataDeserializer` for a given `DeserializerTag`.
  MFBT_API static MarkerDataDeserializer DeserializerForTag(
      DeserializerTag aTag);

  // Retrieve all MarkerTypeFunctions's.
  // While this object lives, no other operations can happen on this list.
  class LockedMarkerTypeFunctionsList {
   public:
    MFBT_API LockedMarkerTypeFunctionsList();
    MFBT_API ~LockedMarkerTypeFunctionsList();

    LockedMarkerTypeFunctionsList(const LockedMarkerTypeFunctionsList&) =
        delete;
    LockedMarkerTypeFunctionsList& operator=(
        const LockedMarkerTypeFunctionsList&) = delete;

    auto begin() const { return mMarkerTypeFunctionsSpan.begin(); }
    auto end() const { return mMarkerTypeFunctionsSpan.end(); }

   private:
    Span<const MarkerTypeFunctions> mMarkerTypeFunctionsSpan;
  };
};

// This helper will examine a marker type's `StreamJSONMarkerData` function, see
// specialization below.
template <typename T>
struct StreamFunctionTypeHelper;

// Helper specialization that takes the expected
// `StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter&, ...)` function and
// provide information about the `...` parameters.
template <typename R, typename... As>
struct StreamFunctionTypeHelper<R(baseprofiler::SpliceableJSONWriter&, As...)> {
  constexpr static size_t scArity = sizeof...(As);
  using TupleType =
      std::tuple<std::remove_cv_t<std::remove_reference_t<As>>...>;

  // Serialization function that takes the exact same parameter types
  // (const-ref'd) as `StreamJSONMarkerData`. This has to be inside the helper
  // because only here can we access the raw parameter pack `As...`.
  // And because we're using the same argument types through
  // references-to-const, permitted implicit conversions can happen.
  static ProfileBufferBlockIndex Serialize(
      ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
      const MarkerCategory& aCategory, MarkerOptions&& aOptions,
      Streaming::DeserializerTag aDeserializerTag, const As&... aAs) {
    // Note that options are first after the entry kind, because they contain
    // the thread id, which is handled first to filter markers by threads.
    return aBuffer.PutObjects(ProfileBufferEntryKind::Marker, aOptions, aName,
                              aCategory, aDeserializerTag,
                              MarkerPayloadType::Cpp, aAs...);
  }
};

// Helper for a marker type.
// A marker type is defined in a `struct` with some expected static member
// functions. See example in BaseProfilerMarkers.h.
template <typename MarkerType>
struct MarkerTypeSerialization {
  // Definitions to access the expected
  // `StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter&, ...)` function
  // and its parameters.
  using StreamFunctionType =
      StreamFunctionTypeHelper<decltype(MarkerType::StreamJSONMarkerData)>;
  constexpr static size_t scStreamFunctionParameterCount =
      StreamFunctionType::scArity;
  using StreamFunctionUserParametersTuple =
      typename StreamFunctionType::TupleType;
  template <size_t i>
  using StreamFunctionParameter =
      std::tuple_element_t<i, StreamFunctionUserParametersTuple>;

  template <typename... Ts>
  static ProfileBufferBlockIndex Serialize(ProfileChunkedBuffer& aBuffer,
                                           const ProfilerString8View& aName,
                                           const MarkerCategory& aCategory,
                                           MarkerOptions&& aOptions,
                                           const Ts&... aTs) {
    static_assert(!std::is_same_v<MarkerType,
                                  ::mozilla::baseprofiler::markers::NoPayload>,
                  "NoPayload should have been handled in the caller.");
    // Register marker type functions, and get the tag for this deserializer.
    // Note that the tag is stored in a function-static object, and this
    // function is static in a templated struct, so there should only be one tag
    // per MarkerType.
    // Making the tag class-static may have been more efficient (to avoid a
    // thread-safe init check at every call), but random global static
    // initialization order would make it more complex to coordinate with
    // `Streaming::TagForMarkerTypeFunctions()`, and also would add a (small)
    // cost for everybody, even the majority of users not using the profiler.
    static const Streaming::DeserializerTag tag =
        Streaming::TagForMarkerTypeFunctions(Deserialize,
                                             MarkerType::MarkerTypeName,
                                             MarkerType::MarkerTypeDisplay);
    return StreamFunctionType::Serialize(aBuffer, aName, aCategory,
                                         std::move(aOptions), tag, aTs...);
  }

 private:
  // This templated function will recursively deserialize each argument expected
  // by `MarkerType::StreamJSONMarkerData()` on the stack, and call it at the
  // end. E.g., for `StreamJSONMarkerData(int, char)`:
  // - DeserializeArguments<0>(aER, aWriter) reads an int and calls:
  // - DeserializeArguments<1>(aER, aWriter, const int&) reads a char and calls:
  // - MarkerType::StreamJSONMarkerData(aWriter, const int&, const char&).
  // Prototyping on godbolt showed that clang and gcc can flatten these
  // recursive calls into one function with successive reads followed by the one
  // stream call; tested up to 40 arguments: https://godbolt.org/z/5KeeM4
  template <size_t i = 0, typename... Args>
  static void DeserializeArguments(ProfileBufferEntryReader& aEntryReader,
                                   baseprofiler::SpliceableJSONWriter& aWriter,
                                   const Args&... aArgs) {
    static_assert(sizeof...(Args) == i,
                  "We should have collected `i` arguments so far");
    if constexpr (i < scStreamFunctionParameterCount) {
      // Deserialize the i-th argument on this stack.
      auto argument = aEntryReader.ReadObject<StreamFunctionParameter<i>>();
      // Add our local argument to the next recursive call.
      DeserializeArguments<i + 1>(aEntryReader, aWriter, aArgs..., argument);
    } else {
      // We've read all the arguments, finally call the `StreamJSONMarkerData`
      // function, which should write the appropriate JSON elements for this
      // marker type. Note that the MarkerType-specific "type" element is
      // already written.
      MarkerType::StreamJSONMarkerData(aWriter, aArgs...);
    }
  }

 public:
  static void Deserialize(ProfileBufferEntryReader& aEntryReader,
                          baseprofiler::SpliceableJSONWriter& aWriter) {
    aWriter.StringProperty("type", MarkerType::MarkerTypeName());
    DeserializeArguments(aEntryReader, aWriter);
  }
};

template <>
struct MarkerTypeSerialization<::mozilla::baseprofiler::markers::NoPayload> {
  // Nothing! NoPayload has special handling avoiding payload work.
};

template <typename MarkerType, typename... Ts>
static ProfileBufferBlockIndex AddMarkerWithOptionalStackToBuffer(
    ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
    const MarkerCategory& aCategory, MarkerOptions&& aOptions,
    const Ts&... aTs) {
  if constexpr (std::is_same_v<MarkerType,
                               ::mozilla::baseprofiler::markers::NoPayload>) {
    static_assert(sizeof...(Ts) == 0,
                  "NoPayload does not accept any payload arguments.");
    // Special case for NoPayload where there is a stack or inner window id:
    // Because these options would be stored in the payload 'data' object, but
    // there is no such object for NoPayload, we convert the marker to another
    // type (without user fields in the 'data' object), so that the stack and/or
    // inner window id are not lost.
    // TODO: Remove this when bug 1646714 lands.
    if (aOptions.Stack().GetChunkedBuffer() ||
        !aOptions.InnerWindowId().IsUnspecified()) {
      struct NoPayloadUserData {
        static constexpr Span<const char> MarkerTypeName() {
          return MakeStringSpan("NoPayloadUserData");
        }
        static void StreamJSONMarkerData(
            baseprofiler::SpliceableJSONWriter& aWriter) {
          // No user payload.
        }
        static mozilla::MarkerSchema MarkerTypeDisplay() {
          using MS = mozilla::MarkerSchema;
          MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
          // No user data to display.
          return schema;
        }
      };
      return MarkerTypeSerialization<NoPayloadUserData>::Serialize(
          aBuffer, aName, aCategory, std::move(aOptions));
    }

    // Note that options are first after the entry kind, because they contain
    // the thread id, which is handled first to filter markers by threads.
    return aBuffer.PutObjects(
        ProfileBufferEntryKind::Marker, std::move(aOptions), aName, aCategory,
        base_profiler_markers_detail::Streaming::DeserializerTag(0));
  } else {
    return MarkerTypeSerialization<MarkerType>::Serialize(
        aBuffer, aName, aCategory, std::move(aOptions), aTs...);
  }
}

// Pointer to a function that can capture a backtrace into the provided
// `ProfileChunkedBuffer`, and returns true when successful.
using OptionalBacktraceCaptureFunction = bool (*)(ProfileChunkedBuffer&,
                                                  StackCaptureOptions);

// Use a pre-allocated and cleared chunked buffer in the main thread's
// `AddMarkerToBuffer()`.
// Null if not the main thread, or if profilers are not active.
MFBT_API ProfileChunkedBuffer* GetClearedBufferForMainThreadAddMarker();
// Called by the profiler(s) when starting/stopping. Safe to nest.
MFBT_API void EnsureBufferForMainThreadAddMarker();
MFBT_API void ReleaseBufferForMainThreadAddMarker();

// Add a marker with the given name, options, and arguments to the given buffer.
// Because this may be called from either Base or Gecko Profiler functions, the
// appropriate backtrace-capturing function must also be provided.
template <typename MarkerType, typename... Ts>
ProfileBufferBlockIndex AddMarkerToBuffer(
    ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
    const MarkerCategory& aCategory, MarkerOptions&& aOptions,
    OptionalBacktraceCaptureFunction aOptionalBacktraceCaptureFunction,
    const Ts&... aTs) {
  if (aOptions.ThreadId().IsUnspecified()) {
    // If yet unspecified, set thread to this thread where the marker is added.
    aOptions.Set(MarkerThreadId::CurrentThread());
  }

  if (aOptions.IsTimingUnspecified()) {
    // If yet unspecified, set timing to this instant of adding the marker.
    aOptions.Set(MarkerTiming::InstantNow());
  }

  StackCaptureOptions captureOptions = aOptions.Stack().CaptureOptions();
  if (captureOptions != StackCaptureOptions::NoStack &&
      // Backtrace capture function will be nullptr if the profiler
      // NoMarkerStacks feature is set.
      aOptionalBacktraceCaptureFunction != nullptr) {
    // A capture was requested, let's attempt to do it here&now. This avoids a
    // lot of allocations that would be necessary if capturing a backtrace
    // separately.
    // TODO reduce internal profiler stack levels, see bug 1659872.
    auto CaptureStackAndAddMarker = [&](ProfileChunkedBuffer& aChunkedBuffer) {
      aOptions.StackRef().UseRequestedBacktrace(
          aOptionalBacktraceCaptureFunction(aChunkedBuffer, captureOptions)
              ? &aChunkedBuffer
              : nullptr);
      // This call must be made from here, while chunkedBuffer is in scope.
      return AddMarkerWithOptionalStackToBuffer<MarkerType>(
          aBuffer, aName, aCategory, std::move(aOptions), aTs...);
    };

    if (ProfileChunkedBuffer* buffer = GetClearedBufferForMainThreadAddMarker();
        buffer) {
      // Use a pre-allocated buffer for the main thread (because it's the most
      // used thread, and most sensitive to overhead), so it's only allocated
      // once. It could be null if this is not the main thread, or no profilers
      // are currently active.
      return CaptureStackAndAddMarker(*buffer);
    }
    // TODO use a local on-stack byte buffer to remove last allocation.
    ProfileBufferChunkManagerSingle chunkManager(
        ProfileBufferChunkManager::scExpectedMaximumStackSize);
    ProfileChunkedBuffer chunkedBuffer(
        ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);
    return CaptureStackAndAddMarker(chunkedBuffer);
  }

  return AddMarkerWithOptionalStackToBuffer<MarkerType>(
      aBuffer, aName, aCategory, std::move(aOptions), aTs...);
}

// Assuming aEntryReader points right after the entry type (being Marker), this
// reads the remainder of the marker and outputs it.
// - GetWriterForThreadCallback, called first, after the thread id is read:
//     (ThreadId) -> SpliceableJSONWriter* or null
//   If null, nothing will be output, but aEntryReader will still be read fully.
// - StackCallback, only called if GetWriterForThreadCallback didn't return
//   null, and if the marker contains a stack:
//     (ProfileChunkedBuffer&) -> void
// - RustMarkerCallback, only called if GetWriterForThreadCallback didn't return
//   null, and if the marker contains a Rust payload:
//     (DeserializerTag) -> void
template <typename GetWriterForThreadCallback, typename StackCallback,
          typename RustMarkerCallback>
void DeserializeAfterKindAndStream(
    ProfileBufferEntryReader& aEntryReader,
    GetWriterForThreadCallback&& aGetWriterForThreadCallback,
    StackCallback&& aStackCallback, RustMarkerCallback&& aRustMarkerCallback) {
  // Each entry is made up of the following:
  //   ProfileBufferEntry::Kind::Marker, <- already read by caller
  //   options,                          <- next location in entries
  //   name,
  //   payload
  const MarkerOptions options = aEntryReader.ReadObject<MarkerOptions>();

  baseprofiler::SpliceableJSONWriter* writer =
      std::forward<GetWriterForThreadCallback>(aGetWriterForThreadCallback)(
          options.ThreadId().ThreadId());
  if (!writer) {
    // No writer associated with this thread id, drop it.
    aEntryReader.SetRemainingBytes(0);
    return;
  }

  // Write the information to JSON with the following schema:
  // [name, startTime, endTime, phase, category, data]
  writer->StartArrayElement();
  {
    writer->UniqueStringElement(aEntryReader.ReadObject<ProfilerString8View>());

    const double startTime = options.Timing().GetStartTime();
    writer->TimeDoubleMsElement(startTime);

    const double endTime = options.Timing().GetEndTime();
    writer->TimeDoubleMsElement(endTime);

    writer->IntElement(static_cast<int64_t>(options.Timing().MarkerPhase()));

    MarkerCategory category = aEntryReader.ReadObject<MarkerCategory>();
    writer->IntElement(static_cast<int64_t>(category.GetCategory()));

    if (const auto tag =
            aEntryReader.ReadObject<mozilla::base_profiler_markers_detail::
                                        Streaming::DeserializerTag>();
        tag != 0) {
      writer->StartObjectElement();
      {
        // Stream "common props".

        // TODO: Move this to top-level tuple, when frontend supports it.
        if (!options.InnerWindowId().IsUnspecified()) {
          // Here, we are converting uint64_t to double. Both Browsing Context
          // and Inner Window IDs are created using
          // `nsContentUtils::GenerateProcessSpecificId`, which is specifically
          // designed to only use 53 of the 64 bits to be lossless when passed
          // into and out of JS as a double.
          writer->DoubleProperty(
              "innerWindowID",
              static_cast<double>(options.InnerWindowId().Id()));
        }

        // TODO: Move this to top-level tuple, when frontend supports it.
        if (ProfileChunkedBuffer* chunkedBuffer =
                options.Stack().GetChunkedBuffer();
            chunkedBuffer) {
          writer->StartObjectProperty("stack");
          {
            std::forward<StackCallback>(aStackCallback)(*chunkedBuffer);
          }
          writer->EndObject();
        }

        auto payloadType = static_cast<mozilla::MarkerPayloadType>(
            aEntryReader.ReadObject<
                std::underlying_type_t<mozilla::MarkerPayloadType>>());

        // Stream the payload, including the type.
        switch (payloadType) {
          case mozilla::MarkerPayloadType::Cpp: {
            mozilla::base_profiler_markers_detail::Streaming::
                MarkerDataDeserializer deserializer =
                    mozilla::base_profiler_markers_detail::Streaming::
                        DeserializerForTag(tag);
            MOZ_RELEASE_ASSERT(deserializer);
            deserializer(aEntryReader, *writer);
            MOZ_ASSERT(aEntryReader.RemainingBytes() == 0u);
            break;
          }
          case mozilla::MarkerPayloadType::Rust:
            std::forward<RustMarkerCallback>(aRustMarkerCallback)(tag);
            MOZ_ASSERT(aEntryReader.RemainingBytes() == 0u);
            break;
          default:
            MOZ_ASSERT_UNREACHABLE("Unknown payload type.");
            break;
        }
      }
      writer->EndObject();
    }
  }
  writer->EndArray();
  MOZ_ASSERT(aEntryReader.RemainingBytes() == 0u);
}

}  // namespace mozilla::base_profiler_markers_detail

namespace mozilla {

// ----------------------------------------------------------------------------
// Serializer, Deserializer: ProfilerStringView<CHAR>

// The serialization starts with a ULEB128 number that encodes both whether the
// ProfilerStringView is literal (Least Significant Bit = 0) or not (LSB = 1),
// plus the string length (excluding null terminator) in bytes, shifted left by
// 1 bit. Following that number:
// - If literal, the string pointer value.
// - If non-literal, the contents as bytes (excluding null terminator if any).
template <typename CHAR>
struct ProfileBufferEntryWriter::Serializer<ProfilerStringView<CHAR>> {
  static Length Bytes(const ProfilerStringView<CHAR>& aString) {
    MOZ_RELEASE_ASSERT(
        aString.Length() < std::numeric_limits<Length>::max() / 2,
        "Double the string length doesn't fit in Length type");
    const Length stringLength = static_cast<Length>(aString.Length());
    if (aString.IsLiteral()) {
      // Literal -> Length shifted left and LSB=0, then pointer.
      return ULEB128Size(stringLength << 1 | 0u) +
             static_cast<ProfileChunkedBuffer::Length>(sizeof(const CHAR*));
    }
    // Non-literal -> Length shifted left and LSB=1, then string size in bytes.
    return ULEB128Size((stringLength << 1) | 1u) + stringLength * sizeof(CHAR);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfilerStringView<CHAR>& aString) {
    MOZ_RELEASE_ASSERT(
        aString.Length() < std::numeric_limits<Length>::max() / 2,
        "Double the string length doesn't fit in Length type");
    const Span<const CHAR> span = aString;
    if (aString.IsLiteral()) {
      // Literal -> Length shifted left and LSB=0, then pointer.
      aEW.WriteULEB128(span.Length() << 1 | 0u);
      aEW.WriteObject(WrapProfileBufferRawPointer(span.Elements()));
      return;
    }
    // Non-literal -> Length shifted left and LSB=1, then string size in bytes.
    aEW.WriteULEB128(span.Length() << 1 | 1u);
    aEW.WriteBytes(span.Elements(), span.LengthBytes());
  }
};

template <typename CHAR>
struct ProfileBufferEntryReader::Deserializer<ProfilerStringView<CHAR>> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       ProfilerStringView<CHAR>& aString) {
    aString = Read(aER);
  }

  static ProfilerStringView<CHAR> Read(ProfileBufferEntryReader& aER) {
    const Length lengthAndIsLiteral = aER.ReadULEB128<Length>();
    const Length stringLength = lengthAndIsLiteral >> 1;
    if ((lengthAndIsLiteral & 1u) == 0u) {
      // LSB==0 -> Literal string, read the string pointer.
      return ProfilerStringView<CHAR>(
          aER.ReadObject<const CHAR*>(), stringLength,
          ProfilerStringView<CHAR>::Ownership::Literal);
    }
    // LSB==1 -> Not a literal string.
    ProfileBufferEntryReader::DoubleSpanOfConstBytes spans =
        aER.ReadSpans(stringLength * sizeof(CHAR));
    if (MOZ_LIKELY(spans.IsSingleSpan()) &&
        reinterpret_cast<uintptr_t>(spans.mFirstOrOnly.Elements()) %
                alignof(CHAR) ==
            0u) {
      // Only a single span, correctly aligned for the CHAR type, we can just
      // refer to it directly, assuming that this ProfilerStringView will not
      // outlive the chunk.
      return ProfilerStringView<CHAR>(
          reinterpret_cast<const CHAR*>(spans.mFirstOrOnly.Elements()),
          stringLength, ProfilerStringView<CHAR>::Ownership::Reference);
    } else {
      // Two spans, we need to concatenate them; or one span, but misaligned.
      // Allocate a buffer to store the string (plus terminal, for safety), and
      // give it to the ProfilerStringView; Note that this is a secret use of
      // ProfilerStringView, which is intended to only be used between
      // deserialization and JSON streaming.
      CHAR* buffer = new CHAR[stringLength + 1];
      spans.CopyBytesTo(buffer);
      buffer[stringLength] = CHAR(0);
      return ProfilerStringView<CHAR>(
          buffer, stringLength,
          ProfilerStringView<CHAR>::Ownership::OwnedThroughStringView);
    }
  }
};

// Serializer, Deserializer: MarkerCategory

// The serialization contains both category numbers encoded as ULEB128.
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerCategory> {
  static Length Bytes(const MarkerCategory& aCategory) {
    return ULEB128Size(static_cast<uint32_t>(aCategory.CategoryPair()));
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerCategory& aCategory) {
    aEW.WriteULEB128(static_cast<uint32_t>(aCategory.CategoryPair()));
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerCategory> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       MarkerCategory& aCategory) {
    aCategory = Read(aER);
  }

  static MarkerCategory Read(ProfileBufferEntryReader& aER) {
    return MarkerCategory(static_cast<baseprofiler::ProfilingCategoryPair>(
        aER.ReadULEB128<uint32_t>()));
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerTiming

// The serialization starts with the marker phase, followed by one or two
// timestamps as needed.
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerTiming> {
  static Length Bytes(const MarkerTiming& aTiming) {
    MOZ_ASSERT(!aTiming.IsUnspecified());
    const auto phase = aTiming.MarkerPhase();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        return SumBytes(phase, aTiming.StartTime());
      case MarkerTiming::Phase::Interval:
        return SumBytes(phase, aTiming.StartTime(), aTiming.EndTime());
      case MarkerTiming::Phase::IntervalStart:
        return SumBytes(phase, aTiming.StartTime());
      case MarkerTiming::Phase::IntervalEnd:
        return SumBytes(phase, aTiming.EndTime());
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        return 0;  // Only to avoid build errors.
    }
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerTiming& aTiming) {
    MOZ_ASSERT(!aTiming.IsUnspecified());
    const auto phase = aTiming.MarkerPhase();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        aEW.WriteObjects(phase, aTiming.StartTime());
        return;
      case MarkerTiming::Phase::Interval:
        aEW.WriteObjects(phase, aTiming.StartTime(), aTiming.EndTime());
        return;
      case MarkerTiming::Phase::IntervalStart:
        aEW.WriteObjects(phase, aTiming.StartTime());
        return;
      case MarkerTiming::Phase::IntervalEnd:
        aEW.WriteObjects(phase, aTiming.EndTime());
        return;
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        return;
    }
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerTiming> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerTiming& aTiming) {
    aTiming.mPhase = aER.ReadObject<MarkerTiming::Phase>();
    switch (aTiming.mPhase) {
      case MarkerTiming::Phase::Instant:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = TimeStamp{};
        break;
      case MarkerTiming::Phase::Interval:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalStart:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = TimeStamp{};
        break;
      case MarkerTiming::Phase::IntervalEnd:
        aTiming.mStartTime = TimeStamp{};
        aTiming.mEndTime = aER.ReadObject<TimeStamp>();
        break;
      default:
        MOZ_RELEASE_ASSERT(aTiming.mPhase == MarkerTiming::Phase::Instant ||
                           aTiming.mPhase == MarkerTiming::Phase::Interval ||
                           aTiming.mPhase ==
                               MarkerTiming::Phase::IntervalStart ||
                           aTiming.mPhase == MarkerTiming::Phase::IntervalEnd);
        break;
    }
  }

  static MarkerTiming Read(ProfileBufferEntryReader& aER) {
    TimeStamp start;
    TimeStamp end;
    auto phase = aER.ReadObject<MarkerTiming::Phase>();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        start = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::Interval:
        start = aER.ReadObject<TimeStamp>();
        end = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalStart:
        start = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalEnd:
        end = aER.ReadObject<TimeStamp>();
        break;
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        break;
    }
    return MarkerTiming(start, end, phase);
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerStack

// The serialization only contains the `ProfileChunkedBuffer` from the
// backtrace; if there is no backtrace or if it's empty, this will implicitly
// store a nullptr (see
// `ProfileBufferEntryWriter::Serializer<ProfilerChunkedBuffer*>`).
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerStack> {
  static Length Bytes(const MarkerStack& aStack) {
    return SumBytes(aStack.GetChunkedBuffer());
  }

  static void Write(ProfileBufferEntryWriter& aEW, const MarkerStack& aStack) {
    aEW.WriteObject(aStack.GetChunkedBuffer());
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerStack> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerStack& aStack) {
    aStack = Read(aER);
  }

  static MarkerStack Read(ProfileBufferEntryReader& aER) {
    return MarkerStack(aER.ReadObject<UniquePtr<ProfileChunkedBuffer>>());
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerOptions

// The serialization contains all members (either trivially-copyable, or they
// provide their specialization above).
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerOptions> {
  static Length Bytes(const MarkerOptions& aOptions) {
    return SumBytes(aOptions.ThreadId(), aOptions.Timing(), aOptions.Stack(),
                    aOptions.InnerWindowId());
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerOptions& aOptions) {
    aEW.WriteObjects(aOptions.ThreadId(), aOptions.Timing(), aOptions.Stack(),
                     aOptions.InnerWindowId());
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerOptions> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerOptions& aOptions) {
    aER.ReadIntoObjects(aOptions.mThreadId, aOptions.mTiming, aOptions.mStack,
                        aOptions.mInnerWindowId);
  }

  static MarkerOptions Read(ProfileBufferEntryReader& aER) {
    MarkerOptions options;
    ReadInto(aER, options);
    return options;
  }
};

}  // namespace mozilla

#endif  // BaseProfilerMarkersDetail_h
