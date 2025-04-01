/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedStencil_h
#define vm_SharedStencil_h

#include "mozilla/Assertions.h"     // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/Atomics.h"        // mozilla::{Atomic, SequentiallyConsistent}
#include "mozilla/CheckedInt.h"     // mozilla::CheckedInt
#include "mozilla/HashFunctions.h"  // mozilla::HahNumber, mozilla::HashBytes
#include "mozilla/HashTable.h"      // mozilla::HashSet
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf
#include "mozilla/RefPtr.h"           // RefPtr
#include "mozilla/Span.h"             // mozilla::Span

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint16_t, uint32_t

#include "frontend/SourceNotes.h"  // js::SrcNote
#include "frontend/TypedIndex.h"   // js::frontend::TypedIndex

#include "js/AllocPolicy.h"            // js::SystemAllocPolicy
#include "js/ColumnNumber.h"           // JS::LimitedColumnNumberOneOrigin
#include "js/TypeDecls.h"              // JSContext,jsbytecode
#include "js/UniquePtr.h"              // js::UniquePtr
#include "js/Vector.h"                 // js::Vector
#include "util/EnumFlags.h"            // js::EnumFlags
#include "util/TrailingArray.h"        // js::TrailingArray
#include "vm/GeneratorAndAsyncKind.h"  // GeneratorKind, FunctionAsyncKind
#include "vm/StencilEnums.h"  // js::{TryNoteKind,ImmutableScriptFlagsEnum,MutableScriptFlagsEnum}

//
// Data structures shared between Stencil and the VM.
//

namespace js {

class FrontendContext;

namespace frontend {
class StencilXDR;
}  // namespace frontend

// Index into gcthings array.
class GCThingIndexType;
class GCThingIndex : public frontend::TypedIndex<GCThingIndexType> {
  // Delegate constructors;
  using Base = frontend::TypedIndex<GCThingIndexType>;
  using Base::Base;

 public:
  static constexpr GCThingIndex outermostScopeIndex() {
    return GCThingIndex(0);
  }

  static constexpr GCThingIndex invalid() { return GCThingIndex(UINT32_MAX); }

  GCThingIndex next() const { return GCThingIndex(index + 1); }
};

/*
 * Exception handling record.
 */
struct TryNote {
  uint32_t kind_;      /* one of TryNoteKind */
  uint32_t stackDepth; /* stack depth upon exception handler entry */
  uint32_t start;      /* start of the try statement or loop relative
                          to script->code() */
  uint32_t length;     /* length of the try statement or loop */

  TryNote(uint32_t kind, uint32_t stackDepth, uint32_t start, uint32_t length)
      : kind_(kind), stackDepth(stackDepth), start(start), length(length) {}

  TryNote() = default;

  TryNoteKind kind() const { return TryNoteKind(kind_); }

  bool isLoop() const {
    switch (kind()) {
      case TryNoteKind::Loop:
      case TryNoteKind::ForIn:
      case TryNoteKind::ForOf:
        return true;
      case TryNoteKind::Catch:
      case TryNoteKind::Finally:
      case TryNoteKind::ForOfIterClose:
      case TryNoteKind::Destructuring:
        return false;
    }
    MOZ_CRASH("Unexpected try note kind");
  }
};

// A block scope has a range in bytecode: it is entered at some offset, and left
// at some later offset.  Scopes can be nested.  Given an offset, the
// ScopeNote containing that offset whose with the highest start value
// indicates the block scope.  The block scope list is sorted by increasing
// start value.
//
// It is possible to leave a scope nonlocally, for example via a "break"
// statement, so there may be short bytecode ranges in a block scope in which we
// are popping the block chain in preparation for a goto.  These exits are also
// nested with respect to outer scopes.  The scopes in these exits are indicated
// by the "index" field, just like any other block.  If a nonlocal exit pops the
// last block scope, the index will be NoScopeIndex.
//
struct ScopeNote {
  // Sentinel index for no Scope.
  static constexpr GCThingIndex NoScopeIndex = GCThingIndex::invalid();

  // Sentinel index for no ScopeNote.
  static const uint32_t NoScopeNoteIndex = UINT32_MAX;

  // Index of the js::Scope in the script's gcthings array, or NoScopeIndex if
  // there is no block scope in this range.
  GCThingIndex index;

  // Bytecode offset at which this scope starts relative to script->code().
  uint32_t start = 0;

  // Length of bytecode span this scope covers.
  uint32_t length = 0;

  // Index of parent block scope in notes, or NoScopeNoteIndex.
  uint32_t parent = 0;
};

// Range of characters in scriptSource which contains a script's source,
// that is, the range used by the Parser to produce a script.
//
// For most functions the fields point to the following locations.
//
//   function * foo(a, b) { return a + b; }
//   ^             ^                       ^
//   |             |                       |
//   |             sourceStart     sourceEnd
//   |                                     |
//   toStringStart               toStringEnd
//
// For the special case of class constructors, the spec requires us to use an
// alternate definition of toStringStart / toStringEnd.
//
//   class C { constructor() { this.field = 42; } }
//   ^                    ^                      ^ ^
//   |                    |                      | |
//   |                    sourceStart    sourceEnd |
//   |                                             |
//   toStringStart                       toStringEnd
//
// Implicit class constructors use the following definitions.
//
//   class C { someMethod() { } }
//   ^                           ^
//   |                           |
//   sourceStart         sourceEnd
//   |                           |
//   toStringStart     toStringEnd
//
// Field initializer lambdas are internal details of the engine, but we still
// provide a sensible definition of these values.
//
//   class C { static field = 1 }
//   class C {        field = 1 }
//   class C {        somefield }
//                    ^        ^
//                    |        |
//          sourceStart        sourceEnd
//
// The non-static private class methods (including getters and setters) ALSO
// create a hidden initializer lambda in addition to the method itself. These
// lambdas are not exposed directly to script.
//
//   class C { #field() {       } }
//   class C { get #field() {   } }
//   class C { async #field() { } }
//   class C { * #field() {     } }
//             ^                 ^
//             |                 |
//             sourceStart       sourceEnd
//
// NOTE: These are counted in Code Units from the start of the script source.
//
// Also included in the SourceExtent is the line and column numbers of the
// sourceStart position. Compilation options may specify the initial line and
// column number.
//
// NOTE: Column number may saturate and must not be used as unique identifier.
struct SourceExtent {
  SourceExtent() = default;

  SourceExtent(uint32_t sourceStart, uint32_t sourceEnd, uint32_t toStringStart,
               uint32_t toStringEnd, uint32_t lineno,
               JS::LimitedColumnNumberOneOrigin column)
      : sourceStart(sourceStart),
        sourceEnd(sourceEnd),
        toStringStart(toStringStart),
        toStringEnd(toStringEnd),
        lineno(lineno),
        column(column) {}

  static SourceExtent makeGlobalExtent(uint32_t len) {
    return SourceExtent(0, len, 0, len, 1, JS::LimitedColumnNumberOneOrigin());
  }

  static SourceExtent makeGlobalExtent(
      uint32_t len, uint32_t lineno, JS::LimitedColumnNumberOneOrigin column) {
    return SourceExtent(0, len, 0, len, lineno, column);
  }

  // FunctionKey is an encoded position of a function within the source text
  // that is unique and reproducible.
  using FunctionKey = uint32_t;
  static constexpr FunctionKey NullFunctionKey = 0;

  uint32_t sourceStart = 0;
  uint32_t sourceEnd = 0;
  uint32_t toStringStart = 0;
  uint32_t toStringEnd = 0;

  // Line and column of |sourceStart_| position.
  // Line number (1-origin).
  uint32_t lineno = 1;
  // Column number in UTF-16 code units.
  JS::LimitedColumnNumberOneOrigin column;

  FunctionKey toFunctionKey() const {
    // In eval("x=>1"), the arrow function will have a sourceStart of 0 which
    // conflicts with the NullFunctionKey, so shift all keys by 1 instead.
    auto result = sourceStart + 1;
    MOZ_ASSERT(result != NullFunctionKey);
    return result;
  }
};

class ImmutableScriptFlags : public EnumFlags<ImmutableScriptFlagsEnum> {
 public:
  ImmutableScriptFlags() = default;

  explicit ImmutableScriptFlags(FieldType rawFlags) : EnumFlags(rawFlags) {}

  operator FieldType() const { return flags_; }
};

class MutableScriptFlags : public EnumFlags<MutableScriptFlagsEnum> {
 public:
  MutableScriptFlags() = default;

  MutableScriptFlags& operator&=(const FieldType rhs) {
    flags_ &= rhs;
    return *this;
  }

  MutableScriptFlags& operator|=(const FieldType rhs) {
    flags_ |= rhs;
    return *this;
  }

  operator FieldType() const { return flags_; }
};

#define GENERIC_FLAGS_READ_ONLY(Field, Enum) \
  [[nodiscard]] bool hasFlag(Enum flag) const { return Field.hasFlag(flag); }

#define GENERIC_FLAGS_READ_WRITE(Field, Enum)                                 \
  [[nodiscard]] bool hasFlag(Enum flag) const { return Field.hasFlag(flag); } \
  void setFlag(Enum flag, bool b = true) { Field.setFlag(flag, b); }          \
  void clearFlag(Enum flag) { Field.clearFlag(flag); }

#define GENERIC_FLAG_GETTER(enumName, lowerName, name) \
  bool lowerName() const { return hasFlag(enumName::name); }

#define GENERIC_FLAG_GETTER_SETTER(enumName, lowerName, name) \
  GENERIC_FLAG_GETTER(enumName, lowerName, name)              \
  void set##name() { setFlag(enumName::name); }               \
  void set##name(bool b) { setFlag(enumName::name, b); }      \
  void clear##name() { clearFlag(enumName::name); }

#define IMMUTABLE_SCRIPT_FLAGS_WITH_ACCESSORS(_)                              \
  _(ImmutableFlags, isForEval, IsForEval)                                     \
  _(ImmutableFlags, isModule, IsModule)                                       \
  _(ImmutableFlags, isFunction, IsFunction)                                   \
  _(ImmutableFlags, selfHosted, SelfHosted)                                   \
  _(ImmutableFlags, forceStrict, ForceStrict)                                 \
  _(ImmutableFlags, hasNonSyntacticScope, HasNonSyntacticScope)               \
  _(ImmutableFlags, noScriptRval, NoScriptRval)                               \
  _(ImmutableFlags, treatAsRunOnce, TreatAsRunOnce)                           \
  _(ImmutableFlags, strict, Strict)                                           \
  _(ImmutableFlags, hasModuleGoal, HasModuleGoal)                             \
  _(ImmutableFlags, hasInnerFunctions, HasInnerFunctions)                     \
  _(ImmutableFlags, hasDirectEval, HasDirectEval)                             \
  _(ImmutableFlags, bindingsAccessedDynamically, BindingsAccessedDynamically) \
  _(ImmutableFlags, hasCallSiteObj, HasCallSiteObj)                           \
  _(ImmutableFlags, isAsync, IsAsync)                                         \
  _(ImmutableFlags, isGenerator, IsGenerator)                                 \
  _(ImmutableFlags, funHasExtensibleScope, FunHasExtensibleScope)             \
  _(ImmutableFlags, functionHasThisBinding, FunctionHasThisBinding)           \
  _(ImmutableFlags, needsHomeObject, NeedsHomeObject)                         \
  _(ImmutableFlags, isDerivedClassConstructor, IsDerivedClassConstructor)     \
  _(ImmutableFlags, isSyntheticFunction, IsSyntheticFunction)                 \
  _(ImmutableFlags, useMemberInitializers, UseMemberInitializers)             \
  _(ImmutableFlags, hasRest, HasRest)                                         \
  _(ImmutableFlags, needsFunctionEnvironmentObjects,                          \
    NeedsFunctionEnvironmentObjects)                                          \
  _(ImmutableFlags, functionHasExtraBodyVarScope,                             \
    FunctionHasExtraBodyVarScope)                                             \
  _(ImmutableFlags, shouldDeclareArguments, ShouldDeclareArguments)           \
  _(ImmutableFlags, needsArgsObj, NeedsArgsObj)                               \
  _(ImmutableFlags, hasMappedArgsObj, HasMappedArgsObj)                       \
  _(ImmutableFlags, isInlinableLargeFunction, IsInlinableLargeFunction)       \
  _(ImmutableFlags, functionHasNewTargetBinding, FunctionHasNewTargetBinding) \
  _(ImmutableFlags, usesArgumentsIntrinsics, UsesArgumentsIntrinsics)         \
                                                                              \
  GeneratorKind generatorKind() const {                                       \
    return isGenerator() ? GeneratorKind::Generator                           \
                         : GeneratorKind::NotGenerator;                       \
  }                                                                           \
                                                                              \
  FunctionAsyncKind asyncKind() const {                                       \
    return isAsync() ? FunctionAsyncKind::AsyncFunction                       \
                     : FunctionAsyncKind::SyncFunction;                       \
  }                                                                           \
                                                                              \
  bool isRelazifiable() const {                                               \
    /*                                                                        \
    ** A script may not be relazifiable if parts of it can be entrained in    \
    ** interesting ways:                                                      \
    **  - Scripts with inner-functions or direct-eval (which can add          \
    **    inner-functions) should not be relazified as their Scopes may be    \
    **    part of another scope-chain.                                        \
    **  - Generators and async functions may be re-entered in complex ways so \
    **    don't discard bytecode. The JIT resume code assumes this.           \
    **  - Functions with template literals must always return the same object \
    **    instance so must not discard it by relazifying.                     \
    */                                                                        \
    return !hasInnerFunctions() && !hasDirectEval() && !isGenerator() &&      \
           !isAsync() && !hasCallSiteObj();                                   \
  }

#define RO_IMMUTABLE_SCRIPT_FLAGS(Field)           \
  using ImmutableFlags = ImmutableScriptFlagsEnum; \
                                                   \
  GENERIC_FLAGS_READ_ONLY(Field, ImmutableFlags)   \
  IMMUTABLE_SCRIPT_FLAGS_WITH_ACCESSORS(GENERIC_FLAG_GETTER)

#define MUTABLE_SCRIPT_FLAGS_WITH_ACCESSORS(_)                          \
  _(MutableFlags, hasRunOnce, HasRunOnce)                               \
  _(MutableFlags, hasScriptCounts, HasScriptCounts)                     \
  _(MutableFlags, hasDebugScript, HasDebugScript)                       \
  _(MutableFlags, allowRelazify, AllowRelazify)                         \
  _(MutableFlags, spewEnabled, SpewEnabled)                             \
  _(MutableFlags, needsFinalWarmUpCount, NeedsFinalWarmUpCount)         \
  _(MutableFlags, failedBoundsCheck, FailedBoundsCheck)                 \
  _(MutableFlags, hadLICMInvalidation, HadLICMInvalidation)             \
  _(MutableFlags, hadReorderingBailout, HadReorderingBailout)           \
  _(MutableFlags, hadEagerTruncationBailout, HadEagerTruncationBailout) \
  _(MutableFlags, hadUnboxFoldingBailout, HadUnboxFoldingBailout)       \
  _(MutableFlags, baselineDisabled, BaselineDisabled)                   \
  _(MutableFlags, ionDisabled, IonDisabled)                             \
  _(MutableFlags, uninlineable, Uninlineable)                           \
  _(MutableFlags, noEagerBaselineHint, NoEagerBaselineHint)             \
  _(MutableFlags, failedLexicalCheck, FailedLexicalCheck)               \
  _(MutableFlags, hadSpeculativePhiBailout, HadSpeculativePhiBailout)

#define RW_MUTABLE_SCRIPT_FLAGS(Field)          \
  using MutableFlags = MutableScriptFlagsEnum;  \
                                                \
  GENERIC_FLAGS_READ_WRITE(Field, MutableFlags) \
  MUTABLE_SCRIPT_FLAGS_WITH_ACCESSORS(GENERIC_FLAG_GETTER_SETTER)

// [SMDOC] JSScript data layout (immutable)
//
// ImmutableScriptData stores variable-length script data that may be shared
// between scripts with the same bytecode, even across different GC Zones.
// Abstractly this structure consists of multiple (optional) arrays that are
// exposed as mozilla::Span<T>. These arrays exist in a single heap allocation.
//
// Under the hood, ImmutableScriptData is a fixed-size header class followed
// the various array bodies interleaved with metadata to compactly encode the
// bounds. These arrays have varying requirements for alignment, performance,
// and jit-friendliness which leads to the complex indexing system below.
//
// Note: The '----' separators are for readability only.
//
// ----
//   <ImmutableScriptData itself>
// ----
//   (REQUIRED) Flags structure
//   (REQUIRED) Array of jsbytecode constituting code()
//   (REQUIRED) Array of SrcNote constituting notes()
// ----
//   (OPTIONAL) Array of uint32_t optional-offsets
//  optArrayOffset:
// ----
//  L0:
//   (OPTIONAL) Array of uint32_t constituting resumeOffsets()
//  L1:
//   (OPTIONAL) Array of ScopeNote constituting scopeNotes()
//  L2:
//   (OPTIONAL) Array of TryNote constituting tryNotes()
//  L3:
// ----
//
// NOTE: The notes() array must have been padded such that
//       flags/code/notes together have uint32_t alignment.
//
// The labels shown are recorded as byte-offsets relative to 'this'. This is to
// reduce memory as well as make ImmutableScriptData easier to share across
// processes.
//
// The L0/L1/L2/L3 labels indicate the start and end of the optional arrays.
// Some of these labels may refer to the same location if the array between
// them is empty. Each unique label position has an offset stored in the
// optional-offsets table. Note that we also avoid entries for labels that
// match 'optArrayOffset'. This saves memory when arrays are empty.
//
// The flags() data indicates (for each optional array) which entry from the
// optional-offsets table marks the *end* of array. The array starts where the
// previous array ends (with the first array beginning at 'optArrayOffset').
// The optional-offset table is addressed at negative indices from
// 'optArrayOffset'.
//
// In general, the length of each array is computed from subtracting the start
// offset of the array from the start offset of the subsequent array. The
// notable exception is that bytecode length is stored explicitly.
class alignas(uint32_t) ImmutableScriptData final
    : public TrailingArray<ImmutableScriptData> {
 private:
  Offset optArrayOffset_ = 0;

  // Length of bytecode
  uint32_t codeLength_ = 0;

 public:
  // Offset of main entry point from code, after predef'ing prologue.
  uint32_t mainOffset = 0;

  // Fixed frame slots.
  uint32_t nfixed = 0;

  // Slots plus maximum stack depth.
  uint32_t nslots = 0;

  // Index into the gcthings array of the body scope.
  GCThingIndex bodyScopeIndex;

  // Number of IC entries to allocate in JitScript for Baseline ICs.
  uint32_t numICEntries = 0;

  // ES6 function length.
  uint16_t funLength = 0;

  // Property Count estimate
  uint16_t propertyCountEstimate = 0;

  // NOTE: The raw bytes of this structure are used for hashing so use explicit
  // padding values as needed for predicatable results across compilers

 private:
  struct Flags {
    uint8_t resumeOffsetsEndIndex : 2;
    uint8_t scopeNotesEndIndex : 2;
    uint8_t tryNotesEndIndex : 2;
    uint8_t _unused : 2;
  };
  static_assert(sizeof(Flags) == sizeof(uint8_t),
                "Structure packing is broken");

  // Offsets (in bytes) from 'this' to each component array. The delta between
  // each offset and the next offset is the size of each array and is defined
  // even if an array is empty.
  Offset flagOffset() const { return offsetOfCode() - sizeof(Flags); }
  Offset codeOffset() const { return offsetOfCode(); }
  Offset noteOffset() const { return offsetOfCode() + codeLength_; }
  Offset optionalOffsetsOffset() const {
    // Determine the location to beginning of optional-offsets array by looking
    // at index for try-notes.
    //
    //   optionalOffsetsOffset():
    //     (OPTIONAL) tryNotesEndOffset
    //     (OPTIONAL) scopeNotesEndOffset
    //     (OPTIONAL) resumeOffsetsEndOffset
    //   optArrayOffset_:
    //     ....
    unsigned numOffsets = flags().tryNotesEndIndex;
    MOZ_ASSERT(numOffsets >= flags().scopeNotesEndIndex);
    MOZ_ASSERT(numOffsets >= flags().resumeOffsetsEndIndex);

    return optArrayOffset_ - (numOffsets * sizeof(Offset));
  }
  Offset resumeOffsetsOffset() const { return optArrayOffset_; }
  Offset scopeNotesOffset() const {
    return getOptionalOffset(flags().resumeOffsetsEndIndex);
  }
  Offset tryNotesOffset() const {
    return getOptionalOffset(flags().scopeNotesEndIndex);
  }
  Offset endOffset() const {
    return getOptionalOffset(flags().tryNotesEndIndex);
  }

  void initOptionalArrays(Offset* cursor, uint32_t numResumeOffsets,
                          uint32_t numScopeNotes, uint32_t numTryNotes);

  // Initialize to GC-safe state
  ImmutableScriptData(uint32_t codeLength, uint32_t noteLength,
                      uint32_t numResumeOffsets, uint32_t numScopeNotes,
                      uint32_t numTryNotes);

  void setOptionalOffset(int index, Offset offset) {
    MOZ_ASSERT(index > 0);
    MOZ_ASSERT(offset != optArrayOffset_, "Do not store implicit offset");
    offsetToPointer<Offset>(optArrayOffset_)[-index] = offset;
  }
  Offset getOptionalOffset(int index) const {
    // The index 0 represents (implicitly) the offset 'optArrayOffset_'.
    if (index == 0) {
      return optArrayOffset_;
    }

    ImmutableScriptData* this_ = const_cast<ImmutableScriptData*>(this);
    return this_->offsetToPointer<Offset>(optArrayOffset_)[-index];
  }

 public:
  static js::UniquePtr<ImmutableScriptData> new_(
      FrontendContext* fc, uint32_t mainOffset, uint32_t nfixed,
      uint32_t nslots, GCThingIndex bodyScopeIndex, uint32_t numICEntries,
      bool isFunction, uint16_t funLength, uint16_t propertyCountEstimate,
      mozilla::Span<const jsbytecode> code, mozilla::Span<const SrcNote> notes,
      mozilla::Span<const uint32_t> resumeOffsets,
      mozilla::Span<const ScopeNote> scopeNotes,
      mozilla::Span<const TryNote> tryNotes);

  static js::UniquePtr<ImmutableScriptData> new_(
      FrontendContext* fc, uint32_t codeLength, uint32_t noteLength,
      uint32_t numResumeOffsets, uint32_t numScopeNotes, uint32_t numTryNotes);

  static js::UniquePtr<ImmutableScriptData> new_(FrontendContext* fc,
                                                 uint32_t totalSize);

  // Validate internal offsets of the data structure seems reasonable. This is
  // for diagnositic purposes only to detect severe corruption. This is not a
  // security boundary!
  bool validateLayout(uint32_t expectedSize);

 private:
  static mozilla::CheckedInt<uint32_t> sizeFor(uint32_t codeLength,
                                               uint32_t noteLength,
                                               uint32_t numResumeOffsets,
                                               uint32_t numScopeNotes,
                                               uint32_t numTryNotes);

 public:
  // The code() and note() arrays together maintain an target alignment by
  // padding the source notes with padding bytes. This allows arrays with
  // stricter alignment requirements to follow them.
  static constexpr size_t CodeNoteAlign = sizeof(uint32_t);

  // Compute number of padding notes to pad out source notes with.
  static uint32_t ComputeNotePadding(uint32_t codeLength, uint32_t noteLength) {
    uint32_t flagLength = sizeof(Flags);
    uint32_t paddingLength =
        CodeNoteAlign - (flagLength + codeLength + noteLength) % CodeNoteAlign;

    if (paddingLength == CodeNoteAlign) {
      return 0;
    }

    return paddingLength;
  }

  // Span over all raw bytes in this struct and its trailing arrays.
  mozilla::Span<const uint8_t> immutableData() const {
    size_t allocSize = endOffset();
    return mozilla::Span{reinterpret_cast<const uint8_t*>(this), allocSize};
  }

 private:
  Flags& flagsRef() { return *offsetToPointer<Flags>(flagOffset()); }
  const Flags& flags() const {
    return const_cast<ImmutableScriptData*>(this)->flagsRef();
  }

 public:
  uint32_t codeLength() const { return codeLength_; }
  jsbytecode* code() { return offsetToPointer<jsbytecode>(codeOffset()); }
  mozilla::Span<jsbytecode> codeSpan() { return {code(), codeLength()}; }

  uint32_t noteLength() const {
    return numElements<SrcNote>(noteOffset(), optionalOffsetsOffset());
  }
  SrcNote* notes() { return offsetToPointer<SrcNote>(noteOffset()); }
  mozilla::Span<SrcNote> notesSpan() { return {notes(), noteLength()}; }

  mozilla::Span<uint32_t> resumeOffsets() {
    return mozilla::Span{offsetToPointer<uint32_t>(resumeOffsetsOffset()),
                         offsetToPointer<uint32_t>(scopeNotesOffset())};
  }
  mozilla::Span<ScopeNote> scopeNotes() {
    return mozilla::Span{offsetToPointer<ScopeNote>(scopeNotesOffset()),
                         offsetToPointer<ScopeNote>(tryNotesOffset())};
  }
  mozilla::Span<TryNote> tryNotes() {
    return mozilla::Span{offsetToPointer<TryNote>(tryNotesOffset()),
                         offsetToPointer<TryNote>(endOffset())};
  }

  // Expose offsets to the JITs.
  static constexpr size_t offsetOfCode() {
    return sizeof(ImmutableScriptData) + sizeof(Flags);
  }
  static constexpr size_t offsetOfResumeOffsetsOffset() {
    // Resume-offsets are the first optional array if they exist. Locate the
    // array with the 'optArrayOffset_' field.
    static_assert(sizeof(Offset) == sizeof(uint32_t),
                  "JIT expect Offset to be uint32_t");
    return offsetof(ImmutableScriptData, optArrayOffset_);
  }
  static constexpr size_t offsetOfNfixed() {
    return offsetof(ImmutableScriptData, nfixed);
  }
  static constexpr size_t offsetOfNslots() {
    return offsetof(ImmutableScriptData, nslots);
  }
  static constexpr size_t offsetOfFunLength() {
    return offsetof(ImmutableScriptData, funLength);
  }

  // ImmutableScriptData has trailing data so isn't copyable or movable.
  ImmutableScriptData(const ImmutableScriptData&) = delete;
  ImmutableScriptData& operator=(const ImmutableScriptData&) = delete;
};

// Wrapper type for ImmutableScriptData to allow sharing across a JSRuntime.
//
// Note: This is distinct from ImmutableScriptData because it contains a mutable
//       ref-count while the ImmutableScriptData may live in read-only memory.
//
// Note: This is *not* directly inlined into the SharedImmutableScriptDataTable
//       because scripts point directly to object and table resizing moves
//       entries. This allows for fast finalization by decrementing the
//       ref-count directly without doing a hash-table lookup.
class SharedImmutableScriptData {
  static constexpr uint32_t IsExternalFlag = 0x80000000;
  static constexpr uint32_t RefCountBits = 0x7FFFFFFF;

  // This class is reference counted as follows: each pointer from a JSScript
  // counts as one reference plus there may be one reference from the shared
  // script data table.
  mozilla::Atomic<uint32_t, mozilla::SequentiallyConsistent>
      refCountAndExternalFlags_ = {};

  mozilla::HashNumber hash_;
  ImmutableScriptData* isd_ = nullptr;

  // End of fields.

  friend class ::JSScript;
  friend class js::frontend::StencilXDR;

 public:
  SharedImmutableScriptData() = default;

  ~SharedImmutableScriptData() { reset(); }

 private:
  bool isExternal() const { return refCountAndExternalFlags_ & IsExternalFlag; }
  void setIsExternal() { refCountAndExternalFlags_ |= IsExternalFlag; }
  void unsetIsExternal() { refCountAndExternalFlags_ &= RefCountBits; }

  void reset() {
    if (isd_ && !isExternal()) {
      js_delete(isd_);
    }
    isd_ = nullptr;
  }

  mozilla::HashNumber calculateHash() const {
    mozilla::Span<const uint8_t> immutableData = isd_->immutableData();
    return mozilla::HashBytes(immutableData.data(), immutableData.size());
  }

 public:
  // Hash over the contents of SharedImmutableScriptData and its
  // ImmutableScriptData.
  struct Hasher;

  uint32_t refCount() const { return refCountAndExternalFlags_ & RefCountBits; }
  void AddRef() { refCountAndExternalFlags_++; }

 private:
  uint32_t decrementRef() {
    MOZ_ASSERT(refCount() != 0);
    return --refCountAndExternalFlags_ & RefCountBits;
  }

 public:
  void Release() {
    uint32_t remain = decrementRef();
    if (remain == 0) {
      reset();
      js_free(this);
    }
  }

  static constexpr size_t offsetOfISD() {
    return offsetof(SharedImmutableScriptData, isd_);
  }

 private:
  static SharedImmutableScriptData* create(FrontendContext* fc);

 public:
  static SharedImmutableScriptData* createWith(
      FrontendContext* fc, js::UniquePtr<ImmutableScriptData>&& isd);

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
    size_t isdSize = isExternal() ? 0 : mallocSizeOf(isd_);
    return mallocSizeOf(this) + isdSize;
  }

  // SharedImmutableScriptData has trailing data so isn't copyable or movable.
  SharedImmutableScriptData(const SharedImmutableScriptData&) = delete;
  SharedImmutableScriptData& operator=(const SharedImmutableScriptData&) =
      delete;

  static bool shareScriptData(FrontendContext* fc,
                              RefPtr<SharedImmutableScriptData>& sisd);

  size_t immutableDataLength() const { return isd_->immutableData().Length(); }
  uint32_t nfixed() const { return isd_->nfixed; }

  ImmutableScriptData* get() { return isd_; }
  mozilla::HashNumber hash() const { return hash_; }

  void setOwn(js::UniquePtr<ImmutableScriptData>&& isd) {
    MOZ_ASSERT(!isd_);
    isd_ = isd.release();
    unsetIsExternal();

    hash_ = calculateHash();
  }

  void setOwn(js::UniquePtr<ImmutableScriptData>&& isd,
              mozilla::HashNumber hash) {
    MOZ_ASSERT(!isd_);
    isd_ = isd.release();
    unsetIsExternal();

    MOZ_ASSERT(hash == calculateHash());
    hash_ = hash;
  }

  void setExternal(ImmutableScriptData* isd) {
    MOZ_ASSERT(!isd_);
    isd_ = isd;
    setIsExternal();

    hash_ = calculateHash();
  }

  void setExternal(ImmutableScriptData* isd, mozilla::HashNumber hash) {
    MOZ_ASSERT(!isd_);
    isd_ = isd;
    setIsExternal();

    MOZ_ASSERT(hash == calculateHash());
    hash_ = hash;
  }
};

// Matches SharedImmutableScriptData objects that have the same atoms as well as
// contain the same bytes in their ImmutableScriptData.
struct SharedImmutableScriptData::Hasher {
  using Lookup = RefPtr<SharedImmutableScriptData>;

  static mozilla::HashNumber hash(const Lookup& l) { return l->hash(); }

  static bool match(SharedImmutableScriptData* entry, const Lookup& lookup) {
    return (entry->isd_->immutableData() == lookup->isd_->immutableData());
  }
};

using SharedImmutableScriptDataTable =
    mozilla::HashSet<SharedImmutableScriptData*,
                     SharedImmutableScriptData::Hasher, SystemAllocPolicy>;

struct MemberInitializers {
#ifdef ENABLE_DECORATORS
  static constexpr size_t NumBits = 30;
#else
  static constexpr size_t NumBits = 31;
#endif
  static constexpr uint32_t MaxInitializers = BitMask(NumBits);

#ifdef DEBUG
  bool valid = false;
#endif

  bool hasPrivateBrand : 1;

#ifdef ENABLE_DECORATORS
  bool hasDecorators : 1;
#endif

  // This struct will eventually have a vector of constant values for optimizing
  // field initializers.
  uint32_t numMemberInitializers : NumBits;

  MemberInitializers(bool hasPrivateBrand,
#ifdef ENABLE_DECORATORS
                     bool hasDecorators,
#endif
                     uint32_t numMemberInitializers)
      :
#ifdef DEBUG
        valid(true),
#endif
        hasPrivateBrand(hasPrivateBrand),
#ifdef ENABLE_DECORATORS
        hasDecorators(hasDecorators),
#endif
        numMemberInitializers(numMemberInitializers) {
#ifdef ENABLE_DECORATORS
    MOZ_ASSERT(
        this->numMemberInitializers == numMemberInitializers,
        "numMemberInitializers should easily fit in the 30-bit bitfield");
#else
    MOZ_ASSERT(
        this->numMemberInitializers == numMemberInitializers,
        "numMemberInitializers should easily fit in the 31-bit bitfield");
#endif
  }

  static MemberInitializers Invalid() { return MemberInitializers(); }

  // Singleton to use for class constructors that do not have to initialize any
  // fields. This is used when we elide the trivial data but still need a valid
  // set to stop scope walking.
  static const MemberInitializers& Empty() {
    static const MemberInitializers zeroInitializers(false,
#ifdef ENABLE_DECORATORS
                                                     false,
#endif
                                                     0);
    return zeroInitializers;
  }

  uint32_t serialize() const {
#ifdef ENABLE_DECORATORS
    auto serialised = (hasPrivateBrand << (NumBits + 1)) |
                      hasDecorators << NumBits | numMemberInitializers;
    return serialised;
#else
    return (hasPrivateBrand << NumBits) | numMemberInitializers;
#endif
  }

  static MemberInitializers deserialize(uint32_t bits) {
#ifdef ENABLE_DECORATORS
    return MemberInitializers((bits & Bit(NumBits + 1)) != 0,
                              (bits & Bit(NumBits)) != 0,
                              bits & BitMask(NumBits));
#else
    return MemberInitializers((bits & Bit(NumBits)) != 0,
                              bits & BitMask(NumBits));
#endif
  }

 private:
  MemberInitializers()
      :
#ifdef DEBUG
        valid(false),
#endif
        hasPrivateBrand(false),
#ifdef ENABLE_DECORATORS
        hasDecorators(false),
#endif
        numMemberInitializers(0) {
  }
};

// See JSOp::Lambda for interepretation of this index.
using FunctionDeclaration = GCThingIndex;
// Defined here to avoid #include cycle with Stencil.h.
using FunctionDeclarationVector =
    Vector<FunctionDeclaration, 0, js::SystemAllocPolicy>;

}  // namespace js

#endif /* vm_SharedStencil_h */
