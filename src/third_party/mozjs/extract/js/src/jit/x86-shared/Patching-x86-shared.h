/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Patching_x86_shared_h
#define jit_x86_shared_Patching_x86_shared_h

namespace js {
namespace jit {

namespace X86Encoding {

inline void* GetPointer(const void* where) {
  void* res;
  memcpy(&res, (const char*)where - sizeof(void*), sizeof(void*));
  return res;
}

inline void SetPointer(void* where, const void* value) {
  memcpy((char*)where - sizeof(void*), &value, sizeof(void*));
}

inline int32_t GetInt32(const void* where) {
  int32_t res;
  memcpy(&res, (const char*)where - sizeof(int32_t), sizeof(int32_t));
  return res;
}

inline void SetInt32(void* where, int32_t value, uint32_t trailing = 0) {
  memcpy((char*)where - trailing - sizeof(int32_t), &value, sizeof(int32_t));
}

inline void SetRel32(void* from, void* to, uint32_t trailing = 0) {
  intptr_t offset =
      reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
  MOZ_ASSERT(offset == static_cast<int32_t>(offset),
             "offset is too great for a 32-bit relocation");
  if (offset != static_cast<int32_t>(offset)) {
    MOZ_CRASH("offset is too great for a 32-bit relocation");
  }

  SetInt32(from, offset, trailing);
}

inline void* GetRel32Target(void* where) {
  int32_t rel = GetInt32(where);
  return (char*)where + rel;
}

// JmpSrc represents a positive offset within a code buffer, or an uninitialized
// value.  Lots of code depends on uninitialized JmpSrc holding the value -1, on
// -1 being a legal value of JmpSrc, and on being able to initialize a JmpSrc
// with the value -1.
//
// The value of the `offset` is always positive and <= MaxCodeBytesPerProcess,
// see ProcessExecutableMemory.h.  The latter quantity in turn must fit in an
// i32.  But we further require that the value is not precisely INT32_MAX, so as
// to allow the JmpSrc value -1 to mean "uninitialized" without ambiguity.
//
// The quantity `trailing` denotes the number of bytes of data that follow the
// patch field in the instruction.  The offset points to the end of the
// instruction as per normal.  The information about trailing bytes is needed
// separately from the offset to correctly patch instructions that have
// immediates trailing the patch field (eg CMPSS and CMPSD).  Currently the only
// allowed values for `trailing` are 0 and 1.

static_assert(MaxCodeBytesPerProcess < size_t(INT32_MAX), "Invariant");

class JmpSrc {
 public:
  JmpSrc() : offset_(INT32_MAX), trailing_(0) {}
  explicit JmpSrc(int32_t offset) : offset_(offset), trailing_(0) {
    // offset -1 is stored as INT32_MAX
    MOZ_ASSERT(offset == -1 || (offset >= 0 && offset < INT32_MAX));
  }
  JmpSrc(int32_t offset, uint32_t trailing)
      : offset_(offset), trailing_(trailing) {
    // Disallow offset -1 in this situation, it does not apply.
    MOZ_ASSERT(offset >= 0 && offset < INT32_MAX);
    MOZ_ASSERT(trailing <= 1);
  }
  int32_t offset() const {
    return offset_ == INT32_MAX ? -1 : int32_t(offset_);
  }
  uint32_t trailing() const { return trailing_; }

 private:
  uint32_t offset_ : 31;
  uint32_t trailing_ : 1;
};

class JmpDst {
 public:
  explicit JmpDst(int32_t offset) : offset_(offset) {}
  int32_t offset() const { return offset_; }

 private:
  int32_t offset_;
};

inline bool CanRelinkJump(void* from, void* to) {
  intptr_t offset = static_cast<char*>(to) - static_cast<char*>(from);
  return (offset == static_cast<int32_t>(offset));
}

}  // namespace X86Encoding

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_Patching_x86_shared_h */
