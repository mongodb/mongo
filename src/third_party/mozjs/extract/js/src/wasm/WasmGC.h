/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2019 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_gc_h
#define wasm_gc_h

#include "mozilla/BinarySearch.h"

#include "jit/ABIArgGenerator.h"  // For ABIArgIter
#include "js/AllocPolicy.h"
#include "js/Vector.h"
#include "util/Memory.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmFrame.h"
#include "wasm/WasmSerialize.h"

namespace js {

namespace jit {
class Label;
class MacroAssembler;
}  // namespace jit

namespace wasm {

class ArgTypeVector;
class BytecodeOffset;

using jit::Label;
using jit::MIRType;
using jit::Register;

// Definitions for stackmaps.

using ExitStubMapVector = Vector<bool, 32, SystemAllocPolicy>;

struct StackMapHeader {
  explicit StackMapHeader(uint32_t numMappedWords = 0)
      : numMappedWords(numMappedWords),
        numExitStubWords(0),
        frameOffsetFromTop(0),
        hasDebugFrameWithLiveRefs(0) {}

  // The total number of stack words covered by the map ..
  static constexpr size_t MappedWordsBits = 30;
  uint32_t numMappedWords : MappedWordsBits;

  // .. of which this many are "exit stub" extras
  static constexpr size_t ExitStubWordsBits = 6;
  uint32_t numExitStubWords : ExitStubWordsBits;

  // Where is Frame* relative to the top?  This is an offset in words.  On every
  // platform, FrameOffsetBits needs to be at least
  // ceil(log2(MaxParams*sizeof-biggest-param-type-in-words)).  The most
  // constraining platforms are 32-bit with SIMD support, currently x86-32.
  static constexpr size_t FrameOffsetBits = 12;
  uint32_t frameOffsetFromTop : FrameOffsetBits;

  // Notes the presence of a DebugFrame with possibly-live references.  A
  // DebugFrame may or may not contain GC-managed data; in situations when it is
  // possible that any pointers in the DebugFrame are non-null, the DebugFrame
  // gets a stackmap.
  uint32_t hasDebugFrameWithLiveRefs : 1;

  WASM_CHECK_CACHEABLE_POD(numMappedWords, numExitStubWords, frameOffsetFromTop,
                           hasDebugFrameWithLiveRefs);

  static constexpr uint32_t maxMappedWords = (1 << MappedWordsBits) - 1;
  static constexpr uint32_t maxExitStubWords = (1 << ExitStubWordsBits) - 1;
  static constexpr uint32_t maxFrameOffsetFromTop = (1 << FrameOffsetBits) - 1;

  static constexpr size_t MaxParamSize =
      std::max(sizeof(jit::FloatRegisters::RegisterContent),
               sizeof(jit::Registers::RegisterContent));

  // Add 16 words to account for the size of FrameWithInstances including any
  // shadow stack (at worst 8 words total), and then a little headroom in case
  // the argument area had to be aligned.
  static_assert(FrameWithInstances::sizeOf() / sizeof(void*) <= 8);
  static_assert(maxFrameOffsetFromTop >=
                    (MaxParams * MaxParamSize / sizeof(void*)) + 16,
                "limited size of the offset field");
};

WASM_DECLARE_CACHEABLE_POD(StackMapHeader);

// This is the expected size for the header
static_assert(sizeof(StackMapHeader) == 8,
              "wasm::StackMapHeader has unexpected size");

// A StackMap is a bit-array containing numMappedWords bits, one bit per
// word of stack.  Bit index zero is for the lowest addressed word in the
// range.
//
// This is a variable-length structure whose size must be known at creation
// time.
//
// Users of the map will know the address of the wasm::Frame that is covered
// by this map.  In order that they can calculate the exact address range
// covered by the map, the map also stores the offset, from the highest
// addressed word of the map, of the embedded wasm::Frame.  This is an offset
// down from the highest address, rather than up from the lowest, so as to
// limit its range to FrameOffsetBits bits.
//
// The stackmap may also cover a DebugFrame (all DebugFrames which may
// potentially contain live pointers into the JS heap get a map).  If so that
// can be noted, since users of the map need to trace pointers in a
// DebugFrame.
//
// Finally, for sanity checking only, for stackmaps associated with a wasm
// trap exit stub, the number of words used by the trap exit stub save area
// is also noted.  This is used in Instance::traceFrame to check that the
// TrapExitDummyValue is in the expected place in the frame.
struct StackMap final {
  // The header contains the constant-sized fields before the variable-sized
  // bitmap that follows.
  StackMapHeader header;

 private:
  // The variable-sized bitmap.
  uint32_t bitmap[1];

  explicit StackMap(uint32_t numMappedWords) : header(numMappedWords) {
    const uint32_t nBitmap = calcNBitmap(header.numMappedWords);
    memset(bitmap, 0, nBitmap * sizeof(bitmap[0]));
  }
  explicit StackMap(const StackMapHeader& header) : header(header) {
    const uint32_t nBitmap = calcNBitmap(header.numMappedWords);
    memset(bitmap, 0, nBitmap * sizeof(bitmap[0]));
  }

 public:
  static StackMap* create(uint32_t numMappedWords) {
    size_t size = allocationSizeInBytes(numMappedWords);
    char* buf = (char*)js_malloc(size);
    if (!buf) {
      return nullptr;
    }
    return ::new (buf) StackMap(numMappedWords);
  }
  static StackMap* create(const StackMapHeader& header) {
    size_t size = allocationSizeInBytes(header.numMappedWords);
    char* buf = (char*)js_malloc(size);
    if (!buf) {
      return nullptr;
    }
    return ::new (buf) StackMap(header);
  }

  void destroy() { js_free((char*)this); }

  // Returns the size of a `StackMap` allocated with `numMappedWords`.
  static size_t allocationSizeInBytes(uint32_t numMappedWords) {
    uint32_t nBitmap = calcNBitmap(numMappedWords);
    return sizeof(StackMap) + (nBitmap - 1) * sizeof(bitmap[0]);
  }

  // Returns the allocated size of this `StackMap`.
  size_t allocationSizeInBytes() const {
    return allocationSizeInBytes(header.numMappedWords);
  }

  // Record the number of words in the map used as a wasm trap exit stub
  // save area.  See comment above.
  void setExitStubWords(uint32_t nWords) {
    MOZ_ASSERT(header.numExitStubWords == 0);
    MOZ_RELEASE_ASSERT(nWords <= header.maxExitStubWords);
    MOZ_ASSERT(nWords <= header.numMappedWords);
    header.numExitStubWords = nWords;
  }

  // Record the offset from the highest-addressed word of the map, that the
  // wasm::Frame lives at.  See comment above.
  void setFrameOffsetFromTop(uint32_t nWords) {
    MOZ_ASSERT(header.frameOffsetFromTop == 0);
    MOZ_RELEASE_ASSERT(nWords <= StackMapHeader::maxFrameOffsetFromTop);
    MOZ_ASSERT(header.frameOffsetFromTop < header.numMappedWords);
    header.frameOffsetFromTop = nWords;
  }

  // If the frame described by this StackMap includes a DebugFrame, call here to
  // record that fact.
  void setHasDebugFrameWithLiveRefs() {
    MOZ_ASSERT(header.hasDebugFrameWithLiveRefs == 0);
    header.hasDebugFrameWithLiveRefs = 1;
  }

  inline void setBit(uint32_t bitIndex) {
    MOZ_ASSERT(bitIndex < header.numMappedWords);
    uint32_t wordIndex = bitIndex / wordsPerBitmapElem;
    uint32_t wordOffset = bitIndex % wordsPerBitmapElem;
    bitmap[wordIndex] |= (1 << wordOffset);
  }

  inline uint32_t getBit(uint32_t bitIndex) const {
    MOZ_ASSERT(bitIndex < header.numMappedWords);
    uint32_t wordIndex = bitIndex / wordsPerBitmapElem;
    uint32_t wordOffset = bitIndex % wordsPerBitmapElem;
    return (bitmap[wordIndex] >> wordOffset) & 1;
  }

  inline uint8_t* rawBitmap() { return (uint8_t*)&bitmap; }
  inline const uint8_t* rawBitmap() const { return (const uint8_t*)&bitmap; }
  inline size_t rawBitmapLengthInBytes() const {
    return calcNBitmap(header.numMappedWords) * sizeof(uint32_t);
  }

 private:
  static constexpr uint32_t wordsPerBitmapElem = sizeof(bitmap[0]) * 8;

  static uint32_t calcNBitmap(uint32_t numMappedWords) {
    MOZ_RELEASE_ASSERT(numMappedWords <= StackMapHeader::maxMappedWords);
    uint32_t nBitmap =
        (numMappedWords + wordsPerBitmapElem - 1) / wordsPerBitmapElem;
    return nBitmap == 0 ? 1 : nBitmap;
  }
};

// This is the expected size for a map that covers 32 or fewer words.
static_assert(sizeof(StackMap) == 12, "wasm::StackMap has unexpected size");

class StackMaps {
 public:
  // A Maplet holds a single code-address-to-map binding.  Note that the
  // code address is the lowest address of the instruction immediately
  // following the instruction of interest, not of the instruction of
  // interest itself.  In practice (at least for the Wasm Baseline compiler)
  // this means that |nextInsnAddr| points either immediately after a call
  // instruction, after a trap instruction or after a no-op.
  struct Maplet {
    const uint8_t* nextInsnAddr;
    StackMap* map;
    Maplet(const uint8_t* nextInsnAddr, StackMap* map)
        : nextInsnAddr(nextInsnAddr), map(map) {}
    void offsetBy(uintptr_t delta) { nextInsnAddr += delta; }
    bool operator<(const Maplet& other) const {
      return uintptr_t(nextInsnAddr) < uintptr_t(other.nextInsnAddr);
    }
  };

 private:
  bool sorted_;
  Vector<Maplet, 0, SystemAllocPolicy> mapping_;

 public:
  StackMaps() : sorted_(false) {}
  ~StackMaps() {
    for (auto& maplet : mapping_) {
      maplet.map->destroy();
      maplet.map = nullptr;
    }
  }
  [[nodiscard]] bool add(const uint8_t* nextInsnAddr, StackMap* map) {
    MOZ_ASSERT(!sorted_);
    return mapping_.append(Maplet(nextInsnAddr, map));
  }
  [[nodiscard]] bool add(const Maplet& maplet) {
    return add(maplet.nextInsnAddr, maplet.map);
  }
  void clear() {
    for (auto& maplet : mapping_) {
      maplet.nextInsnAddr = nullptr;
      maplet.map = nullptr;
    }
    mapping_.clear();
  }
  bool empty() const { return mapping_.empty(); }
  size_t length() const { return mapping_.length(); }
  Maplet* getRef(size_t i) { return &mapping_[i]; }
  Maplet get(size_t i) const { return mapping_[i]; }
  Maplet move(size_t i) {
    Maplet m = mapping_[i];
    mapping_[i].map = nullptr;
    return m;
  }
  void offsetBy(uintptr_t delta) {
    for (auto& maplet : mapping_) maplet.offsetBy(delta);
  }
  void finishAndSort() {
    MOZ_ASSERT(!sorted_);
    std::sort(mapping_.begin(), mapping_.end());
    sorted_ = true;
  }
  void finishAlreadySorted() {
    MOZ_ASSERT(!sorted_);
    MOZ_ASSERT(std::is_sorted(mapping_.begin(), mapping_.end()));
    sorted_ = true;
  }
  const StackMap* findMap(const uint8_t* nextInsnAddr) const {
    struct Comparator {
      int operator()(Maplet aVal) const {
        if (uintptr_t(mTarget) < uintptr_t(aVal.nextInsnAddr)) {
          return -1;
        }
        if (uintptr_t(mTarget) > uintptr_t(aVal.nextInsnAddr)) {
          return 1;
        }
        return 0;
      }
      explicit Comparator(const uint8_t* aTarget) : mTarget(aTarget) {}
      const uint8_t* mTarget;
    };

    size_t result;
    if (mozilla::BinarySearchIf(mapping_, 0, mapping_.length(),
                                Comparator(nextInsnAddr), &result)) {
      return mapping_[result].map;
    }

    return nullptr;
  }
};

// Supporting code for creation of stackmaps.

// StackArgAreaSizeUnaligned returns the size, in bytes, of the stack arg area
// size needed to pass |argTypes|, excluding any alignment padding beyond the
// size of the area as a whole.  The size is as determined by the platforms
// native ABI.
//
// StackArgAreaSizeAligned returns the same, but rounded up to the nearest 16
// byte boundary.
//
// Note, StackArgAreaSize{Unaligned,Aligned}() must process all the arguments
// in order to take into account all necessary alignment constraints.  The
// signature must include any receiver argument -- in other words, it must be
// the complete native-ABI-level call signature.
template <class T>
static inline size_t StackArgAreaSizeUnaligned(const T& argTypes) {
  jit::WasmABIArgIter<const T> i(argTypes);
  while (!i.done()) {
    i++;
  }
  return i.stackBytesConsumedSoFar();
}

static inline size_t StackArgAreaSizeUnaligned(
    const SymbolicAddressSignature& saSig) {
  // WasmABIArgIter::ABIArgIter wants the items to be iterated over to be
  // presented in some type that has methods length() and operator[].  So we
  // have to wrap up |saSig|'s array of types in this API-matching class.
  class MOZ_STACK_CLASS ItemsAndLength {
    const MIRType* items_;
    size_t length_;

   public:
    ItemsAndLength(const MIRType* items, size_t length)
        : items_(items), length_(length) {}
    size_t length() const { return length_; }
    MIRType operator[](size_t i) const { return items_[i]; }
  };

  // Assert, at least crudely, that we're not accidentally going to run off
  // the end of the array of types, nor into undefined parts of it, while
  // iterating.
  MOZ_ASSERT(saSig.numArgs <
             sizeof(saSig.argTypes) / sizeof(saSig.argTypes[0]));
  MOZ_ASSERT(saSig.argTypes[saSig.numArgs] == MIRType::None /*the end marker*/);

  ItemsAndLength itemsAndLength(saSig.argTypes, saSig.numArgs);
  return StackArgAreaSizeUnaligned(itemsAndLength);
}

static inline size_t AlignStackArgAreaSize(size_t unalignedSize) {
  return AlignBytes(unalignedSize, jit::WasmStackAlignment);
}

// A stackmap creation helper.  Create a stackmap from a vector of booleans.
// The caller owns the resulting stackmap.

using StackMapBoolVector = Vector<bool, 128, SystemAllocPolicy>;

wasm::StackMap* ConvertStackMapBoolVectorToStackMap(
    const StackMapBoolVector& vec, bool hasRefs);

// Generate a stackmap for a function's stack-overflow-at-entry trap, with
// the structure:
//
//    <reg dump area>
//    |       ++ <space reserved before trap, if any>
//    |               ++ <space for Frame>
//    |                       ++ <inbound arg area>
//    |                                           |
//    Lowest Addr                                 Highest Addr
//
// The caller owns the resulting stackmap.  This assumes a grow-down stack.
//
// For non-debug builds, if the stackmap would contain no pointers, no
// stackmap is created, and nullptr is returned.  For a debug build, a
// stackmap is always created and returned.
//
// The "space reserved before trap" is the space reserved by
// MacroAssembler::wasmReserveStackChecked, in the case where the frame is
// "small", as determined by that function.
[[nodiscard]] bool CreateStackMapForFunctionEntryTrap(
    const ArgTypeVector& argTypes, const jit::RegisterOffsets& trapExitLayout,
    size_t trapExitLayoutWords, size_t nBytesReservedBeforeTrap,
    size_t nInboundStackArgBytes, wasm::StackMap** result);

// At a resumable wasm trap, the machine's registers are saved on the stack by
// (code generated by) GenerateTrapExit().  This function writes into |args| a
// vector of booleans describing the ref-ness of the saved integer registers.
// |args[0]| corresponds to the low addressed end of the described section of
// the save area.
[[nodiscard]] bool GenerateStackmapEntriesForTrapExit(
    const ArgTypeVector& args, const jit::RegisterOffsets& trapExitLayout,
    const size_t trapExitLayoutNumWords, ExitStubMapVector* extras);

// Shared write barrier code.
//
// A barriered store looks like this:
//
//   Label skipPreBarrier;
//   EmitWasmPreBarrierGuard(..., &skipPreBarrier);
//   <COMPILER-SPECIFIC ACTIONS HERE>
//   EmitWasmPreBarrierCall(...);
//   bind(&skipPreBarrier);
//
//   <STORE THE VALUE IN MEMORY HERE>
//
//   Label skipPostBarrier;
//   <COMPILER-SPECIFIC ACTIONS HERE>
//   EmitWasmPostBarrierGuard(..., &skipPostBarrier);
//   <CALL POST-BARRIER HERE IN A COMPILER-SPECIFIC WAY>
//   bind(&skipPostBarrier);
//
// The actions are divided up to allow other actions to be placed between them,
// such as saving and restoring live registers.  The postbarrier call invokes
// C++ and will kill all live registers.

// Before storing a GC pointer value in memory, skip to `skipBarrier` if the
// prebarrier is not needed.  Will clobber `scratch`.
//
// It is OK for `instance` and `scratch` to be the same register.
//
// If `trapOffset` is non-null, then metadata to catch a null access and emit
// a null pointer exception will be emitted. This will only catch a null access
// due to an incremental GC being in progress, the write that follows this
// pre-barrier guard must also be guarded against null.

void EmitWasmPreBarrierGuard(jit::MacroAssembler& masm, Register instance,
                             Register scratch, Register valueAddr,
                             size_t valueOffset, Label* skipBarrier,
                             BytecodeOffset* trapOffset);

// Before storing a GC pointer value in memory, call out-of-line prebarrier
// code. This assumes `PreBarrierReg` contains the address that will be updated.
// On ARM64 it also assums that x28 (the PseudoStackPointer) has the same value
// as SP.  `PreBarrierReg` is preserved by the barrier function.  Will clobber
// `scratch`.
//
// It is OK for `instance` and `scratch` to be the same register.

void EmitWasmPreBarrierCall(jit::MacroAssembler& masm, Register instance,
                            Register scratch, Register valueAddr,
                            size_t valueOffset);

// After storing a GC pointer value in memory, skip to `skipBarrier` if a
// postbarrier is not needed.  If the location being set is in an heap-allocated
// object then `object` must reference that object; otherwise it should be None.
// The value that was stored is `setValue`.  Will clobber `otherScratch` and
// will use other available scratch registers.
//
// `otherScratch` cannot be a designated scratch register.

void EmitWasmPostBarrierGuard(jit::MacroAssembler& masm,
                              const mozilla::Maybe<Register>& object,
                              Register otherScratch, Register setValue,
                              Label* skipBarrier);

#ifdef DEBUG
// Check whether |nextPC| is a valid code address for a stackmap created by
// this compiler.
bool IsValidStackMapKey(bool debugEnabled, const uint8_t* nextPC);
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_gc_h
