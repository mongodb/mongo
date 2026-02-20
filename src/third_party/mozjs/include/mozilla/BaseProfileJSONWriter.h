/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BASEPROFILEJSONWRITER_H
#define BASEPROFILEJSONWRITER_H

#include "mozilla/FailureLatch.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/HashTable.h"
#include "mozilla/JSONWriter.h"
#include "mozilla/Maybe.h"
#include "mozilla/NotNull.h"
#include "mozilla/ProgressLogger.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Flow.h"

#include <functional>
#include <ostream>
#include <string_view>
#include <stdint.h>

namespace mozilla {
namespace baseprofiler {

class SpliceableJSONWriter;

// On average, profile JSONs are large enough such that we want to avoid
// reallocating its buffer when expanding. Additionally, the contents of the
// profile are not accessed until the profile is entirely written. For these
// reasons we use a chunked writer that keeps an array of chunks, which is
// concatenated together after writing is finished.
class ChunkedJSONWriteFunc final : public JSONWriteFunc, public FailureLatch {
 public:
  friend class SpliceableJSONWriter;

  explicit ChunkedJSONWriteFunc(FailureLatch& aFailureLatch)
      : mFailureLatch(WrapNotNullUnchecked(&aFailureLatch)) {
    (void)AllocChunk(kChunkSize);
  }

  [[nodiscard]] bool IsEmpty() const {
    MOZ_ASSERT_IF(!mChunkPtr, !mChunkEnd && mChunkList.length() == 0 &&
                                  mChunkLengths.length() == 0);
    return !mChunkPtr;
  }

  // Length of data written so far, excluding null terminator.
  [[nodiscard]] size_t Length() const {
    MOZ_ASSERT(mChunkLengths.length() == mChunkList.length());
    size_t totalLen = 0;
    for (size_t i = 0; i < mChunkLengths.length(); i++) {
      MOZ_ASSERT(strlen(mChunkList[i].get()) == mChunkLengths[i]);
      totalLen += mChunkLengths[i];
    }
    return totalLen;
  }

  void Write(const Span<const char>& aStr) final {
    if (Failed()) {
      return;
    }

    MOZ_ASSERT(mChunkPtr >= mChunkList.back().get() && mChunkPtr <= mChunkEnd);
    MOZ_ASSERT(mChunkEnd >= mChunkList.back().get() + mChunkLengths.back());
    MOZ_ASSERT(*mChunkPtr == '\0');

    // Most strings to be written are small, but subprocess profiles (e.g.,
    // from the content process in e10s) may be huge. If the string is larger
    // than a chunk, allocate its own chunk.
    char* newPtr;
    if (aStr.size() >= kChunkSize) {
      if (!AllocChunk(aStr.size() + 1)) {
        return;
      }
      newPtr = mChunkPtr + aStr.size();
    } else {
      newPtr = mChunkPtr + aStr.size();
      if (newPtr >= mChunkEnd) {
        if (!AllocChunk(kChunkSize)) {
          return;
        }
        newPtr = mChunkPtr + aStr.size();
      }
    }

    memcpy(mChunkPtr, aStr.data(), aStr.size());
    *newPtr = '\0';
    mChunkPtr = newPtr;
    mChunkLengths.back() += aStr.size();
  }

  [[nodiscard]] bool CopyDataIntoLazilyAllocatedBuffer(
      const std::function<char*(size_t)>& aAllocator) const {
    // Request a buffer for the full content plus a null terminator.
    if (Failed()) {
      return false;
    }

    char* ptr = aAllocator(Length() + 1);

    if (!ptr) {
      // Failed to allocate memory.
      return false;
    }

    for (size_t i = 0; i < mChunkList.length(); i++) {
      size_t len = mChunkLengths[i];
      memcpy(ptr, mChunkList[i].get(), len);
      ptr += len;
    }
    *ptr = '\0';
    return true;
  }

  [[nodiscard]] UniquePtr<char[]> CopyData() const {
    UniquePtr<char[]> c;
    if (!CopyDataIntoLazilyAllocatedBuffer([&](size_t allocationSize) {
          c = MakeUnique<char[]>(allocationSize);
          return c.get();
        })) {
      // Something went wrong, make sure the returned pointer is null even if
      // the allocation happened.
      c = nullptr;
    }
    return c;
  }

  void Take(ChunkedJSONWriteFunc&& aOther) {
    SetFailureFrom(aOther);
    if (Failed()) {
      return;
    }

    for (size_t i = 0; i < aOther.mChunkList.length(); i++) {
      MOZ_ALWAYS_TRUE(mChunkLengths.append(aOther.mChunkLengths[i]));
      MOZ_ALWAYS_TRUE(mChunkList.append(std::move(aOther.mChunkList[i])));
    }
    mChunkPtr = mChunkList.back().get() + mChunkLengths.back();
    mChunkEnd = mChunkPtr;
    aOther.Clear();
  }

  FAILURELATCH_IMPL_PROXY(*mFailureLatch)

  // Change the failure latch to be used here, and if the previous latch was
  // already in failure state, set that failure in the new latch.
  // This allows using this WriteFunc in isolation, before attempting to bring
  // it into another operation group with its own FailureLatch.
  void ChangeFailureLatchAndForwardState(FailureLatch& aFailureLatch) {
    aFailureLatch.SetFailureFrom(*this);
    mFailureLatch = WrapNotNullUnchecked(&aFailureLatch);
  }

 private:
  void Clear() {
    mChunkPtr = nullptr;
    mChunkEnd = nullptr;
    mChunkList.clear();
    mChunkLengths.clear();
  }

  void ClearAndSetFailure(std::string aFailure) {
    Clear();
    SetFailure(std::move(aFailure));
  }

  [[nodiscard]] bool ClearAndSetFailureAndFalse(std::string aFailure) {
    ClearAndSetFailure(std::move(aFailure));
    return false;
  }

  [[nodiscard]] bool AllocChunk(size_t aChunkSize) {
    if (Failed()) {
      if (mChunkPtr) {
        // FailureLatch is in failed state, but chunks have not been cleared yet
        // (error must have happened elsewhere).
        Clear();
      }
      return false;
    }

    MOZ_ASSERT(mChunkLengths.length() == mChunkList.length());
    UniquePtr<char[]> newChunk = MakeUniqueFallible<char[]>(aChunkSize);
    if (!newChunk) {
      return ClearAndSetFailureAndFalse(
          "OOM in ChunkedJSONWriteFunc::AllocChunk allocating new chunk");
    }
    mChunkPtr = newChunk.get();
    mChunkEnd = mChunkPtr + aChunkSize;
    *mChunkPtr = '\0';
    if (!mChunkLengths.append(0)) {
      return ClearAndSetFailureAndFalse(
          "OOM in ChunkedJSONWriteFunc::AllocChunk appending length");
    }
    if (!mChunkList.append(std::move(newChunk))) {
      return ClearAndSetFailureAndFalse(
          "OOM in ChunkedJSONWriteFunc::AllocChunk appending new chunk");
    }
    return true;
  }

  static const size_t kChunkSize = 4096 * 512;

  // Pointer for writing inside the current chunk.
  //
  // The current chunk is always at the back of mChunkList, i.e.,
  // mChunkList.back() <= mChunkPtr <= mChunkEnd.
  char* mChunkPtr = nullptr;

  // Pointer to the end of the current chunk.
  //
  // The current chunk is always at the back of mChunkList, i.e.,
  // mChunkEnd >= mChunkList.back() + mChunkLengths.back().
  char* mChunkEnd = nullptr;

  // List of chunks and their lengths.
  //
  // For all i, the length of the string in mChunkList[i] is
  // mChunkLengths[i].
  Vector<UniquePtr<char[]>> mChunkList;
  Vector<size_t> mChunkLengths;

  NotNull<FailureLatch*> mFailureLatch;
};

struct OStreamJSONWriteFunc final : public JSONWriteFunc {
  explicit OStreamJSONWriteFunc(std::ostream& aStream) : mStream(aStream) {}

  void Write(const Span<const char>& aStr) final {
    std::string_view sv(aStr.data(), aStr.size());
    mStream << sv;
  }

  std::ostream& mStream;
};

class UniqueJSONStrings;

class SpliceableJSONWriter : public JSONWriter, public FailureLatch {
 public:
  SpliceableJSONWriter(JSONWriteFunc& aWriter, FailureLatch& aFailureLatch)
      : JSONWriter(aWriter, JSONWriter::SingleLineStyle),
        mFailureLatch(WrapNotNullUnchecked(&aFailureLatch)) {}

  SpliceableJSONWriter(UniquePtr<JSONWriteFunc> aWriter,
                       FailureLatch& aFailureLatch)
      : JSONWriter(std::move(aWriter), JSONWriter::SingleLineStyle),
        mFailureLatch(WrapNotNullUnchecked(&aFailureLatch)) {}

  void StartBareList() { StartCollection(scEmptyString, scEmptyString); }

  void EndBareList() { EndCollection(scEmptyString); }

  // Output a time (int64_t given in nanoseconds) in milliseconds. trim zeroes.
  // E.g.: 1'234'567'890 -> "1234.56789"
  void TimeI64NsProperty(const Span<const char>& aMaybePropertyName,
                         int64_t aTime_ns) {
    if (aTime_ns == 0) {
      Scalar(aMaybePropertyName, MakeStringSpan("0"));
      return;
    }

    static constexpr int64_t million = 1'000'000;
    const int64_t absNanos = std::abs(aTime_ns);
    const int64_t integerMilliseconds = absNanos / million;
    auto remainderNanoseconds = static_cast<uint32_t>(absNanos % million);

    // Plenty enough to fit INT64_MIN (-9223372036854775808).
    static constexpr size_t DIGITS_MAX = 23;
    char buf[DIGITS_MAX + 1];
    int len =
        snprintf(buf, DIGITS_MAX, (aTime_ns >= 0) ? "%" PRIu64 : "-%" PRIu64,
                 integerMilliseconds);
    if (remainderNanoseconds != 0) {
      buf[len++] = '.';
      // Output up to 6 fractional digits. Exit early if the rest would
      // be trailing zeros.
      uint32_t powerOfTen = static_cast<uint32_t>(million / 10);
      for (;;) {
        auto digit = remainderNanoseconds / powerOfTen;
        buf[len++] = '0' + static_cast<char>(digit);
        remainderNanoseconds %= powerOfTen;
        if (remainderNanoseconds == 0) {
          break;
        }
        powerOfTen /= 10;
        if (powerOfTen == 0) {
          break;
        }
      }
    }

    Scalar(aMaybePropertyName, Span<const char>(buf, len));
  }

  // Output a (double) time in milliseconds, with at best nanosecond precision.
  void TimeDoubleMsProperty(const Span<const char>& aMaybePropertyName,
                            double aTime_ms) {
    const double dTime_ns = aTime_ms * 1'000'000.0;
    // Make sure it's well within int64_t range.
    // 2^63 nanoseconds is almost 300 years; these times are relative to
    // firefox startup, this should be enough for most uses.
    if (dTime_ns >= 0.0) {
      MOZ_RELEASE_ASSERT(dTime_ns < double(INT64_MAX - 1));
    } else {
      MOZ_RELEASE_ASSERT(dTime_ns > double(INT64_MIN + 2));
    }
    // Round to nearest integer nanosecond. The conversion to integer truncates
    // the fractional part, so first we need to push it 0.5 away from zero.
    const int64_t iTime_ns =
        (dTime_ns >= 0.0) ? int64_t(dTime_ns + 0.5) : int64_t(dTime_ns - 0.5);
    TimeI64NsProperty(aMaybePropertyName, iTime_ns);
  }

  // Output a (double) time in milliseconds, with at best nanosecond precision.
  void TimeDoubleMsElement(double aTime_ms) {
    TimeDoubleMsProperty(nullptr, aTime_ms);
  }

  // This function must be used to correctly stream timestamps in profiles.
  // Null timestamps don't output anything.
  void TimeProperty(const Span<const char>& aMaybePropertyName,
                    const TimeStamp& aTime) {
    if (!aTime.IsNull()) {
      TimeDoubleMsProperty(
          aMaybePropertyName,
          (aTime - TimeStamp::ProcessCreation()).ToMilliseconds());
    }
  }

  // JSON doesn't support 64bit integers so we encode them as hex strings
  static std::array<char, 17> HexString(uint64_t aId) {
    std::array<char, 17> buf = {};
    static const char* hex_digits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
      buf[i] = hex_digits[(aId >> (60 - i * 4)) & 0xf];
    }
    buf[16] = '0';  // null terminate the string
    return buf;
  }

  // We store flows as strings because JS can't handle 64 bit numbers in JSON
  void FlowProperty(const Span<const char>& aName, Flow aFlow) {
    UniqueStringProperty(aName, HexString(aFlow.Id()));
  }

  void NullElements(uint32_t aCount) {
    for (uint32_t i = 0; i < aCount; i++) {
      NullElement();
    }
  }

  void Splice(const Span<const char>& aStr) {
    Separator();
    WriteFunc().Write(aStr);
    mNeedComma[mDepth] = true;
  }

  void Splice(const char* aStr, size_t aLen) {
    Separator();
    WriteFunc().Write(Span<const char>(aStr, aLen));
    mNeedComma[mDepth] = true;
  }

  // Splice the given JSON directly in, without quoting.
  void SplicedJSONProperty(const Span<const char>& aMaybePropertyName,
                           const Span<const char>& aJsonValue) {
    Scalar(aMaybePropertyName, aJsonValue);
  }

  void CopyAndSplice(const ChunkedJSONWriteFunc& aFunc) {
    Separator();
    for (size_t i = 0; i < aFunc.mChunkList.length(); i++) {
      WriteFunc().Write(
          Span<const char>(aFunc.mChunkList[i].get(), aFunc.mChunkLengths[i]));
    }
    mNeedComma[mDepth] = true;
  }

  // Takes the chunks from aFunc and write them. If move is not possible
  // (e.g., using OStreamJSONWriteFunc), aFunc's chunks are copied and its
  // storage cleared.
  virtual void TakeAndSplice(ChunkedJSONWriteFunc&& aFunc) {
    Separator();
    for (size_t i = 0; i < aFunc.mChunkList.length(); i++) {
      WriteFunc().Write(
          Span<const char>(aFunc.mChunkList[i].get(), aFunc.mChunkLengths[i]));
    }
    aFunc.mChunkPtr = nullptr;
    aFunc.mChunkEnd = nullptr;
    aFunc.mChunkList.clear();
    aFunc.mChunkLengths.clear();
    mNeedComma[mDepth] = true;
  }

  // Set (or reset) the pointer to a UniqueJSONStrings.
  void SetUniqueStrings(UniqueJSONStrings& aUniqueStrings) {
    MOZ_RELEASE_ASSERT(!mUniqueStrings);
    mUniqueStrings = &aUniqueStrings;
  }

  // Set (or reset) the pointer to a UniqueJSONStrings.
  void ResetUniqueStrings() {
    MOZ_RELEASE_ASSERT(mUniqueStrings);
    mUniqueStrings = nullptr;
  }

  // Add `aStr` to the unique-strings list (if not already there), and write its
  // index as a named object property.
  inline void UniqueStringProperty(const Span<const char>& aName,
                                   const Span<const char>& aStr);

  // Add `aStr` to the unique-strings list (if not already there), and write its
  // index as an array element.
  inline void UniqueStringElement(const Span<const char>& aStr);

  // THe following functions override JSONWriter functions non-virtually. The
  // goal is to try and prevent calls that specify a style, which would be
  // ignored anyway because the whole thing is single-lined. It's fine if some
  // calls still make it through a `JSONWriter&`, no big deal.
  void Start() { JSONWriter::Start(); }
  void StartArrayProperty(const Span<const char>& aName) {
    JSONWriter::StartArrayProperty(aName);
  }
  template <size_t N>
  void StartArrayProperty(const char (&aName)[N]) {
    JSONWriter::StartArrayProperty(Span<const char>(aName, N));
  }
  void StartArrayElement() { JSONWriter::StartArrayElement(); }
  void StartObjectProperty(const Span<const char>& aName) {
    JSONWriter::StartObjectProperty(aName);
  }
  template <size_t N>
  void StartObjectProperty(const char (&aName)[N]) {
    JSONWriter::StartObjectProperty(Span<const char>(aName, N));
  }
  void StartObjectElement() { JSONWriter::StartObjectElement(); }

  FAILURELATCH_IMPL_PROXY(*mFailureLatch)

 protected:
  NotNull<FailureLatch*> mFailureLatch;

 private:
  UniqueJSONStrings* mUniqueStrings = nullptr;
};

class SpliceableChunkedJSONWriter final : public SpliceableJSONWriter {
 public:
  explicit SpliceableChunkedJSONWriter(FailureLatch& aFailureLatch)
      : SpliceableJSONWriter(MakeUnique<ChunkedJSONWriteFunc>(aFailureLatch),
                             aFailureLatch) {}

  // Access the ChunkedJSONWriteFunc as reference-to-const, usually to copy data
  // out.
  const ChunkedJSONWriteFunc& ChunkedWriteFunc() const {
    return ChunkedWriteFuncRef();
  }

  // Access the ChunkedJSONWriteFunc as rvalue-reference, usually to take its
  // data out. This writer shouldn't be used anymore after this.
  ChunkedJSONWriteFunc&& TakeChunkedWriteFunc() {
    ChunkedJSONWriteFunc& ref = ChunkedWriteFuncRef();
#ifdef DEBUG
    mTaken = true;
#endif  //
    return std::move(ref);
  }

  // Adopts the chunks from aFunc without copying.
  void TakeAndSplice(ChunkedJSONWriteFunc&& aFunc) override {
    MOZ_ASSERT(!mTaken);
    Separator();
    ChunkedWriteFuncRef().Take(std::move(aFunc));
    mNeedComma[mDepth] = true;
  }

  void ChangeFailureLatchAndForwardState(FailureLatch& aFailureLatch) {
    mFailureLatch = WrapNotNullUnchecked(&aFailureLatch);
    return ChunkedWriteFuncRef().ChangeFailureLatchAndForwardState(
        aFailureLatch);
  }

 private:
  const ChunkedJSONWriteFunc& ChunkedWriteFuncRef() const {
    MOZ_ASSERT(!mTaken);
    // The WriteFunc was non-fallibly allocated as a ChunkedJSONWriteFunc in the
    // only constructor above, so it's safe to cast to ChunkedJSONWriteFunc&.
    return static_cast<const ChunkedJSONWriteFunc&>(WriteFunc());
  }

  ChunkedJSONWriteFunc& ChunkedWriteFuncRef() {
    MOZ_ASSERT(!mTaken);
    // The WriteFunc was non-fallibly allocated as a ChunkedJSONWriteFunc in the
    // only constructor above, so it's safe to cast to ChunkedJSONWriteFunc&.
    return static_cast<ChunkedJSONWriteFunc&>(WriteFunc());
  }

#ifdef DEBUG
  bool mTaken = false;
#endif
};

class JSONSchemaWriter {
  JSONWriter& mWriter;
  uint32_t mIndex;

 public:
  explicit JSONSchemaWriter(JSONWriter& aWriter) : mWriter(aWriter), mIndex(0) {
    aWriter.StartObjectProperty("schema",
                                SpliceableJSONWriter::SingleLineStyle);
  }

  void WriteField(const Span<const char>& aName) {
    mWriter.IntProperty(aName, mIndex++);
  }

  template <size_t Np1>
  void WriteField(const char (&aName)[Np1]) {
    WriteField(Span<const char>(aName, Np1 - 1));
  }

  ~JSONSchemaWriter() { mWriter.EndObject(); }
};

// This class helps create an indexed list of unique strings, and inserts the
// index as a JSON value. The collected list of unique strings can later be
// inserted as a JSON array.
// This can be useful for elements/properties with many repeated strings.
//
// With only JSONWriter w,
// `w.WriteElement("a"); w.WriteElement("b"); w.WriteElement("a");`
// when done inside a JSON array, will generate:
// `["a", "b", "c"]`
//
// With UniqueStrings u,
// `u.WriteElement(w, "a"); u.WriteElement(w, "b"); u.WriteElement(w, "a");`
// when done inside a JSON array, will generate:
// `[0, 1, 0]`
// and later, `u.SpliceStringTableElements(w)` (inside a JSON array), will
// output the corresponding indexed list of unique strings:
// `["a", "b"]`
class UniqueJSONStrings final : public FailureLatch {
 public:
  // Start an empty list of unique strings.
  MFBT_API explicit UniqueJSONStrings(FailureLatch& aFailureLatch);

  // Start with a copy of the strings from another list.
  MFBT_API UniqueJSONStrings(FailureLatch& aFailureLatch,
                             const UniqueJSONStrings& aOther,
                             ProgressLogger aProgressLogger);

  MFBT_API ~UniqueJSONStrings();

  // Add `aStr` to the list (if not already there), and write its index as a
  // named object property.
  void WriteProperty(SpliceableJSONWriter& aWriter,
                     const Span<const char>& aName,
                     const Span<const char>& aStr) {
    if (const Maybe<uint32_t> maybeIndex = GetOrAddIndex(aStr); maybeIndex) {
      aWriter.IntProperty(aName, *maybeIndex);
    } else {
      aWriter.SetFailureFrom(*this);
    }
  }

  // Add `aStr` to the list (if not already there), and write its index as an
  // array element.
  void WriteElement(SpliceableJSONWriter& aWriter,
                    const Span<const char>& aStr) {
    if (const Maybe<uint32_t> maybeIndex = GetOrAddIndex(aStr); maybeIndex) {
      aWriter.IntElement(*maybeIndex);
    } else if (!aWriter.Failed()) {
      aWriter.SetFailureFrom(*this);
    }
  }

  // Splice all collected unique strings into an array. This should only be done
  // once, and then this UniqueStrings shouldn't be used anymore.
  MFBT_API void SpliceStringTableElements(SpliceableJSONWriter& aWriter);

  FAILURELATCH_IMPL_PROXY(mStringTableWriter)

  void ChangeFailureLatchAndForwardState(FailureLatch& aFailureLatch) {
    mStringTableWriter.ChangeFailureLatchAndForwardState(aFailureLatch);
  }

 private:
  MFBT_API void ClearAndSetFailure(std::string aFailure);

  // If `aStr` is already listed, return its index.
  // Otherwise add it to the list and return the new index.
  MFBT_API Maybe<uint32_t> GetOrAddIndex(const Span<const char>& aStr);

  SpliceableChunkedJSONWriter mStringTableWriter;
  HashMap<HashNumber, uint32_t> mStringHashToIndexMap;
};

void SpliceableJSONWriter::UniqueStringProperty(const Span<const char>& aName,
                                                const Span<const char>& aStr) {
  MOZ_RELEASE_ASSERT(mUniqueStrings);
  mUniqueStrings->WriteProperty(*this, aName, aStr);
}

// Add `aStr` to the list (if not already there), and write its index as an
// array element.
void SpliceableJSONWriter::UniqueStringElement(const Span<const char>& aStr) {
  MOZ_RELEASE_ASSERT(mUniqueStrings);
  mUniqueStrings->WriteElement(*this, aStr);
}

}  // namespace baseprofiler
}  // namespace mozilla

#endif  // BASEPROFILEJSONWRITER_H
