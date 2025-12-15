/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This header contains basic definitions required to create marker types, and
// to add markers to the profiler buffers.
//
// In most cases, #include "mozilla/BaseProfilerMarkers.h" instead, or
// #include "mozilla/BaseProfilerMarkerTypes.h" for common marker types.

#ifndef BaseProfilerMarkersPrerequisites_h
#define BaseProfilerMarkersPrerequisites_h

namespace mozilla {

enum class StackCaptureOptions {
  NoStack,    // No stack captured.
  Full,       // Capture a full stack, including label frames, JS frames and
              // native frames.
  NonNative,  // Capture a stack without native frames for reduced overhead.
};

}

#include "BaseProfileJSONWriter.h"
#include "BaseProfilingCategory.h"
#include "mozilla/Maybe.h"
#include "mozilla/ProfileChunkedBuffer.h"
#include "mozilla/BaseProfilerState.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Variant.h"

#include <initializer_list>
#include <string_view>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The header <X11/X.h> defines "None" as a macro that expands to "0L".
// This is terrible because we have an enum variant named "None" too in this
// file. To work around this, we undefine the macro "None".
#ifdef None
#  undef None
#endif

namespace mozilla {

// Return a NotNull<const CHAR*> pointing at the literal empty string `""`.
template <typename CHAR>
constexpr const CHAR* LiteralEmptyStringPointer() {
  static_assert(std::is_same_v<CHAR, char> || std::is_same_v<CHAR, char16_t>,
                "Only char and char16_t are supported in Firefox");
  if constexpr (std::is_same_v<CHAR, char>) {
    return "";
  }
  if constexpr (std::is_same_v<CHAR, char16_t>) {
    return u"";
  }
}

// Return a string_view<CHAR> pointing at the literal empty string.
template <typename CHAR>
constexpr std::basic_string_view<CHAR> LiteralEmptyStringView() {
  static_assert(std::is_same_v<CHAR, char> || std::is_same_v<CHAR, char16_t>,
                "Only char and char16_t are supported in Firefox");
  // Use `operator""sv()` from <string_view>.
  using namespace std::literals::string_view_literals;
  if constexpr (std::is_same_v<CHAR, char>) {
    return ""sv;
  }
  if constexpr (std::is_same_v<CHAR, char16_t>) {
    return u""sv;
  }
}

// General string view, optimized for short on-stack life before serialization,
// and between deserialization and JSON-streaming.
template <typename CHAR>
class MOZ_STACK_CLASS ProfilerStringView {
 public:
  // Default constructor points at "" (literal empty string).
  constexpr ProfilerStringView() = default;

  // Don't allow copy.
  ProfilerStringView(const ProfilerStringView&) = delete;
  ProfilerStringView& operator=(const ProfilerStringView&) = delete;

  // Allow move. For consistency the moved-from string is always reset to "".
  constexpr ProfilerStringView(ProfilerStringView&& aOther)
      : mStringView(std::move(aOther.mStringView)),
        mOwnership(aOther.mOwnership) {
    if (mOwnership == Ownership::OwnedThroughStringView) {
      // We now own the buffer, make the other point at the literal "".
      aOther.mStringView = LiteralEmptyStringView<CHAR>();
      aOther.mOwnership = Ownership::Literal;
    }
  }
  constexpr ProfilerStringView& operator=(ProfilerStringView&& aOther) {
    mStringView = std::move(aOther.mStringView);
    mOwnership = aOther.mOwnership;
    if (mOwnership == Ownership::OwnedThroughStringView) {
      // We now own the buffer, make the other point at the literal "".
      aOther.mStringView = LiteralEmptyStringView<CHAR>();
      aOther.mOwnership = Ownership::Literal;
    }
    return *this;
  }

  ~ProfilerStringView() {
    if (MOZ_UNLIKELY(mOwnership == Ownership::OwnedThroughStringView)) {
      // We own the buffer pointed at by mStringView, destroy it.
      // This is only used between deserialization and streaming.
      delete mStringView.data();
    }
  }

  // Implicit construction from nullptr, points at "" (literal empty string).
  constexpr MOZ_IMPLICIT ProfilerStringView(decltype(nullptr)) {}

  // Implicit constructor from a literal string.
  template <size_t Np1>
  constexpr MOZ_IMPLICIT ProfilerStringView(const CHAR (&aLiteralString)[Np1])
      : ProfilerStringView(aLiteralString, Np1 - 1, Ownership::Literal) {}

  // Constructor from a non-literal string.
  constexpr ProfilerStringView(const CHAR* aString, size_t aLength)
      : ProfilerStringView(aString, aLength, Ownership::Reference) {}

  // Implicit constructor from a string_view.
  constexpr MOZ_IMPLICIT ProfilerStringView(
      const std::basic_string_view<CHAR>& aStringView)
      : ProfilerStringView(aStringView.data(), aStringView.length(),
                           Ownership::Reference) {}

  // Implicit constructor from an expiring string_view. We assume that the
  // pointed-at string will outlive this ProfilerStringView.
  constexpr MOZ_IMPLICIT ProfilerStringView(
      std::basic_string_view<CHAR>&& aStringView)
      : ProfilerStringView(aStringView.data(), aStringView.length(),
                           Ownership::Reference) {}

  // Implicit constructor from std::string.
  constexpr MOZ_IMPLICIT ProfilerStringView(
      const std::basic_string<CHAR>& aString)
      : ProfilerStringView(aString.data(), aString.length(),
                           Ownership::Reference) {}

  // Construction from a raw pointer to a null-terminated string.
  // This is a named class-static function to make it more obvious where work is
  // being done (to determine the string length), and encourage users to instead
  // provide a length, if already known.
  // TODO: Find callers and convert them to constructor instead if possible.
  static constexpr ProfilerStringView WrapNullTerminatedString(
      const CHAR* aString) {
    return ProfilerStringView(
        aString, aString ? std::char_traits<CHAR>::length(aString) : 0,
        Ownership::Reference);
  }

  // Implicit constructor for an object with member functions `Data()`
  // `Length()`, and `IsLiteral()`, common in xpcom strings.
  template <
      typename String,
      typename DataReturnType = decltype(std::declval<const String>().Data()),
      typename LengthReturnType =
          decltype(std::declval<const String>().Length()),
      typename IsLiteralReturnType =
          decltype(std::declval<const String>().IsLiteral()),
      typename =
          std::enable_if_t<std::is_convertible_v<DataReturnType, const CHAR*> &&
                           std::is_integral_v<LengthReturnType> &&
                           std::is_same_v<IsLiteralReturnType, bool>>>
  constexpr MOZ_IMPLICIT ProfilerStringView(const String& aString)
      : ProfilerStringView(
            static_cast<const CHAR*>(aString.Data()), aString.Length(),
            aString.IsLiteral() ? Ownership::Literal : Ownership::Reference) {}

  [[nodiscard]] constexpr const std::basic_string_view<CHAR>& StringView()
      const {
    return mStringView;
  }

  [[nodiscard]] constexpr size_t Length() const { return mStringView.length(); }

  [[nodiscard]] constexpr bool IsLiteral() const {
    return mOwnership == Ownership::Literal;
  }
  [[nodiscard]] constexpr bool IsReference() const {
    return mOwnership == Ownership::Reference;
  }
  // No `IsOwned...()` because it's a secret, only used internally!

  [[nodiscard]] Span<const CHAR> AsSpan() const {
    return Span<const CHAR>(mStringView.data(), mStringView.length());
  }
  [[nodiscard]] operator Span<const CHAR>() const { return AsSpan(); }

 private:
  enum class Ownership { Literal, Reference, OwnedThroughStringView };

  // Allow deserializer to store anything here.
  friend ProfileBufferEntryReader::Deserializer<ProfilerStringView>;

  constexpr ProfilerStringView(const CHAR* aString, size_t aLength,
                               Ownership aOwnership)
      : mStringView(aString ? std::basic_string_view<CHAR>(aString, aLength)
                            : LiteralEmptyStringView<CHAR>()),
        mOwnership(aString ? aOwnership : Ownership::Literal) {}

  // String view to an outside string (literal or reference).
  // We may actually own the pointed-at buffer, but it is only used internally
  // between deserialization and JSON streaming.
  std::basic_string_view<CHAR> mStringView = LiteralEmptyStringView<CHAR>();

  Ownership mOwnership = Ownership::Literal;
};

using ProfilerString8View = ProfilerStringView<char>;
using ProfilerString16View = ProfilerStringView<char16_t>;

// This compulsory marker parameter contains the required category information.
class MarkerCategory {
 public:
  // Constructor from category pair (includes both super- and sub-categories).
  constexpr explicit MarkerCategory(
      baseprofiler::ProfilingCategoryPair aCategoryPair)
      : mCategoryPair(aCategoryPair) {}

  // Returns the stored category pair.
  constexpr baseprofiler::ProfilingCategoryPair CategoryPair() const {
    return mCategoryPair;
  }

  // Returns the super-category from the stored category pair.
  baseprofiler::ProfilingCategory GetCategory() const {
    return GetProfilingCategoryPairInfo(mCategoryPair).mCategory;
  }

 private:
  baseprofiler::ProfilingCategoryPair mCategoryPair =
      baseprofiler::ProfilingCategoryPair::OTHER;
};

namespace baseprofiler::category {

// Each category pair name constructs a MarkerCategory.
// E.g.: mozilla::baseprofiler::category::OTHER_Profiling
// Profiler macros will take the category name alone without namespace.
// E.g.: `PROFILER_MARKER_UNTYPED("name", OTHER_Profiling)`
#define CATEGORY_ENUM_BEGIN_CATEGORY(name, labelAsString, color)
#define CATEGORY_ENUM_SUBCATEGORY(supercategory, name, labelAsString) \
  static constexpr MarkerCategory name{ProfilingCategoryPair::name};
#define CATEGORY_ENUM_END_CATEGORY
MOZ_PROFILING_CATEGORY_LIST(CATEGORY_ENUM_BEGIN_CATEGORY,
                            CATEGORY_ENUM_SUBCATEGORY,
                            CATEGORY_ENUM_END_CATEGORY)
#undef CATEGORY_ENUM_BEGIN_CATEGORY
#undef CATEGORY_ENUM_SUBCATEGORY
#undef CATEGORY_ENUM_END_CATEGORY

// Import `MarkerCategory` into this namespace. This will allow using this type
// dynamically in macros that prepend `::mozilla::baseprofiler::category::` to
// the given category, e.g.:
// `PROFILER_MARKER_UNTYPED("name", MarkerCategory(...))`
using MarkerCategory = ::mozilla::MarkerCategory;

}  // namespace baseprofiler::category

// The classes below are all embedded in a `MarkerOptions` object.
class MarkerOptions;

// This marker option captures a given thread id.
// If left unspecified (by default construction) during the add-marker call, the
// current thread id will be used then.
class MarkerThreadId {
 public:
  // Default constructor, keeps the thread id unspecified.
  constexpr MarkerThreadId() = default;

  // Constructor from a given thread id.
  constexpr explicit MarkerThreadId(
      baseprofiler::BaseProfilerThreadId aThreadId)
      : mThreadId(aThreadId) {}

  // Use the current thread's id.
  static MarkerThreadId CurrentThread() {
    return MarkerThreadId(baseprofiler::profiler_current_thread_id());
  }

  // Use the main thread's id. This can be useful to record a marker from a
  // possibly-unregistered thread, and display it in the main thread track.
  static MarkerThreadId MainThread() {
    return MarkerThreadId(baseprofiler::profiler_main_thread_id());
  }

  [[nodiscard]] constexpr baseprofiler::BaseProfilerThreadId ThreadId() const {
    return mThreadId;
  }

  [[nodiscard]] constexpr bool IsUnspecified() const {
    return !mThreadId.IsSpecified();
  }

 private:
  baseprofiler::BaseProfilerThreadId mThreadId;
};

// This marker option contains marker timing information.
// This class encapsulates the logic for correctly storing a marker based on its
// Use the static methods to create the MarkerTiming. This is a transient object
// that is being used to enforce the constraints of the combinations of the
// data.
class MarkerTiming {
 public:
  // The following static methods are used to create the MarkerTiming based on
  // the type that it is.

  static MarkerTiming InstantAt(const TimeStamp& aTime) {
    MOZ_ASSERT(!aTime.IsNull(), "Time is null for an instant marker.");
    return MarkerTiming{aTime, TimeStamp{}, MarkerTiming::Phase::Instant};
  }

  static MarkerTiming InstantNow() { return InstantAt(TimeStamp::Now()); }

  static MarkerTiming Interval(const TimeStamp& aStartTime,
                               const TimeStamp& aEndTime) {
    MOZ_ASSERT(!aStartTime.IsNull(),
               "Start time is null for an interval marker.");
    MOZ_ASSERT(!aEndTime.IsNull(), "End time is null for an interval marker.");
    return MarkerTiming{aStartTime, aEndTime, MarkerTiming::Phase::Interval};
  }

  static MarkerTiming IntervalUntilNowFrom(const TimeStamp& aStartTime) {
    return Interval(aStartTime, TimeStamp::Now());
  }

  static MarkerTiming IntervalStart(const TimeStamp& aTime = TimeStamp::Now()) {
    MOZ_ASSERT(!aTime.IsNull(), "Time is null for an interval start marker.");
    return MarkerTiming{aTime, TimeStamp{}, MarkerTiming::Phase::IntervalStart};
  }

  static MarkerTiming IntervalEnd(const TimeStamp& aTime = TimeStamp::Now()) {
    MOZ_ASSERT(!aTime.IsNull(), "Time is null for an interval end marker.");
    return MarkerTiming{TimeStamp{}, aTime, MarkerTiming::Phase::IntervalEnd};
  }

  // Set the interval end in this timing.
  // If there was already a start time, this makes it a full interval.
  void SetIntervalEnd(const TimeStamp& aTime = TimeStamp::Now()) {
    MOZ_ASSERT(!aTime.IsNull(), "Time is null for an interval end marker.");
    mEndTime = aTime;
    mPhase = mStartTime.IsNull() ? Phase::IntervalEnd : Phase::Interval;
  }

  [[nodiscard]] const TimeStamp& StartTime() const { return mStartTime; }
  [[nodiscard]] const TimeStamp& EndTime() const { return mEndTime; }

  // The phase differentiates Instant markers from Interval markers.
  // Interval markers can either carry both timestamps on a single marker,
  // or they can be split into individual Start and End markers, which are
  // associated with each other via the marker name.
  //
  // The numeric representation of this enum value is also exposed in the
  // ETW trace event's Phase field.
  enum class Phase : uint8_t {
    Instant = 0,
    Interval = 1,
    IntervalStart = 2,
    IntervalEnd = 3,
  };

  [[nodiscard]] Phase MarkerPhase() const {
    MOZ_ASSERT(!IsUnspecified());
    return mPhase;
  }

  // The following getter methods are used to put the value into the buffer for
  // storage.
  [[nodiscard]] double GetStartTime() const {
    MOZ_ASSERT(!IsUnspecified());
    // If mStartTime is null (e.g., for IntervalEnd), this will output 0.0 as
    // expected.
    return MarkerTiming::timeStampToDouble(mStartTime);
  }

  [[nodiscard]] double GetEndTime() const {
    MOZ_ASSERT(!IsUnspecified());
    // If mEndTime is null (e.g., for Instant or IntervalStart), this will
    // output 0.0 as expected.
    return MarkerTiming::timeStampToDouble(mEndTime);
  }

  [[nodiscard]] uint8_t GetPhase() const {
    MOZ_ASSERT(!IsUnspecified());
    return static_cast<uint8_t>(mPhase);
  }

  // This is a constructor for Rust FFI bindings. It must not be used outside of
  // this! Please see the other static constructors above.
  static void UnsafeConstruct(MarkerTiming* aMarkerTiming,
                              const TimeStamp& aStartTime,
                              const TimeStamp& aEndTime, Phase aPhase) {
    new (aMarkerTiming) MarkerTiming{aStartTime, aEndTime, aPhase};
  }

 private:
  friend ProfileBufferEntryWriter::Serializer<MarkerTiming>;
  friend ProfileBufferEntryReader::Deserializer<MarkerTiming>;
  friend MarkerOptions;

  // Default timing leaves it internally "unspecified", serialization getters
  // and add-marker functions will default to `InstantNow()`.
  constexpr MarkerTiming() = default;

  // This should only be used by internal profiler code.
  [[nodiscard]] bool IsUnspecified() const {
    return mStartTime.IsNull() && mEndTime.IsNull();
  }

  // Full constructor, used by static factory functions.
  constexpr MarkerTiming(const TimeStamp& aStartTime, const TimeStamp& aEndTime,
                         Phase aPhase)
      : mStartTime(aStartTime), mEndTime(aEndTime), mPhase(aPhase) {}

  static double timeStampToDouble(const TimeStamp& time) {
    if (time.IsNull()) {
      // The Phase lets us know not to use this value.
      return 0;
    }
    return (time - TimeStamp::ProcessCreation()).ToMilliseconds();
  }

  TimeStamp mStartTime;
  TimeStamp mEndTime;
  Phase mPhase = Phase::Instant;
};

// This marker option allows three cases:
// - By default, no stacks are captured.
// - The caller can request a stack capture, and the add-marker code will take
//   care of it in the most efficient way.
// - The caller can still provide an existing backtrace, for cases where a
//   marker reports something that happened elsewhere.
class MarkerStack {
 public:
  // Default constructor, no capture.
  constexpr MarkerStack() = default;

  // Disallow copy.
  MarkerStack(const MarkerStack&) = delete;
  MarkerStack& operator=(const MarkerStack&) = delete;

  // Allow move.
  MarkerStack(MarkerStack&& aOther)
      : mCaptureOptions(aOther.mCaptureOptions),
        mOptionalChunkedBufferStorage(
            std::move(aOther.mOptionalChunkedBufferStorage)),
        mChunkedBuffer(aOther.mChunkedBuffer) {
    AssertInvariants();
    aOther.Clear();
  }
  MarkerStack& operator=(MarkerStack&& aOther) {
    mCaptureOptions = aOther.mCaptureOptions;
    mOptionalChunkedBufferStorage =
        std::move(aOther.mOptionalChunkedBufferStorage);
    mChunkedBuffer = aOther.mChunkedBuffer;
    AssertInvariants();
    aOther.Clear();
    return *this;
  }

  // Take ownership of a backtrace. If null or empty, equivalent to NoStack().
  explicit MarkerStack(UniquePtr<ProfileChunkedBuffer>&& aExternalChunkedBuffer)
      : mOptionalChunkedBufferStorage(
            (!aExternalChunkedBuffer || aExternalChunkedBuffer->IsEmpty())
                ? nullptr
                : std::move(aExternalChunkedBuffer)),
        mChunkedBuffer(mOptionalChunkedBufferStorage.get()) {
    AssertInvariants();
  }

  // Use an existing backtrace stored elsewhere, which the user must guarantee
  // is alive during the add-marker call. If empty, equivalent to NoStack().
  explicit MarkerStack(ProfileChunkedBuffer& aExternalChunkedBuffer)
      : mChunkedBuffer(aExternalChunkedBuffer.IsEmpty()
                           ? nullptr
                           : &aExternalChunkedBuffer) {
    AssertInvariants();
  }

  // Don't capture a stack in this marker.
  static MarkerStack NoStack() {
    return MarkerStack(StackCaptureOptions::NoStack);
  }

  // Capture a stack when adding this marker.
  static MarkerStack Capture(
      StackCaptureOptions aCaptureOptions = StackCaptureOptions::Full) {
    // Actual capture will be handled inside profiler_add_marker.
    return MarkerStack(aCaptureOptions);
  }

  // Optionally capture a stack, useful for avoiding long-winded ternaries.
  static MarkerStack MaybeCapture(bool aDoCapture) {
    return aDoCapture ? Capture() : NoStack();
  }

  // Use an existing backtrace stored elsewhere, which the user must guarantee
  // is alive during the add-marker call. If empty, equivalent to NoStack().
  static MarkerStack UseBacktrace(
      ProfileChunkedBuffer& aExternalChunkedBuffer) {
    return MarkerStack(aExternalChunkedBuffer);
  }

  // Take ownership of a backtrace previously captured with
  // `profiler_capture_backtrace()`. If null, equivalent to NoStack().
  static MarkerStack TakeBacktrace(
      UniquePtr<ProfileChunkedBuffer>&& aExternalChunkedBuffer) {
    return MarkerStack(std::move(aExternalChunkedBuffer));
  }

  // Construct with the given capture options.
  static MarkerStack WithCaptureOptions(StackCaptureOptions aCaptureOptions) {
    return MarkerStack(aCaptureOptions);
  }

  [[nodiscard]] StackCaptureOptions CaptureOptions() const {
    return mCaptureOptions;
  }

  ProfileChunkedBuffer* GetChunkedBuffer() const { return mChunkedBuffer; }

  // Use backtrace after a request. If null, equivalent to NoStack().
  void UseRequestedBacktrace(ProfileChunkedBuffer* aExternalChunkedBuffer) {
    MOZ_RELEASE_ASSERT(mCaptureOptions != StackCaptureOptions::NoStack);
    mCaptureOptions = StackCaptureOptions::NoStack;
    if (aExternalChunkedBuffer && !aExternalChunkedBuffer->IsEmpty()) {
      // We only need to use the provided buffer if it is not empty.
      mChunkedBuffer = aExternalChunkedBuffer;
    }
    AssertInvariants();
  }

  void Clear() {
    mCaptureOptions = StackCaptureOptions::NoStack;
    mOptionalChunkedBufferStorage.reset();
    mChunkedBuffer = nullptr;
    AssertInvariants();
  }

 private:
  explicit MarkerStack(StackCaptureOptions aCaptureOptions)
      : mCaptureOptions(aCaptureOptions) {
    AssertInvariants();
  }

  // This should be called after every constructor and non-const function.
  void AssertInvariants() const {
#ifdef DEBUG
    if (mCaptureOptions != StackCaptureOptions::NoStack) {
      MOZ_ASSERT(!mOptionalChunkedBufferStorage,
                 "We should not hold a buffer when capture is requested");
      MOZ_ASSERT(!mChunkedBuffer,
                 "We should not point at a buffer when capture is requested");
    } else {
      if (mOptionalChunkedBufferStorage) {
        MOZ_ASSERT(mChunkedBuffer == mOptionalChunkedBufferStorage.get(),
                   "Non-null mOptionalChunkedBufferStorage must be pointed-at "
                   "by mChunkedBuffer");
      }
      if (mChunkedBuffer) {
        MOZ_ASSERT(!mChunkedBuffer->IsEmpty(),
                   "Non-null mChunkedBuffer must not be empty");
      }
    }
#endif  // DEBUG
  }

  StackCaptureOptions mCaptureOptions = StackCaptureOptions::NoStack;

  // Optional storage for the backtrace, in case it was captured before the
  // add-marker call.
  UniquePtr<ProfileChunkedBuffer> mOptionalChunkedBufferStorage;

  // If not null, this points to the backtrace. It may point to a backtrace
  // temporarily stored on the stack, or to mOptionalChunkedBufferStorage.
  ProfileChunkedBuffer* mChunkedBuffer = nullptr;
};

// This marker option captures a given inner window id.
class MarkerInnerWindowId {
 public:
  // Default constructor, it leaves the id unspecified.
  constexpr MarkerInnerWindowId() = default;

  // Constructor with a specified inner window id.
  constexpr explicit MarkerInnerWindowId(uint64_t i) : mInnerWindowId(i) {}

  // Constructor with either specified inner window id or Nothing.
  constexpr explicit MarkerInnerWindowId(const Maybe<uint64_t>& i)
      : mInnerWindowId(i.valueOr(scNoId)) {}

  // Explicit option with unspecified id.
  constexpr static MarkerInnerWindowId NoId() { return MarkerInnerWindowId{}; }

  [[nodiscard]] bool IsUnspecified() const { return mInnerWindowId == scNoId; }

  [[nodiscard]] constexpr uint64_t Id() const { return mInnerWindowId; }

 private:
  static constexpr uint64_t scNoId = 0;
  uint64_t mInnerWindowId = scNoId;
};

// This class combines each of the possible marker options above.
class MarkerOptions {
 public:
  // Constructor from individual options (including none).
  // Implicit to allow `{}` and one option type as-is.
  // Options that are not provided here are defaulted. In particular, timing
  // defaults to `MarkerTiming::InstantNow()` when the marker is recorded.
  template <typename... Options>
  MOZ_IMPLICIT MarkerOptions(Options&&... aOptions) {
    (Set(std::forward<Options>(aOptions)), ...);
  }

  // Disallow copy.
  MarkerOptions(const MarkerOptions&) = delete;
  MarkerOptions& operator=(const MarkerOptions&) = delete;

  // Allow move.
  MarkerOptions(MarkerOptions&&) = default;
  MarkerOptions& operator=(MarkerOptions&&) = default;

  // The embedded `MarkerTiming` hasn't been specified yet.
  [[nodiscard]] bool IsTimingUnspecified() const {
    return mTiming.IsUnspecified();
  }

  // Each option may be added in a chain by e.g.:
  // `options.Set(MarkerThreadId(123)).Set(MarkerTiming::IntervalEnd())`.
  // When passed to an add-marker function, it must be an rvalue, either created
  // on the spot, or `std::move`d from storage, e.g.:
  // `PROFILER_MARKER_UNTYPED("...", std::move(options).Set(...))`;
  //
  // Options can be read by their name (without "Marker"), e.g.: `o.ThreadId()`.
  // Add "Ref" for a non-const reference, e.g.: `o.ThreadIdRef() = ...;`
#define FUNCTIONS_ON_MEMBER(NAME)                      \
  MarkerOptions& Set(Marker##NAME&& a##NAME)& {        \
    m##NAME = std::move(a##NAME);                      \
    return *this;                                      \
  }                                                    \
                                                       \
  MarkerOptions&& Set(Marker##NAME&& a##NAME)&& {      \
    m##NAME = std::move(a##NAME);                      \
    return std::move(*this);                           \
  }                                                    \
                                                       \
  const Marker##NAME& NAME() const { return m##NAME; } \
                                                       \
  Marker##NAME& NAME##Ref() { return m##NAME; }

  FUNCTIONS_ON_MEMBER(ThreadId);
  FUNCTIONS_ON_MEMBER(Timing);
  FUNCTIONS_ON_MEMBER(Stack);
  FUNCTIONS_ON_MEMBER(InnerWindowId);
#undef FUNCTIONS_ON_MEMBER

 private:
  friend ProfileBufferEntryReader::Deserializer<MarkerOptions>;

  MarkerThreadId mThreadId;
  MarkerTiming mTiming;
  MarkerStack mStack;
  MarkerInnerWindowId mInnerWindowId;
};

}  // namespace mozilla

namespace mozilla::baseprofiler::markers {

// Default marker payload types, with no extra information, not even a marker
// type and payload. This is intended for label-only markers.
struct NoPayload final {};

}  // namespace mozilla::baseprofiler::markers

namespace mozilla {

class JSONWriter;

// This class collects all the information necessary to stream the JSON schema
// that informs the front-end how to display a type of markers.
// It will be created and populated in `MarkerTypeDisplay()` functions in each
// marker type definition, see Add/Set functions.
class MarkerSchema {
 public:
  // This is used to describe a C++ type that is expected to be specified to
  // the marker and used in PayloadField. This type is the expected input type
  // to the marker data.
  enum class InputType {
    Uint64,
    Uint32,
    Uint8,
    Boolean,
    CString,
    String,
    TimeStamp,
    TimeDuration
  };

  enum class Location : unsigned {
    MarkerChart,
    MarkerTable,
    // This adds markers to the main marker timeline in the header.
    TimelineOverview,
    // In the timeline, this is a section that breaks out markers that are
    // related to memory. When memory counters are enabled, this is its own
    // track, otherwise it is displayed with the main thread.
    TimelineMemory,
    // This adds markers to the IPC timeline area in the header.
    TimelineIPC,
    // This adds markers to the FileIO timeline area in the header.
    TimelineFileIO,
    // TODO - This is not supported yet.
    StackChart
  };

  // Used as constructor parameter, to explicitly specify that the location (and
  // other display options) are handled as a special case in the front-end.
  // In this case, *no* schema will be output for this type.
  struct SpecialFrontendLocation {};

  enum class Format {
    // ----------------------------------------------------
    // String types.

    // Show the URL, and handle PII sanitization
    Url,
    // Show the file path, and handle PII sanitization.
    FilePath,
    // Show arbitrary string and handle PII sanitization
    SanitizedString,
    // Important, do not put URL or file path information here, as it will not
    // be sanitized. Please be careful with including other types of PII here as
    // well.
    // e.g. "Label: Some String"
    String,

    // Show a string from a UniqueStringArray given an index in the profile.
    // e.g. 1, given string table ["hello", "world"] will show "world"
    UniqueString,

    // ----------------------------------------------------
    // Numeric types

    // For time data that represents a duration of time.
    // e.g. "Label: 5s, 5ms, 5μs"
    Duration,
    // Data that happened at a specific time, relative to the start of the
    // profile. e.g. "Label: 15.5s, 20.5ms, 30.5μs"
    Time,
    // The following are alternatives to display a time only in a specific unit
    // of time.
    Seconds,       // "Label: 5s"
    Milliseconds,  // "Label: 5ms"
    Microseconds,  // "Label: 5μs"
    Nanoseconds,   // "Label: 5ns"
    // e.g. "Label: 5.55mb, 5 bytes, 312.5kb"
    Bytes,
    // This should be a value between 0 and 1.
    // "Label: 50%"
    Percentage,
    // The integer should be used for generic representations of numbers.
    // Do not use it for time information.
    // "Label: 52, 5,323, 1,234,567"
    Integer,
    // The decimal should be used for generic representations of numbers.
    // Do not use it for time information.
    // "Label: 52.23, 0.0054, 123,456.78"
    Decimal,

    // A flow is a u64 identifier that's unique across processes. All of
    // the markers with same flow id before a terminating flow id will be
    // considered part of the same "flow" and linked together.
    Flow,
    // A terminating flow ends a flow of a particular id and allows that id
    // to be reused again. It often makes sense for destructors to create
    // a marker with a field of this type.
    TerminatingFlow
  };

  // This represents groups of markers which MarkerTypes can expose to indicate
  // what group they belong to (multiple groups are allowed combined in bitwise
  // or). This is currently only used for ETW filtering. In the long run this
  // should be generalized to gecko markers.
  enum class ETWMarkerGroup : uint64_t {
    Generic = 1,
    UserMarkers = 1 << 1,
    Memory = 1 << 2,
    Scheduling = 1 << 3,
    Text = 1 << 4,
    Tracing = 1 << 5
  };

  // Flags which describe additional information for a PayloadField.
  enum class PayloadFlags : uint32_t { None = 0, Searchable = 1 };

  // This is one field of payload to be used for additional marker data.
  struct PayloadField {
    // Key identifying the marker.
    const char* Key;
    // Input type, this represents the data type specified.
    InputType InputTy;
    // Label, additional description.
    const char* Label = nullptr;
    // Format as written to the JSON.
    Format Fmt = Format::String;
    // Optional PayloadFlags.
    PayloadFlags Flags = PayloadFlags::None;
  };

  enum class Searchable { NotSearchable, Searchable };
  enum class GraphType { Line, Bar, FilledLine };
  enum class GraphColor {
    Blue,
    Green,
    Grey,
    Ink,
    Magenta,
    Orange,
    Purple,
    Red,
    Teal,
    Yellow
  };

  // Marker schema, with a non-empty list of locations where markers should be
  // shown.
  // Tech note: Even though `aLocations` are templated arguments, they are
  // assigned to an `enum class` object, so they can only be of that enum type.
  template <typename... Locations>
  explicit MarkerSchema(Location aLocation, Locations... aLocations)
      : mLocations{aLocation, aLocations...} {}

  // Alternative constructor for MarkerSchema.
  explicit MarkerSchema(const mozilla::MarkerSchema::Location* aLocations,
                        size_t aLength)
      : mLocations(aLocations, aLocations + aLength) {}

  // Marker schema for types that have special frontend handling.
  // Nothing else should be set in this case.
  // Implicit to allow quick return from MarkerTypeDisplay functions.
  MOZ_IMPLICIT MarkerSchema(SpecialFrontendLocation) {}

  // Caller must specify location(s) or SpecialFrontendLocation above.
  MarkerSchema() = delete;

  // Optional labels in the marker chart, the chart tooltip, and the marker
  // table. If not provided, the marker "name" will be used. The given string
  // can contain element keys in braces to include data elements streamed by
  // `StreamJSONMarkerData()`. E.g.: "This is {text}"

#define LABEL_SETTER(name)                       \
  MarkerSchema& Set##name(std::string a##name) { \
    m##name = std::move(a##name);                \
    return *this;                                \
  }

  LABEL_SETTER(ChartLabel)
  LABEL_SETTER(TooltipLabel)
  LABEL_SETTER(TableLabel)

#undef LABEL_SETTER

  MarkerSchema& SetAllLabels(std::string aText) {
    // Here we set the same text in each label.
    // TODO: Move to a single "label" field once the front-end allows it.
    SetChartLabel(aText);
    SetTooltipLabel(aText);
    SetTableLabel(std::move(aText));
    return *this;
  }

  MarkerSchema& SetIsStackBased() {
    mIsStackBased = true;
    return *this;
  }

  // Each data element that is streamed by `StreamJSONMarkerData()` can be
  // displayed as indicated by using one of the `Add...` function below.
  // Each `Add...` will add a line in the full marker description. Parameters:
  // - `aKey`: Element property name as streamed by `StreamJSONMarkerData()`.
  // - `aLabel`: Optional prefix. Defaults to the key name.
  // - `aFormat`: How to format the data element value, see `Format` above.
  // - `aSearchable`: Optional, indicates if the value is used in searches,
  //   defaults to false.

  MarkerSchema& AddKeyFormat(std::string aKey, Format aFormat) {
    mData.emplace_back(mozilla::VariantType<DynamicData>{},
                       DynamicData{std::move(aKey), mozilla::Nothing{}, aFormat,
                                   mozilla::Nothing{}});
    return *this;
  }

  MarkerSchema& AddKeyLabelFormat(std::string aKey, std::string aLabel,
                                  Format aFormat) {
    mData.emplace_back(
        mozilla::VariantType<DynamicData>{},
        DynamicData{std::move(aKey), mozilla::Some(std::move(aLabel)), aFormat,
                    mozilla::Nothing{}});
    return *this;
  }

  MarkerSchema& AddKeyFormatSearchable(std::string aKey, Format aFormat,
                                       Searchable aSearchable) {
    mData.emplace_back(mozilla::VariantType<DynamicData>{},
                       DynamicData{std::move(aKey), mozilla::Nothing{}, aFormat,
                                   mozilla::Some(aSearchable)});
    return *this;
  }

  MarkerSchema& AddKeyLabelFormatSearchable(std::string aKey,
                                            std::string aLabel, Format aFormat,
                                            Searchable aSearchable) {
    mData.emplace_back(
        mozilla::VariantType<DynamicData>{},
        DynamicData{std::move(aKey), mozilla::Some(std::move(aLabel)), aFormat,
                    mozilla::Some(aSearchable)});
    return *this;
  }

  // The display may also include static rows.

  MarkerSchema& AddStaticLabelValue(std::string aLabel, std::string aValue) {
    mData.emplace_back(mozilla::VariantType<StaticData>{},
                       StaticData{std::move(aLabel), std::move(aValue)});
    return *this;
  }

  // Markers can be shown as timeline tracks.

  MarkerSchema& AddChart(std::string aKey, GraphType aType) {
    mGraphs.emplace_back(GraphData{std::move(aKey), aType, mozilla::Nothing{}});
    return *this;
  }

  MarkerSchema& AddChartColor(std::string aKey, GraphType aType,
                              GraphColor aColor) {
    mGraphs.emplace_back(
        GraphData{std::move(aKey), aType, mozilla::Some(aColor)});
    return *this;
  }

  // Internal streaming function.
  MFBT_API void Stream(JSONWriter& aWriter, const Span<const char>& aName) &&;

 private:
  MFBT_API static Span<const char> LocationToStringSpan(Location aLocation);
  MFBT_API static Span<const char> FormatToStringSpan(Format aFormat);
  MFBT_API static Span<const char> GraphTypeToStringSpan(GraphType aType);
  MFBT_API static Span<const char> GraphColorToStringSpan(GraphColor aColor);

  // List of marker display locations. Empty for SpecialFrontendLocation.
  std::vector<Location> mLocations;
  // Labels for different places.
  std::string mChartLabel;
  std::string mTooltipLabel;
  std::string mTableLabel;
  bool mIsStackBased = false;
  // Main display, made of zero or more rows of key+label+format or label+value.
 private:
  struct DynamicData {
    std::string mKey;
    mozilla::Maybe<std::string> mLabel;
    Format mFormat;
    mozilla::Maybe<Searchable> mSearchable;
  };
  struct StaticData {
    std::string mLabel;
    std::string mValue;
  };
  using DataRow = mozilla::Variant<DynamicData, StaticData>;
  using DataRowVector = std::vector<DataRow>;

  DataRowVector mData;

  struct GraphData {
    std::string mKey;
    GraphType mType;
    mozilla::Maybe<GraphColor> mColor;
  };
  std::vector<GraphData> mGraphs;
};

namespace detail {
// GCC doesn't allow this to live inside the class.
template <typename PayloadType>
static void StreamPayload(baseprofiler::SpliceableJSONWriter& aWriter,
                          const Span<const char> aKey,
                          const PayloadType& aPayload) {
  aWriter.StringProperty(aKey, aPayload);
}

template <typename PayloadType>
inline void StreamPayload(baseprofiler::SpliceableJSONWriter& aWriter,
                          const Span<const char> aKey,
                          const Maybe<PayloadType>& aPayload) {
  if (aPayload.isSome()) {
    StreamPayload(aWriter, aKey, *aPayload);
  } else {
    aWriter.NullProperty(aKey);
  }
}

template <>
inline void StreamPayload<bool>(baseprofiler::SpliceableJSONWriter& aWriter,
                                const Span<const char> aKey,
                                const bool& aPayload) {
  aWriter.BoolProperty(aKey, aPayload);
}

template <>
inline void StreamPayload<ProfilerString8View>(
    baseprofiler::SpliceableJSONWriter& aWriter, const Span<const char> aKey,
    const ProfilerString8View& aPayload) {
  aWriter.StringProperty(aKey, aPayload);
}

template <>
inline void StreamPayload<Flow>(baseprofiler::SpliceableJSONWriter& aWriter,
                                const Span<const char> aKey,
                                const Flow& aPayload) {
  aWriter.FlowProperty(aKey, aPayload);
}

}  // namespace detail

// This helper class is used by MarkerTypes that want to support the general
// MarkerType object schema. When using this the markers will also transmit
// their payload to the ETW tracer as well as requiring less inline code.
// This is a curiously recurring template, the template argument is the child
// class itself.
template <typename T>
struct BaseMarkerType {
  static constexpr const char* Description = nullptr;

  static constexpr const char* AllLabels = nullptr;
  static constexpr const char* ChartLabel = nullptr;
  static constexpr const char* TableLabel = nullptr;
  static constexpr const char* TooltipLabel = nullptr;

  // Setting this property to true is a promise that the the marker will nest
  // properly.  i.e. it can't have a partially overlapping time range with any
  // other stack based markers on the same thread.
  static constexpr bool IsStackBased = false;

  // This indicates whether this marker type wants the names passed to the
  // individual marker calls stores along with the marker.
  static constexpr bool StoreName = false;

  static constexpr MarkerSchema::ETWMarkerGroup Group =
      MarkerSchema::ETWMarkerGroup::Generic;

  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{T::Locations, std::size(T::Locations)};
    if (T::AllLabels) {
      schema.SetAllLabels(T::AllLabels);
    }
    if (T::ChartLabel) {
      schema.SetChartLabel(T::ChartLabel);
    }
    if (T::TableLabel) {
      schema.SetTableLabel(T::TableLabel);
    }
    if (T::TooltipLabel) {
      schema.SetTooltipLabel(T::TooltipLabel);
    }
    if (T::IsStackBased) {
      schema.SetIsStackBased();
    }
    for (const MS::PayloadField field : T::PayloadFields) {
      if (field.Label) {
        if (uint32_t(field.Flags) & uint32_t(MS::PayloadFlags::Searchable)) {
          schema.AddKeyLabelFormatSearchable(field.Key, field.Label, field.Fmt,
                                             MS::Searchable::Searchable);
        } else {
          schema.AddKeyLabelFormat(field.Key, field.Label, field.Fmt);
        }
      } else {
        if (uint32_t(field.Flags) & uint32_t(MS::PayloadFlags::Searchable)) {
          schema.AddKeyFormatSearchable(field.Key, field.Fmt,
                                        MS::Searchable::Searchable);
        } else {
          schema.AddKeyFormat(field.Key, field.Fmt);
        }
      }
    }
    if (T::Description) {
      schema.AddStaticLabelValue("Description", T::Description);
    }
    return schema;
  }

  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan(T::Name);
  }

  // This is called by the child class since the child class version of this
  // function is used to infer the argument types by the profile buffer and
  // allows the child to do any special data conversion it needs to do.
  // Optionally the child can opt not to use this at all and write the data
  // out itself.
  template <typename... PayloadArguments>
  static void StreamJSONMarkerDataImpl(
      baseprofiler::SpliceableJSONWriter& aWriter,
      const PayloadArguments&... aPayloadArguments) {
    size_t i = 0;
    (detail::StreamPayload(aWriter, MakeStringSpan(T::PayloadFields[i++].Key),
                           aPayloadArguments),
     ...);
  }
};
}  // namespace mozilla

#endif  // BaseProfilerMarkersPrerequisites_h
