/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsUnwindInfo_h
#define mozilla_WindowsUnwindInfo_h

#ifdef _M_X64

#  include <cstdint>

#  include "mozilla/Assertions.h"
#  include "mozilla/UniquePtr.h"

namespace mozilla {

// On Windows x64, there is no standard function prologue, hence extra
// information that describes the prologue must be added for each non-leaf
// function in order to properly unwind the stack. This extra information is
// grouped into so-called function tables.
//
// A function table is a contiguous array of one or more RUNTIME_FUNCTION
// entries. Each RUNTIME_FUNCTION entry associates a start and end offset in
// code with specific unwind information. The function table is present in the
// .pdata section of binaries for static code, and added dynamically with
// RtlAddFunctionTable or RtlInstallFunctionTableCallback for dynamic code.
// RUNTIME_FUNCTION entries point to the unwind information, which can thus
// live at a different location in memory, for example it lives in the .xdata
// section for static code.
//
// Contrary to RUNTIME_FUNCTION, Microsoft provides no standard structure
// definition to map the unwind information. This file thus provides some
// helpers to read this data, originally based on breakpad code. The unwind
// information is partially documented at:
// https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64.

// The unwind information stores a bytecode in UnwindInfo.unwind_code[] that
// describes how the instructions in the function prologue interact with the
// stack. An instruction in this bytecode is called an unwind code.
// UnwindCodeOperationCodes enumerates all opcodes used by this bytecode.
// Unwind codes are stored in contiguous slots of 16 bits, where each unwind
// code can span either 1, 2, or 3 slots depending on the opcode it uses.
enum UnwindOperationCodes : uint8_t {
  // UnwindCode.operation_info == register number
  UWOP_PUSH_NONVOL = 0,
  // UnwindCode.operation_info == 0 or 1,
  // alloc size in next slot (if 0) or next 2 slots (if 1)
  UWOP_ALLOC_LARGE = 1,
  // UnwindCode.operation_info == size of allocation / 8 - 1
  UWOP_ALLOC_SMALL = 2,
  // no UnwindCode.operation_info; register number UnwindInfo.frame_register
  // receives (rsp + UnwindInfo.frame_offset*16)
  UWOP_SET_FPREG = 3,
  // UnwindCode.operation_info == register number, offset in next slot
  UWOP_SAVE_NONVOL = 4,
  // UnwindCode.operation_info == register number, offset in next 2 slots
  UWOP_SAVE_NONVOL_FAR = 5,
  // Version 1; undocumented; not meant for x64
  UWOP_SAVE_XMM = 6,
  // Version 2; undocumented
  UWOP_EPILOG = 6,
  // Version 1; undocumented; not meant for x64
  UWOP_SAVE_XMM_FAR = 7,
  // Version 2; undocumented
  UWOP_SPARE = 7,
  // UnwindCode.operation_info == XMM reg number, offset in next slot
  UWOP_SAVE_XMM128 = 8,
  // UnwindCode.operation_info == XMM reg number, offset in next 2 slots
  UWOP_SAVE_XMM128_FAR = 9,
  // UnwindCode.operation_info == 0: no error-code, 1: error-code
  UWOP_PUSH_MACHFRAME = 10
};

// Strictly speaking, UnwindCode represents a slot -- not a full unwind code.
union UnwindCode {
  struct {
    uint8_t offset_in_prolog;
    UnwindOperationCodes unwind_operation_code : 4;
    uint8_t operation_info : 4;
  };
  uint16_t frame_offset;
};

// UnwindInfo is a variable-sized struct meant for C-style direct access to the
// unwind information. Be careful:
//  - prefer using the size() helper method to using sizeof;
//  - don't construct objects of this type, cast pointers instead;
//  - consider using the IterableUnwindInfo helpers to iterate over unwind
//    codes.
struct UnwindInfo {
  uint8_t version : 3;
  uint8_t flags : 5;  // either 0, UNW_FLAG_CHAININFO, or a combination of
                      // UNW_FLAG_EHANDLER and UNW_FLAG_UHANDLER
  uint8_t size_of_prolog;
  uint8_t count_of_codes;  // contains the length of the unwind_code[] array
  uint8_t frame_register : 4;
  uint8_t frame_offset : 4;
  UnwindCode unwind_code[1];  // variable length
  // Note: There is extra data after the variable length array if using flags
  //       UNW_FLAG_EHANDLER, UNW_FLAG_UHANDLER, or UNW_FLAG_CHAININFO. We
  //       ignore the extra data at the moment. For more details, see:
  //       https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64.
  //
  //       When using UNW_FLAG_EHANDLER or UNW_FLAG_UHANDLER, the extra data
  //       includes handler data of unspecificied size: only the handler knows
  //       the correct size for this data. This makes it difficult to know the
  //       size of the full unwind information or to copy it in this particular
  //       case.

  UnwindInfo(const UnwindInfo&) = delete;
  UnwindInfo& operator=(const UnwindInfo&) = delete;
  UnwindInfo(UnwindInfo&&) = delete;
  UnwindInfo& operator=(UnwindInfo&&) = delete;
  ~UnwindInfo() = delete;

  // Size of this structure, including the variable length unwind_code array
  // but NOT including the extra data related to flags UNW_FLAG_EHANDLER,
  // UNW_FLAG_UHANDLER, and UNW_FLAG_CHAININFO.
  //
  // The places where we currently use these helpers read unwind information at
  // function entry points; as such we expect that they may encounter
  // UNW_FLAG_EHANDLER and/or UNW_FLAG_UHANDLER but won't need to use the
  // associated extra data, and it is expected that they should not encounter
  // UNW_FLAG_CHAININFO. UNW_FLAG_CHAININFO is typically used for code that
  // lives separately from the entry point of the function to which it belongs,
  // this code then has chained unwind info pointing to the entry point.
  inline size_t Size() const {
    return offsetof(UnwindInfo, unwind_code) +
           count_of_codes * sizeof(UnwindCode);
  }

  // Note: We currently do not copy the extra data related to flags
  //       UNW_FLAG_EHANDLER, UNW_FLAG_UHANDLER, and UNW_FLAG_CHAININFO.
  UniquePtr<uint8_t[]> Copy() const {
    auto s = Size();
    auto result = MakeUnique<uint8_t[]>(s);
    std::memcpy(result.get(), reinterpret_cast<const void*>(this), s);
    return result;
  }

  // An unwind code spans a number of slots in the unwind_code array that can
  // vary from 1 to 3. This method assumes that the index parameter points to
  // a slot that is the start of an unwind code. If the unwind code is
  // well-formed, it returns true and sets its second parameter to the number
  // of slots that the unwind code occupies.
  //
  // This function returns false if the unwind code is ill-formed, i.e.:
  //  - either the index points out of bounds;
  //  - or the opcode is invalid, or unexpected (e.g. UWOP_SAVE_XMM and
  //    UWOP_SAVE_XMM_FAR in version 1);
  //  - or using the correct slots count for the opcode would go out of bounds.
  bool GetSlotsCountForCodeAt(uint8_t aIndex, uint8_t* aSlotsCount) const {
    if (aIndex >= count_of_codes) {
      MOZ_ASSERT_UNREACHABLE("The index is out of bounds");
      return false;
    }

    const UnwindCode& unwindCode = unwind_code[aIndex];
    uint8_t slotsCount = 0;

    // See https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64
    switch (unwindCode.unwind_operation_code) {
      // Start with fixed-size opcodes common to versions 1 and 2
      case UWOP_SAVE_NONVOL_FAR:
      case UWOP_SAVE_XMM128_FAR:
        slotsCount = 3;
        break;

      case UWOP_SAVE_NONVOL:
      case UWOP_SAVE_XMM128:
        slotsCount = 2;
        break;

      case UWOP_PUSH_NONVOL:
      case UWOP_ALLOC_SMALL:
      case UWOP_SET_FPREG:
      case UWOP_PUSH_MACHFRAME:
        slotsCount = 1;
        break;

      // UWOP_ALLOC_LARGE is the only variable-sized opcode. It is common to
      // versions 1 and 2. It is ill-formed if the info is not 0 or 1.
      case UWOP_ALLOC_LARGE:
        if (unwindCode.operation_info > 1) {
          MOZ_ASSERT_UNREACHABLE(
              "Operation UWOP_ALLOC_LARGE is used, but operation_info "
              "is not 0 or 1");
          return false;
        }
        slotsCount = 2 + unwindCode.operation_info;
        break;

      case UWOP_SPARE:
        if (version != 2) {
          MOZ_ASSERT_UNREACHABLE(
              "Operation code UWOP_SPARE is used, but version is not 2");
          return false;
        }
        slotsCount = 3;
        break;

      case UWOP_EPILOG:
        if (version != 2) {
          MOZ_ASSERT_UNREACHABLE(
              "Operation code UWOP_EPILOG is used, but version is not 2");
          return false;
        }
        slotsCount = 2;
        break;

      default:
        MOZ_ASSERT_UNREACHABLE("An unknown operation code is used");
        return false;
    }

    // The unwind code is ill-formed if using the correct number of slots for
    // the opcode would go out of bounds.
    if (count_of_codes - aIndex < slotsCount) {
      MOZ_ASSERT_UNREACHABLE(
          "A valid operation code is used, but it spans too many slots");
      return false;
    }

    *aSlotsCount = slotsCount;
    return true;
  }
};

class IterableUnwindInfo {
  class Iterator {
   public:
    UnwindInfo& Info() { return mInfo; }

    uint8_t Index() const {
      MOZ_ASSERT(IsValid());
      return mIndex;
    }

    uint8_t SlotsCount() const {
      MOZ_ASSERT(IsValid());
      return mSlotsCount;
    }

    // An iterator is valid if it points to a well-formed unwind code.
    // The end iterator is invalid as it does not point to any unwind code.
    // All invalid iterators compare equal, which allows comparison with the
    // end iterator to exit loops as soon as an ill-formed unwind code is met.
    bool IsValid() const { return mIsValid; }

    bool IsAtEnd() const { return mIndex >= mInfo.count_of_codes; }

    bool operator==(const Iterator& aOther) const {
      if (mIsValid != aOther.mIsValid) {
        return false;
      }
      // Comparing two invalid iterators.
      if (!mIsValid) {
        return true;
      }
      // Comparing two valid iterators.
      return mIndex == aOther.mIndex;
    }

    bool operator!=(const Iterator& aOther) const { return !(*this == aOther); }

    Iterator& operator++() {
      MOZ_ASSERT(IsValid());
      mIndex += mSlotsCount;
      if (mIndex < mInfo.count_of_codes) {
        mIsValid = mInfo.GetSlotsCountForCodeAt(mIndex, &mSlotsCount);
        MOZ_ASSERT(IsValid());
      } else {
        mIsValid = false;
      }
      return *this;
    }

    const UnwindCode& operator*() {
      MOZ_ASSERT(IsValid());
      return mInfo.unwind_code[mIndex];
    }

   private:
    friend class IterableUnwindInfo;

    Iterator(UnwindInfo& aInfo, uint8_t aIndex, uint8_t aSlotsCount,
             bool aIsValid)
        : mInfo(aInfo),
          mIndex(aIndex),
          mSlotsCount(aSlotsCount),
          mIsValid(aIsValid) {};

    UnwindInfo& mInfo;
    uint8_t mIndex;
    uint8_t mSlotsCount;
    bool mIsValid;
  };

 public:
  explicit IterableUnwindInfo(UnwindInfo& aInfo)
      : mBegin(aInfo, 0, 0, false),
        mEnd(aInfo, aInfo.count_of_codes, 0, false) {
    if (aInfo.count_of_codes) {
      mBegin.mIsValid = aInfo.GetSlotsCountForCodeAt(0, &mBegin.mSlotsCount);
      MOZ_ASSERT(mBegin.mIsValid);
    }
  }

  explicit IterableUnwindInfo(uint8_t* aInfo)
      : IterableUnwindInfo(*reinterpret_cast<UnwindInfo*>(aInfo)) {}

  UnwindInfo& Info() { return mBegin.Info(); }

  const Iterator& begin() { return mBegin; }

  const Iterator& end() { return mEnd; }

 private:
  Iterator mBegin;
  Iterator mEnd;
};

}  // namespace mozilla

#endif  // _M_X64

#endif  // mozilla_WindowsUnwindInfo_h
