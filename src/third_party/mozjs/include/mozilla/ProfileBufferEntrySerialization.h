/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferEntrySerialization_h
#define ProfileBufferEntrySerialization_h

#include "mozilla/Assertions.h"
#include "mozilla/leb128iterator.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/ProfileBufferIndex.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Unused.h"
#include "mozilla/Variant.h"

#include <string>
#include <tuple>

namespace mozilla {

class ProfileBufferEntryWriter;

// Iterator-like class used to read from an entry.
// An entry may be split in two memory segments (e.g., the ends of a ring
// buffer, or two chunks of a chunked buffer); it doesn't deal with this
// underlying buffer, but only with one or two spans pointing at the space
// where the entry lives.
class ProfileBufferEntryReader {
 public:
  using Byte = uint8_t;
  using Length = uint32_t;

  using SpanOfConstBytes = Span<const Byte>;

  // Class to be specialized for types to be read from a profile buffer entry.
  // See common specializations at the bottom of this header.
  // The following static functions must be provided:
  //   static void ReadInto(EntryReader aER&, T& aT)
  //   {
  //     /* Call `aER.ReadX(...)` function to deserialize into aT, be sure to
  //        read exactly `Bytes(aT)`! */
  //   }
  //   static T Read(EntryReader& aER) {
  //     /* Call `aER.ReadX(...)` function to deserialize and return a `T`, be
  //        sure to read exactly `Bytes(returned value)`! */
  //   }
  template <typename T>
  struct Deserializer;

  ProfileBufferEntryReader() = default;

  // Reader over one Span.
  ProfileBufferEntryReader(SpanOfConstBytes aSpan,
                           ProfileBufferBlockIndex aCurrentBlockIndex,
                           ProfileBufferBlockIndex aNextBlockIndex)
      : mCurrentSpan(aSpan),
        mNextSpanOrEmpty(aSpan.Last(0)),
        mCurrentBlockIndex(aCurrentBlockIndex),
        mNextBlockIndex(aNextBlockIndex) {
    // 2nd internal Span points at the end of the 1st internal Span, to enforce
    // invariants.
    CheckInvariants();
  }

  // Reader over two Spans, the second one must not be empty.
  ProfileBufferEntryReader(SpanOfConstBytes aSpanHead,
                           SpanOfConstBytes aSpanTail,
                           ProfileBufferBlockIndex aCurrentBlockIndex,
                           ProfileBufferBlockIndex aNextBlockIndex)
      : mCurrentSpan(aSpanHead),
        mNextSpanOrEmpty(aSpanTail),
        mCurrentBlockIndex(aCurrentBlockIndex),
        mNextBlockIndex(aNextBlockIndex) {
    MOZ_RELEASE_ASSERT(!mNextSpanOrEmpty.IsEmpty());
    if (MOZ_UNLIKELY(mCurrentSpan.IsEmpty())) {
      // First span is already empty, skip it.
      mCurrentSpan = mNextSpanOrEmpty;
      mNextSpanOrEmpty = mNextSpanOrEmpty.Last(0);
    }
    CheckInvariants();
  }

  // Allow copying, which is needed when used as an iterator in some std
  // functions (e.g., string assignment), and to occasionally backtrack.
  // Be aware that the main profile buffer APIs give a reference to an entry
  // reader, and expect that reader to advance to the end of the entry, so don't
  // just advance copies!
  ProfileBufferEntryReader(const ProfileBufferEntryReader&) = default;
  ProfileBufferEntryReader& operator=(const ProfileBufferEntryReader&) =
      default;

  // Don't =default moving, as it doesn't bring any benefit in this class.

  [[nodiscard]] Length RemainingBytes() const {
    return mCurrentSpan.LengthBytes() + mNextSpanOrEmpty.LengthBytes();
  }

  void SetRemainingBytes(Length aBytes) {
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    if (aBytes <= mCurrentSpan.LengthBytes()) {
      mCurrentSpan = mCurrentSpan.First(aBytes);
      mNextSpanOrEmpty = mCurrentSpan.Last(0);
    } else {
      mNextSpanOrEmpty =
          mNextSpanOrEmpty.First(aBytes - mCurrentSpan.LengthBytes());
    }
  }

  [[nodiscard]] ProfileBufferBlockIndex CurrentBlockIndex() const {
    return mCurrentBlockIndex;
  }

  [[nodiscard]] ProfileBufferBlockIndex NextBlockIndex() const {
    return mNextBlockIndex;
  }

  // Create a reader of size zero, pointing at aOffset past the current position
  // of this Reader, so it can be used as end iterator.
  [[nodiscard]] ProfileBufferEntryReader EmptyIteratorAtOffset(
      Length aOffset) const {
    MOZ_RELEASE_ASSERT(aOffset <= RemainingBytes());
    if (MOZ_LIKELY(aOffset < mCurrentSpan.LengthBytes())) {
      // aOffset is before the end of mCurrentSpan.
      return ProfileBufferEntryReader(mCurrentSpan.Subspan(aOffset, 0),
                                      mCurrentBlockIndex, mNextBlockIndex);
    }
    // aOffset is right at the end of mCurrentSpan, or inside mNextSpanOrEmpty.
    return ProfileBufferEntryReader(
        mNextSpanOrEmpty.Subspan(aOffset - mCurrentSpan.LengthBytes(), 0),
        mCurrentBlockIndex, mNextBlockIndex);
  }

  // Be like a limited input iterator, with only `*`, prefix-`++`, `==`, `!=`.
  // These definitions are expected by std functions, to recognize this as an
  // iterator. See https://en.cppreference.com/w/cpp/iterator/iterator_traits
  using difference_type = std::make_signed_t<Length>;
  using value_type = Byte;
  using pointer = const Byte*;
  using reference = const Byte&;
  using iterator_category = std::input_iterator_tag;

  [[nodiscard]] const Byte& operator*() {
    // Assume the caller will read from the returned reference (and not just
    // take the address).
    MOZ_RELEASE_ASSERT(mCurrentSpan.LengthBytes() >= 1);
    return *(mCurrentSpan.Elements());
  }

  ProfileBufferEntryReader& operator++() {
    MOZ_RELEASE_ASSERT(mCurrentSpan.LengthBytes() >= 1);
    if (MOZ_LIKELY(mCurrentSpan.LengthBytes() > 1)) {
      // More than 1 byte left in mCurrentSpan, just eat it.
      mCurrentSpan = mCurrentSpan.From(1);
    } else {
      // mCurrentSpan will be empty, move mNextSpanOrEmpty to mCurrentSpan.
      mCurrentSpan = mNextSpanOrEmpty;
      mNextSpanOrEmpty = mNextSpanOrEmpty.Last(0);
    }
    CheckInvariants();
    return *this;
  }

  ProfileBufferEntryReader& operator+=(Length aBytes) {
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    if (MOZ_LIKELY(aBytes <= mCurrentSpan.LengthBytes())) {
      // All bytes are in mCurrentSpan.
      // Update mCurrentSpan past the read bytes.
      mCurrentSpan = mCurrentSpan.From(aBytes);
      if (mCurrentSpan.IsEmpty() && !mNextSpanOrEmpty.IsEmpty()) {
        // Don't leave mCurrentSpan empty, move non-empty mNextSpanOrEmpty into
        // mCurrentSpan.
        mCurrentSpan = mNextSpanOrEmpty;
        mNextSpanOrEmpty = mNextSpanOrEmpty.Last(0);
      }
    } else {
      // mCurrentSpan does not hold enough bytes.
      // This should only happen at most once: Only for double spans, and when
      // data crosses the gap.
      const Length tail =
          aBytes - static_cast<Length>(mCurrentSpan.LengthBytes());
      // Move mNextSpanOrEmpty to mCurrentSpan, past the data. So the next call
      // will go back to the true case above.
      mCurrentSpan = mNextSpanOrEmpty.From(tail);
      mNextSpanOrEmpty = mNextSpanOrEmpty.Last(0);
    }
    CheckInvariants();
    return *this;
  }

  [[nodiscard]] bool operator==(const ProfileBufferEntryReader& aOther) const {
    return mCurrentSpan.Elements() == aOther.mCurrentSpan.Elements();
  }
  [[nodiscard]] bool operator!=(const ProfileBufferEntryReader& aOther) const {
    return mCurrentSpan.Elements() != aOther.mCurrentSpan.Elements();
  }

  // Read an unsigned LEB128 number and move iterator ahead.
  template <typename T>
  [[nodiscard]] T ReadULEB128() {
    return ::mozilla::ReadULEB128<T>(*this);
  }

  // This struct points at a number of bytes through either one span, or two
  // separate spans (in the rare cases when it is split between two chunks).
  // So the possibilities are:
  // - Totally empty: { [] [] }
  // - First span is not empty: { [content] [] } (Most common case.)
  // - Both spans are not empty: { [cont] [ent] }
  // But something like { [] [content] } is not possible.
  //
  // Recommended usage patterns:
  // - Call a utility function like `CopyBytesTo` if you always need to copy the
  //   data to an outside buffer, e.g., to deserialize an aligned object.
  // - Access both spans one after the other; Note that the second one may be
  //   empty; and the fist could be empty as well if there is no data at all.
  // - Check is the second span is empty, in which case you only need to read
  //   the first one; and since its part of a chunk, it may be directly passed
  //   as an unaligned pointer or reference, thereby saving one copy. But
  //   remember to always handle the double-span case as well.
  //
  // Reminder: An empty span still has a non-null pointer, so it's safe to use
  // with functions like memcpy.
  struct DoubleSpanOfConstBytes {
    SpanOfConstBytes mFirstOrOnly;
    SpanOfConstBytes mSecondOrEmpty;

    void CheckInvariants() const {
      MOZ_ASSERT(mFirstOrOnly.IsEmpty() ? mSecondOrEmpty.IsEmpty() : true,
                 "mSecondOrEmpty should not be the only span to contain data");
    }

    DoubleSpanOfConstBytes() : mFirstOrOnly(), mSecondOrEmpty() {
      CheckInvariants();
    }

    DoubleSpanOfConstBytes(const Byte* aOnlyPointer, size_t aOnlyLength)
        : mFirstOrOnly(aOnlyPointer, aOnlyLength), mSecondOrEmpty() {
      CheckInvariants();
    }

    DoubleSpanOfConstBytes(const Byte* aFirstPointer, size_t aFirstLength,
                           const Byte* aSecondPointer, size_t aSecondLength)
        : mFirstOrOnly(aFirstPointer, aFirstLength),
          mSecondOrEmpty(aSecondPointer, aSecondLength) {
      CheckInvariants();
    }

    // Is there no data at all?
    [[nodiscard]] bool IsEmpty() const {
      // We only need to check the first span, because if it's empty, the second
      // one must be empty as well.
      return mFirstOrOnly.IsEmpty();
    }

    // Total length (in bytes) pointed at by both spans.
    [[nodiscard]] size_t LengthBytes() const {
      return mFirstOrOnly.LengthBytes() + mSecondOrEmpty.LengthBytes();
    }

    // Utility functions to copy all `LengthBytes()` to a given buffer.
    void CopyBytesTo(void* aDest) const {
      memcpy(aDest, mFirstOrOnly.Elements(), mFirstOrOnly.LengthBytes());
      if (MOZ_UNLIKELY(!mSecondOrEmpty.IsEmpty())) {
        memcpy(static_cast<Byte*>(aDest) + mFirstOrOnly.LengthBytes(),
               mSecondOrEmpty.Elements(), mSecondOrEmpty.LengthBytes());
      }
    }

    // If the second span is empty, only the first span may point at data.
    [[nodiscard]] bool IsSingleSpan() const { return mSecondOrEmpty.IsEmpty(); }
  };

  // Get Span(s) to a sequence of bytes, see `DoubleSpanOfConstBytes` for usage.
  // Note that the reader location is *not* updated, do `+=` on it afterwards.
  [[nodiscard]] DoubleSpanOfConstBytes PeekSpans(Length aBytes) const {
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    if (MOZ_LIKELY(aBytes <= mCurrentSpan.LengthBytes())) {
      // All `aBytes` are in the current chunk, only one span is needed.
      return DoubleSpanOfConstBytes{mCurrentSpan.Elements(), aBytes};
    }
    // Otherwise the first span covers then end of the current chunk, and the
    // second span starts in the next chunk.
    return DoubleSpanOfConstBytes{
        mCurrentSpan.Elements(), mCurrentSpan.LengthBytes(),
        mNextSpanOrEmpty.Elements(), aBytes - mCurrentSpan.LengthBytes()};
  }

  // Get Span(s) to a sequence of bytes, see `DoubleSpanOfConstBytes` for usage,
  // and move the reader forward.
  [[nodiscard]] DoubleSpanOfConstBytes ReadSpans(Length aBytes) {
    DoubleSpanOfConstBytes spans = PeekSpans(aBytes);
    (*this) += aBytes;
    return spans;
  }

  // Read a sequence of bytes, like memcpy.
  void ReadBytes(void* aDest, Length aBytes) {
    DoubleSpanOfConstBytes spans = ReadSpans(aBytes);
    MOZ_ASSERT(spans.LengthBytes() == aBytes);
    spans.CopyBytesTo(aDest);
  }

  template <typename T>
  void ReadIntoObject(T& aObject) {
    Deserializer<T>::ReadInto(*this, aObject);
  }

  // Read into one or more objects, sequentially.
  // `EntryReader::ReadIntoObjects()` with nothing is implicitly allowed, this
  // could be useful for generic programming.
  template <typename... Ts>
  void ReadIntoObjects(Ts&... aTs) {
    (ReadIntoObject(aTs), ...);
  }

  // Read data as an object and move iterator ahead.
  template <typename T>
  [[nodiscard]] T ReadObject() {
    T ob = Deserializer<T>::Read(*this);
    return ob;
  }

 private:
  friend class ProfileBufferEntryWriter;

  // Invariants:
  // - mCurrentSpan cannot be empty unless mNextSpanOrEmpty is also empty. So
  //   mCurrentSpan always points at the next byte to read or the end.
  // - If mNextSpanOrEmpty is empty, it points at the end of mCurrentSpan. So
  //   when reaching the end of mCurrentSpan, we can blindly move
  //   mNextSpanOrEmpty to mCurrentSpan and keep the invariants.
  SpanOfConstBytes mCurrentSpan;
  SpanOfConstBytes mNextSpanOrEmpty;
  ProfileBufferBlockIndex mCurrentBlockIndex;
  ProfileBufferBlockIndex mNextBlockIndex;

  void CheckInvariants() const {
    MOZ_ASSERT(!mCurrentSpan.IsEmpty() || mNextSpanOrEmpty.IsEmpty());
    MOZ_ASSERT(!mNextSpanOrEmpty.IsEmpty() ||
               (mNextSpanOrEmpty == mCurrentSpan.Last(0)));
  }
};

// Iterator-like class used to write into an entry.
// An entry may be split in two memory segments (e.g., the ends of a ring
// buffer, or two chunks of a chunked buffer); it doesn't deal with this
// underlying buffer, but only with one or two spans pointing at the space
// reserved for the entry.
class ProfileBufferEntryWriter {
 public:
  using Byte = uint8_t;
  using Length = uint32_t;

  using SpanOfBytes = Span<Byte>;

  // Class to be specialized for types to be written in an entry.
  // See common specializations at the bottom of this header.
  // The following static functions must be provided:
  //   static Length Bytes(const T& aT) {
  //     /* Return number of bytes that will be written. */
  //   }
  //   static void Write(ProfileBufferEntryWriter& aEW,
  //                     const T& aT) {
  //     /* Call `aEW.WriteX(...)` functions to serialize aT, be sure to write
  //        exactly `Bytes(aT)` bytes! */
  //   }
  template <typename T>
  struct Serializer;

  ProfileBufferEntryWriter() = default;

  ProfileBufferEntryWriter(SpanOfBytes aSpan,
                           ProfileBufferBlockIndex aCurrentBlockIndex,
                           ProfileBufferBlockIndex aNextBlockIndex)
      : mCurrentSpan(aSpan),
        mCurrentBlockIndex(aCurrentBlockIndex),
        mNextBlockIndex(aNextBlockIndex) {}

  ProfileBufferEntryWriter(SpanOfBytes aSpanHead, SpanOfBytes aSpanTail,
                           ProfileBufferBlockIndex aCurrentBlockIndex,
                           ProfileBufferBlockIndex aNextBlockIndex)
      : mCurrentSpan(aSpanHead),
        mNextSpanOrEmpty(aSpanTail),
        mCurrentBlockIndex(aCurrentBlockIndex),
        mNextBlockIndex(aNextBlockIndex) {
    // Either:
    // - mCurrentSpan is not empty, OR
    // - mNextSpanOrEmpty is empty if mNextSpanOrEmpty is empty as well.
    MOZ_RELEASE_ASSERT(!mCurrentSpan.IsEmpty() || mNextSpanOrEmpty.IsEmpty());
  }

  // Disable copying and moving, so we can't have multiple writing heads.
  ProfileBufferEntryWriter(const ProfileBufferEntryWriter&) = delete;
  ProfileBufferEntryWriter& operator=(const ProfileBufferEntryWriter&) = delete;
  ProfileBufferEntryWriter(ProfileBufferEntryWriter&&) = delete;
  ProfileBufferEntryWriter& operator=(ProfileBufferEntryWriter&&) = delete;

  void Set() {
    mCurrentSpan = SpanOfBytes{};
    mNextSpanOrEmpty = SpanOfBytes{};
    mCurrentBlockIndex = nullptr;
    mNextBlockIndex = nullptr;
  }

  void Set(SpanOfBytes aSpan, ProfileBufferBlockIndex aCurrentBlockIndex,
           ProfileBufferBlockIndex aNextBlockIndex) {
    mCurrentSpan = aSpan;
    mNextSpanOrEmpty = SpanOfBytes{};
    mCurrentBlockIndex = aCurrentBlockIndex;
    mNextBlockIndex = aNextBlockIndex;
  }

  void Set(SpanOfBytes aSpan0, SpanOfBytes aSpan1,
           ProfileBufferBlockIndex aCurrentBlockIndex,
           ProfileBufferBlockIndex aNextBlockIndex) {
    mCurrentSpan = aSpan0;
    mNextSpanOrEmpty = aSpan1;
    mCurrentBlockIndex = aCurrentBlockIndex;
    mNextBlockIndex = aNextBlockIndex;
    // Either:
    // - mCurrentSpan is not empty, OR
    // - mNextSpanOrEmpty is empty if mNextSpanOrEmpty is empty as well.
    MOZ_RELEASE_ASSERT(!mCurrentSpan.IsEmpty() || mNextSpanOrEmpty.IsEmpty());
  }

  [[nodiscard]] Length RemainingBytes() const {
    return mCurrentSpan.LengthBytes() + mNextSpanOrEmpty.LengthBytes();
  }

  [[nodiscard]] ProfileBufferBlockIndex CurrentBlockIndex() const {
    return mCurrentBlockIndex;
  }

  [[nodiscard]] ProfileBufferBlockIndex NextBlockIndex() const {
    return mNextBlockIndex;
  }

  // Be like a limited output iterator, with only `*` and prefix-`++`.
  // These definitions are expected by std functions, to recognize this as an
  // iterator. See https://en.cppreference.com/w/cpp/iterator/iterator_traits
  using value_type = Byte;
  using pointer = Byte*;
  using reference = Byte&;
  using iterator_category = std::output_iterator_tag;

  [[nodiscard]] Byte& operator*() {
    MOZ_RELEASE_ASSERT(RemainingBytes() >= 1);
    return *(
        (MOZ_LIKELY(!mCurrentSpan.IsEmpty()) ? mCurrentSpan : mNextSpanOrEmpty)
            .Elements());
  }

  ProfileBufferEntryWriter& operator++() {
    if (MOZ_LIKELY(mCurrentSpan.LengthBytes() >= 1)) {
      // There is at least 1 byte in mCurrentSpan, eat it.
      mCurrentSpan = mCurrentSpan.From(1);
    } else {
      // mCurrentSpan is empty, move mNextSpanOrEmpty (past the first byte) to
      // mCurrentSpan.
      MOZ_RELEASE_ASSERT(mNextSpanOrEmpty.LengthBytes() >= 1);
      mCurrentSpan = mNextSpanOrEmpty.From(1);
      mNextSpanOrEmpty = mNextSpanOrEmpty.First(0);
    }
    return *this;
  }

  ProfileBufferEntryWriter& operator+=(Length aBytes) {
    // Note: This is a rare operation. The code below is a copy of `WriteBytes`
    // but without the `memcpy`s.
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    if (MOZ_LIKELY(aBytes <= mCurrentSpan.LengthBytes())) {
      // Data fits in mCurrentSpan.
      // Update mCurrentSpan. It may become empty, so in case of a double span,
      // the next call will go to the false case below.
      mCurrentSpan = mCurrentSpan.From(aBytes);
    } else {
      // Data does not fully fit in mCurrentSpan.
      // This should only happen at most once: Only for double spans, and when
      // data crosses the gap or starts there.
      const Length tail =
          aBytes - static_cast<Length>(mCurrentSpan.LengthBytes());
      // Move mNextSpanOrEmpty to mCurrentSpan, past the data. So the next call
      // will go back to the true case above.
      mCurrentSpan = mNextSpanOrEmpty.From(tail);
      mNextSpanOrEmpty = mNextSpanOrEmpty.First(0);
    }
    return *this;
  }

  // Number of bytes needed to represent `aValue` in unsigned LEB128.
  template <typename T>
  [[nodiscard]] static unsigned ULEB128Size(T aValue) {
    return ::mozilla::ULEB128Size(aValue);
  }

  // Write number as unsigned LEB128 and move iterator ahead.
  template <typename T>
  void WriteULEB128(T aValue) {
    ::mozilla::WriteULEB128(aValue, *this);
  }

  // Number of bytes needed to serialize objects.
  template <typename... Ts>
  [[nodiscard]] static Length SumBytes(const Ts&... aTs) {
    return (0 + ... + Serializer<Ts>::Bytes(aTs));
  }

  // Write a sequence of bytes, like memcpy.
  void WriteBytes(const void* aSrc, Length aBytes) {
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    if (MOZ_LIKELY(aBytes <= mCurrentSpan.LengthBytes())) {
      // Data fits in mCurrentSpan.
      memcpy(mCurrentSpan.Elements(), aSrc, aBytes);
      // Update mCurrentSpan. It may become empty, so in case of a double span,
      // the next call will go to the false case below.
      mCurrentSpan = mCurrentSpan.From(aBytes);
    } else {
      // Data does not fully fit in mCurrentSpan.
      // This should only happen at most once: Only for double spans, and when
      // data crosses the gap or starts there.
      // Split data between the end of mCurrentSpan and the beginning of
      // mNextSpanOrEmpty. (mCurrentSpan could be empty, it's ok to do a memcpy
      // because Span::Elements() is never null.)
      memcpy(mCurrentSpan.Elements(), aSrc, mCurrentSpan.LengthBytes());
      const Length tail =
          aBytes - static_cast<Length>(mCurrentSpan.LengthBytes());
      memcpy(mNextSpanOrEmpty.Elements(),
             reinterpret_cast<const Byte*>(aSrc) + mCurrentSpan.LengthBytes(),
             tail);
      // Move mNextSpanOrEmpty to mCurrentSpan, past the data. So the next call
      // will go back to the true case above.
      mCurrentSpan = mNextSpanOrEmpty.From(tail);
      mNextSpanOrEmpty = mNextSpanOrEmpty.First(0);
    }
  }

  void WriteFromReader(ProfileBufferEntryReader& aReader, Length aBytes) {
    MOZ_RELEASE_ASSERT(aBytes <= RemainingBytes());
    MOZ_RELEASE_ASSERT(aBytes <= aReader.RemainingBytes());
    Length read0 = std::min(
        aBytes, static_cast<Length>(aReader.mCurrentSpan.LengthBytes()));
    if (read0 != 0) {
      WriteBytes(aReader.mCurrentSpan.Elements(), read0);
    }
    Length read1 = aBytes - read0;
    if (read1 != 0) {
      WriteBytes(aReader.mNextSpanOrEmpty.Elements(), read1);
    }
    aReader += aBytes;
  }

  // Write a single object by using the appropriate Serializer.
  template <typename T>
  void WriteObject(const T& aObject) {
    Serializer<T>::Write(*this, aObject);
  }

  // Write one or more objects, sequentially.
  // Allow `EntryWrite::WriteObjects()` with nothing, this could be useful
  // for generic programming.
  template <typename... Ts>
  void WriteObjects(const Ts&... aTs) {
    (WriteObject(aTs), ...);
  }

 private:
  // The two spans covering the memory still to be written.
  SpanOfBytes mCurrentSpan;
  SpanOfBytes mNextSpanOrEmpty;
  ProfileBufferBlockIndex mCurrentBlockIndex;
  ProfileBufferBlockIndex mNextBlockIndex;
};

// ============================================================================
// Serializer and Deserializer ready-to-use specializations.

// ----------------------------------------------------------------------------
// Trivially-copyable types (default)

// The default implementation works for all trivially-copyable types (e.g.,
// PODs).
//
// Usage: `aEW.WriteObject(123);`.
//
// Raw pointers, though trivially-copyable, are explicitly forbidden when
// writing (to avoid unexpected leaks/UAFs), instead use one of
// `WrapProfileBufferLiteralCStringPointer`, `WrapProfileBufferUnownedCString`,
// or `WrapProfileBufferRawPointer` as needed.
template <typename T>
struct ProfileBufferEntryWriter::Serializer {
  static_assert(std::is_trivially_copyable_v<T>,
                "Serializer only works with trivially-copyable types by "
                "default, use/add specialization for other types.");

  static constexpr Length Bytes(const T&) { return sizeof(T); }

  static void Write(ProfileBufferEntryWriter& aEW, const T& aT) {
    static_assert(!std::is_pointer<T>::value,
                  "Serializer won't write raw pointers by default, use "
                  "WrapProfileBufferRawPointer or other.");
    aEW.WriteBytes(&aT, sizeof(T));
  }
};

// Usage: `aER.ReadObject<int>();` or `int x; aER.ReadIntoObject(x);`.
template <typename T>
struct ProfileBufferEntryReader::Deserializer {
  static_assert(std::is_trivially_copyable_v<T>,
                "Deserializer only works with trivially-copyable types by "
                "default, use/add specialization for other types.");

  static void ReadInto(ProfileBufferEntryReader& aER, T& aT) {
    aER.ReadBytes(&aT, sizeof(T));
  }

  static T Read(ProfileBufferEntryReader& aER) {
    // Note that this creates a default `T` first, and then overwrites it with
    // bytes from the buffer. Trivially-copyable types support this without UB.
    T ob;
    ReadInto(aER, ob);
    return ob;
  }
};

// ----------------------------------------------------------------------------
// Strip const/volatile/reference from types.

// Automatically strip `const`.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<const T>
    : public ProfileBufferEntryWriter::Serializer<T> {};

template <typename T>
struct ProfileBufferEntryReader::Deserializer<const T>
    : public ProfileBufferEntryReader::Deserializer<T> {};

// Automatically strip `volatile`.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<volatile T>
    : public ProfileBufferEntryWriter::Serializer<T> {};

template <typename T>
struct ProfileBufferEntryReader::Deserializer<volatile T>
    : public ProfileBufferEntryReader::Deserializer<T> {};

// Automatically strip `lvalue-reference`.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<T&>
    : public ProfileBufferEntryWriter::Serializer<T> {};

template <typename T>
struct ProfileBufferEntryReader::Deserializer<T&>
    : public ProfileBufferEntryReader::Deserializer<T> {};

// Automatically strip `rvalue-reference`.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<T&&>
    : public ProfileBufferEntryWriter::Serializer<T> {};

template <typename T>
struct ProfileBufferEntryReader::Deserializer<T&&>
    : public ProfileBufferEntryReader::Deserializer<T> {};

// ----------------------------------------------------------------------------
// ProfileBufferBlockIndex

// ProfileBufferBlockIndex, serialized as the underlying value.
template <>
struct ProfileBufferEntryWriter::Serializer<ProfileBufferBlockIndex> {
  static constexpr Length Bytes(const ProfileBufferBlockIndex& aBlockIndex) {
    return sizeof(ProfileBufferBlockIndex);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfileBufferBlockIndex& aBlockIndex) {
    aEW.WriteBytes(&aBlockIndex, sizeof(aBlockIndex));
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<ProfileBufferBlockIndex> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       ProfileBufferBlockIndex& aBlockIndex) {
    aER.ReadBytes(&aBlockIndex, sizeof(aBlockIndex));
  }

  static ProfileBufferBlockIndex Read(ProfileBufferEntryReader& aER) {
    ProfileBufferBlockIndex blockIndex;
    ReadInto(aER, blockIndex);
    return blockIndex;
  }
};

// ----------------------------------------------------------------------------
// Literal C string pointer

// Wrapper around a pointer to a literal C string.
template <size_t NonTerminalCharacters>
struct ProfileBufferLiteralCStringPointer {
  const char* mCString;
};

// Wrap a pointer to a literal C string.
template <size_t CharactersIncludingTerminal>
ProfileBufferLiteralCStringPointer<CharactersIncludingTerminal - 1>
WrapProfileBufferLiteralCStringPointer(
    const char (&aCString)[CharactersIncludingTerminal]) {
  return {aCString};
}

// Literal C strings, serialized as the raw pointer because it is unique and
// valid for the whole program lifetime.
//
// Usage: `aEW.WriteObject(WrapProfileBufferLiteralCStringPointer("hi"));`.
//
// No deserializer is provided for this type, instead it must be deserialized as
// a raw pointer: `aER.ReadObject<const char*>();`
template <size_t CharactersIncludingTerminal>
struct ProfileBufferEntryReader::Deserializer<
    ProfileBufferLiteralCStringPointer<CharactersIncludingTerminal>> {
  static constexpr Length Bytes(
      const ProfileBufferLiteralCStringPointer<CharactersIncludingTerminal>&) {
    // We're only storing a pointer, its size is independent from the pointer
    // value.
    return sizeof(const char*);
  }

  static void Write(
      ProfileBufferEntryWriter& aEW,
      const ProfileBufferLiteralCStringPointer<CharactersIncludingTerminal>&
          aWrapper) {
    // Write the pointer *value*, not the string contents.
    aEW.WriteBytes(aWrapper.mCString, sizeof(aWrapper.mCString));
  }
};

// ----------------------------------------------------------------------------
// C string contents

// Wrapper around a pointer to a C string whose contents will be serialized.
struct ProfileBufferUnownedCString {
  const char* mCString;
};

// Wrap a pointer to a C string whose contents will be serialized.
inline ProfileBufferUnownedCString WrapProfileBufferUnownedCString(
    const char* aCString) {
  return {aCString};
}

// The contents of a (probably) unowned C string are serialized as the number of
// characters (encoded as ULEB128) and all the characters in the string. The
// terminal '\0' is omitted.
//
// Usage: `aEW.WriteObject(WrapProfileBufferUnownedCString(str.c_str()))`.
//
// No deserializer is provided for this pointer type, instead it must be
// deserialized as one of the other string types that manages its contents,
// e.g.: `aER.ReadObject<std::string>();`
template <>
struct ProfileBufferEntryWriter::Serializer<ProfileBufferUnownedCString> {
  static Length Bytes(const ProfileBufferUnownedCString& aS) {
    const auto len = strlen(aS.mCString);
    return ULEB128Size(len) + len;
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfileBufferUnownedCString& aS) {
    const auto len = strlen(aS.mCString);
    aEW.WriteULEB128(len);
    aEW.WriteBytes(aS.mCString, len);
  }
};

// ----------------------------------------------------------------------------
// Raw pointers

// Wrapper around a pointer to be serialized as the raw pointer value.
template <typename T>
struct ProfileBufferRawPointer {
  T* mRawPointer;
};

// Wrap a pointer to be serialized as the raw pointer value.
template <typename T>
ProfileBufferRawPointer<T> WrapProfileBufferRawPointer(T* aRawPointer) {
  return {aRawPointer};
}

// Raw pointers are serialized as the raw pointer value.
//
// Usage: `aEW.WriteObject(WrapProfileBufferRawPointer(ptr));`
//
// The wrapper is compulsory when writing pointers (to avoid unexpected
// leaks/UAFs), but reading can be done straight into a raw pointer object,
// e.g.: `aER.ReadObject<Foo*>;`.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<ProfileBufferRawPointer<T>> {
  template <typename U>
  static constexpr Length Bytes(const U&) {
    return sizeof(T*);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfileBufferRawPointer<T>& aWrapper) {
    aEW.WriteBytes(&aWrapper.mRawPointer, sizeof(aWrapper.mRawPointer));
  }
};

// Usage: `aER.ReadObject<Foo*>;` or `Foo* p; aER.ReadIntoObject(p);`, no
// wrapper necessary.
template <typename T>
struct ProfileBufferEntryReader::Deserializer<ProfileBufferRawPointer<T>> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       ProfileBufferRawPointer<T>& aPtr) {
    aER.ReadBytes(&aPtr.mRawPointer, sizeof(aPtr));
  }

  static ProfileBufferRawPointer<T> Read(ProfileBufferEntryReader& aER) {
    ProfileBufferRawPointer<T> rawPointer;
    ReadInto(aER, rawPointer);
    return rawPointer;
  }
};

// ----------------------------------------------------------------------------
// std::string contents

// std::string contents are serialized as the number of characters (encoded as
// ULEB128) and all the characters in the string. The terminal '\0' is omitted.
//
// Usage: `std::string s = ...; aEW.WriteObject(s);`
template <typename CHAR>
struct ProfileBufferEntryWriter::Serializer<std::basic_string<CHAR>> {
  static Length Bytes(const std::basic_string<CHAR>& aS) {
    const Length len = static_cast<Length>(aS.length());
    return ULEB128Size(len) + len;
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const std::basic_string<CHAR>& aS) {
    const Length len = static_cast<Length>(aS.length());
    aEW.WriteULEB128(len);
    aEW.WriteBytes(aS.c_str(), len * sizeof(CHAR));
  }
};

// Usage: `std::string s = aEW.ReadObject<std::string>(s);` or
// `std::string s; aER.ReadIntoObject(s);`
template <typename CHAR>
struct ProfileBufferEntryReader::Deserializer<std::basic_string<CHAR>> {
  static void ReadCharsInto(ProfileBufferEntryReader& aER,
                            std::basic_string<CHAR>& aS, size_t aLength) {
    // Assign to `aS` by using iterators.
    // (`aER+0` so we get the same iterator type as `aER+len`.)
    aS.assign(aER, aER.EmptyIteratorAtOffset(aLength));
    aER += aLength;
  }

  static void ReadInto(ProfileBufferEntryReader& aER,
                       std::basic_string<CHAR>& aS) {
    ReadCharsInto(
        aER, aS,
        aER.ReadULEB128<typename std::basic_string<CHAR>::size_type>());
  }

  static std::basic_string<CHAR> ReadChars(ProfileBufferEntryReader& aER,
                                           size_t aLength) {
    // Construct a string by using iterators.
    // (`aER+0` so we get the same iterator type as `aER+len`.)
    std::basic_string<CHAR> s(aER, aER.EmptyIteratorAtOffset(aLength));
    aER += aLength;
    return s;
  }

  static std::basic_string<CHAR> Read(ProfileBufferEntryReader& aER) {
    return ReadChars(
        aER, aER.ReadULEB128<typename std::basic_string<CHAR>::size_type>());
  }
};

// ----------------------------------------------------------------------------
// mozilla::UniqueFreePtr<CHAR>

// UniqueFreePtr<CHAR>, which points at a string allocated with `malloc`
// (typically generated by `strdup()`), is serialized as the number of
// *bytes* (encoded as ULEB128) and all the characters in the string. The
// null terminator is omitted.
// `CHAR` can be any type that has a specialization for
// `std::char_traits<CHAR>::length(const CHAR*)`.
//
// Note: A nullptr pointer will be serialized like an empty string, so when
// deserializing it will result in an allocated buffer only containing a
// single null terminator.
template <typename CHAR>
struct ProfileBufferEntryWriter::Serializer<UniqueFreePtr<CHAR>> {
  static Length Bytes(const UniqueFreePtr<CHAR>& aS) {
    if (!aS) {
      // Null pointer, store it as if it was an empty string (so: 0 bytes).
      return ULEB128Size(0u);
    }
    // Note that we store the size in *bytes*, not in number of characters.
    const auto bytes = std::char_traits<CHAR>::length(aS.get()) * sizeof(CHAR);
    return ULEB128Size(bytes) + bytes;
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const UniqueFreePtr<CHAR>& aS) {
    if (!aS) {
      // Null pointer, store it as if it was an empty string (so we write a
      // length of 0 bytes).
      aEW.WriteULEB128(0u);
      return;
    }
    // Note that we store the size in *bytes*, not in number of characters.
    const auto bytes = std::char_traits<CHAR>::length(aS.get()) * sizeof(CHAR);
    aEW.WriteULEB128(bytes);
    aEW.WriteBytes(aS.get(), bytes);
  }
};

template <typename CHAR>
struct ProfileBufferEntryReader::Deserializer<UniqueFreePtr<CHAR>> {
  static void ReadInto(ProfileBufferEntryReader& aER, UniqueFreePtr<CHAR>& aS) {
    aS = Read(aER);
  }

  static UniqueFreePtr<CHAR> Read(ProfileBufferEntryReader& aER) {
    // Read the number of *bytes* that follow.
    const auto bytes = aER.ReadULEB128<size_t>();
    // We need a buffer of the non-const character type.
    using NC_CHAR = std::remove_const_t<CHAR>;
    // We allocate the required number of bytes, plus one extra character for
    // the null terminator.
    NC_CHAR* buffer = static_cast<NC_CHAR*>(malloc(bytes + sizeof(NC_CHAR)));
    // Copy the characters into the buffer.
    aER.ReadBytes(buffer, bytes);
    // And append a null terminator.
    buffer[bytes / sizeof(NC_CHAR)] = NC_CHAR(0);
    return UniqueFreePtr<CHAR>(buffer);
  }
};

// ----------------------------------------------------------------------------
// std::tuple

// std::tuple is serialized as a sequence of each recursively-serialized item.
//
// This is equivalent to manually serializing each item, so reading/writing
// tuples is equivalent to reading/writing their elements in order, e.g.:
// ```
// std::tuple<int, std::string> is = ...;
// aEW.WriteObject(is); // Write the tuple, equivalent to:
// aEW.WriteObject(/* int */ std::get<0>(is), /* string */ std::get<1>(is));
// ...
// // Reading back can be done directly into a tuple:
// auto is = aER.ReadObject<std::tuple<int, std::string>>();
// // Or each item could be read separately:
// auto i = aER.ReadObject<int>(); auto s = aER.ReadObject<std::string>();
// ```
template <typename... Ts>
struct ProfileBufferEntryWriter::Serializer<std::tuple<Ts...>> {
 private:
  template <size_t... Is>
  static Length TupleBytes(const std::tuple<Ts...>& aTuple,
                           std::index_sequence<Is...>) {
    return (0 + ... + SumBytes(std::get<Is>(aTuple)));
  }

  template <size_t... Is>
  static void TupleWrite(ProfileBufferEntryWriter& aEW,
                         const std::tuple<Ts...>& aTuple,
                         std::index_sequence<Is...>) {
    (aEW.WriteObject(std::get<Is>(aTuple)), ...);
  }

 public:
  static Length Bytes(const std::tuple<Ts...>& aTuple) {
    // Generate a 0..N-1 index pack, we'll add the sizes of each item.
    return TupleBytes(aTuple, std::index_sequence_for<Ts...>());
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const std::tuple<Ts...>& aTuple) {
    // Generate a 0..N-1 index pack, we'll write each item.
    TupleWrite(aEW, aTuple, std::index_sequence_for<Ts...>());
  }
};

template <typename... Ts>
struct ProfileBufferEntryReader::Deserializer<std::tuple<Ts...>> {
  template <size_t I>
  static void TupleIReadInto(ProfileBufferEntryReader& aER,
                             std::tuple<Ts...>& aTuple) {
    aER.ReadIntoObject(std::get<I>(aTuple));
  }

  template <size_t... Is>
  static void TupleReadInto(ProfileBufferEntryReader& aER,
                            std::tuple<Ts...>& aTuple,
                            std::index_sequence<Is...>) {
    (TupleIReadInto<Is>(aER, aTuple), ...);
  }

  static void ReadInto(ProfileBufferEntryReader& aER,
                       std::tuple<Ts...>& aTuple) {
    TupleReadInto(aER, aTuple, std::index_sequence_for<Ts...>());
  }

  static std::tuple<Ts...> Read(ProfileBufferEntryReader& aER) {
    // Note that this creates default `Ts` first, and then overwrites them.
    std::tuple<Ts...> ob;
    ReadInto(aER, ob);
    return ob;
  }
};
// ----------------------------------------------------------------------------
// mozilla::Span

// Span. All elements are serialized in sequence.
// The caller is assumed to know the number of elements (they may manually
// write&read it before the span if needed).
// Similar to tuples, reading/writing spans is equivalent to reading/writing
// their elements in order.
template <class T, size_t N>
struct ProfileBufferEntryWriter::Serializer<Span<T, N>> {
  static Length Bytes(const Span<T, N>& aSpan) {
    Length bytes = 0;
    for (const T& element : aSpan) {
      bytes += SumBytes(element);
    }
    return bytes;
  }

  static void Write(ProfileBufferEntryWriter& aEW, const Span<T, N>& aSpan) {
    for (const T& element : aSpan) {
      aEW.WriteObject(element);
    }
  }
};

template <class T, size_t N>
struct ProfileBufferEntryReader::Deserializer<Span<T, N>> {
  // Read elements back into span pointing at a pre-allocated buffer.
  static void ReadInto(ProfileBufferEntryReader& aER, Span<T, N>& aSpan) {
    for (T& element : aSpan) {
      aER.ReadIntoObject(element);
    }
  }

  // A Span does not own its data, this would probably leak so we forbid this.
  static Span<T, N> Read(ProfileBufferEntryReader& aER) = delete;
};

// ----------------------------------------------------------------------------
// mozilla::Maybe

// Maybe<T> is serialized as one byte containing either 'm' (Nothing),
// or 'M' followed by the recursively-serialized `T` object.
template <typename T>
struct ProfileBufferEntryWriter::Serializer<Maybe<T>> {
  static Length Bytes(const Maybe<T>& aMaybe) {
    // 1 byte to store nothing/something flag, then object size if present.
    return aMaybe.isNothing() ? 1 : (1 + SumBytes(aMaybe.ref()));
  }

  static void Write(ProfileBufferEntryWriter& aEW, const Maybe<T>& aMaybe) {
    // 'm'/'M' is just an arbitrary 1-byte value to distinguish states.
    if (aMaybe.isNothing()) {
      aEW.WriteObject<char>('m');
    } else {
      aEW.WriteObject<char>('M');
      // Use the Serializer for the contained type.
      aEW.WriteObject(aMaybe.ref());
    }
  }
};

template <typename T>
struct ProfileBufferEntryReader::Deserializer<Maybe<T>> {
  static void ReadInto(ProfileBufferEntryReader& aER, Maybe<T>& aMaybe) {
    char c = aER.ReadObject<char>();
    if (c == 'm') {
      aMaybe.reset();
    } else {
      MOZ_ASSERT(c == 'M');
      // If aMaybe is empty, create a default `T` first, to be overwritten.
      // Otherwise we'll just overwrite whatever was already there.
      if (aMaybe.isNothing()) {
        aMaybe.emplace();
      }
      // Use the Deserializer for the contained type.
      aER.ReadIntoObject(aMaybe.ref());
    }
  }

  static Maybe<T> Read(ProfileBufferEntryReader& aER) {
    Maybe<T> maybe;
    char c = aER.ReadObject<char>();
    MOZ_ASSERT(c == 'M' || c == 'm');
    if (c == 'M') {
      // Note that this creates a default `T` inside the Maybe first, and then
      // overwrites it.
      maybe = Some(T{});
      // Use the Deserializer for the contained type.
      aER.ReadIntoObject(maybe.ref());
    }
    return maybe;
  }
};

// ----------------------------------------------------------------------------
// mozilla::Variant

// Variant is serialized as the tag (0-based index of the stored type, encoded
// as ULEB128), and the recursively-serialized object.
template <typename... Ts>
struct ProfileBufferEntryWriter::Serializer<Variant<Ts...>> {
 public:
  static Length Bytes(const Variant<Ts...>& aVariantTs) {
    return aVariantTs.match([](auto aIndex, const auto& aAlternative) {
      return ULEB128Size(aIndex) + SumBytes(aAlternative);
    });
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const Variant<Ts...>& aVariantTs) {
    aVariantTs.match([&aEW](auto aIndex, const auto& aAlternative) {
      aEW.WriteULEB128(aIndex);
      aEW.WriteObject(aAlternative);
    });
  }
};

template <typename... Ts>
struct ProfileBufferEntryReader::Deserializer<Variant<Ts...>> {
 private:
  // Called from the fold expression in `VariantReadInto()`, only the selected
  // variant will deserialize the object.
  template <size_t I>
  static void VariantIReadInto(ProfileBufferEntryReader& aER,
                               Variant<Ts...>& aVariantTs, unsigned aTag) {
    if (I == aTag) {
      // Ensure the variant contains the target type. Note that this may create
      // a default object.
      if (!aVariantTs.template is<I>()) {
        aVariantTs = Variant<Ts...>(VariantIndex<I>{});
      }
      aER.ReadIntoObject(aVariantTs.template as<I>());
    }
  }

  template <size_t... Is>
  static void VariantReadInto(ProfileBufferEntryReader& aER,
                              Variant<Ts...>& aVariantTs,
                              std::index_sequence<Is...>) {
    unsigned tag = aER.ReadULEB128<unsigned>();
    (VariantIReadInto<Is>(aER, aVariantTs, tag), ...);
  }

 public:
  static void ReadInto(ProfileBufferEntryReader& aER,
                       Variant<Ts...>& aVariantTs) {
    // Generate a 0..N-1 index pack, the selected variant will deserialize
    // itself.
    VariantReadInto(aER, aVariantTs, std::index_sequence_for<Ts...>());
  }

  static Variant<Ts...> Read(ProfileBufferEntryReader& aER) {
    // Note that this creates a default `Variant` of the first type, and then
    // overwrites it. Consider using `ReadInto` for more control if needed.
    Variant<Ts...> variant(VariantIndex<0>{});
    ReadInto(aER, variant);
    return variant;
  }
};

}  // namespace mozilla

#endif  // ProfileBufferEntrySerialization_h
