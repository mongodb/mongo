/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCacheIRCompiler_h
#define jit_BaselineCacheIRCompiler_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CacheIRReader.h"

struct JS_PUBLIC_API JSContext;

class JSScript;

namespace js {
namespace jit {

class CacheIRWriter;
class ICFallbackStub;
class ICScript;
class JitCode;
class Label;
class MacroAssembler;

struct Address;
struct Register;

enum class ICAttachResult { Attached, DuplicateStub, TooLarge, OOM };

bool TryFoldingStubs(JSContext* cx, ICFallbackStub* fallback, JSScript* script,
                     ICScript* icScript);

ICAttachResult AttachBaselineCacheIRStub(JSContext* cx,
                                         const CacheIRWriter& writer,
                                         CacheKind kind, JSScript* outerScript,
                                         ICScript* icScript,
                                         ICFallbackStub* stub,
                                         const char* name);

// BaselineCacheIRCompiler compiles CacheIR to BaselineIC native code.
class MOZ_RAII BaselineCacheIRCompiler : public CacheIRCompiler {
  bool makesGCCalls_;
  uint8_t localTracingSlots_ = 0;
  Register baselineFrameReg_ = FramePointer;

  // This register points to the baseline frame of the caller. It should only
  // be used before we enter a stub frame. This is normally the frame pointer
  // register, but with --enable-ic-frame-pointers we have to allocate a
  // separate register.
  inline Register baselineFrameReg() {
    MOZ_ASSERT(!enteredStubFrame_);
    return baselineFrameReg_;
  }

  [[nodiscard]] bool emitStoreSlotShared(bool isFixed, ObjOperandId objId,
                                         uint32_t offsetOffset,
                                         ValOperandId rhsId);
  [[nodiscard]] bool emitAddAndStoreSlotShared(
      CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
      uint32_t newShapeOffset, mozilla::Maybe<uint32_t> numNewSlotsOffset);

  bool updateArgc(CallFlags flags, Register argcReg, Register scratch);
  void loadStackObject(ArgumentKind kind, CallFlags flags, Register argcReg,
                       Register dest);
  void pushArguments(Register argcReg, Register calleeReg, Register scratch,
                     Register scratch2, CallFlags flags, uint32_t argcFixed,
                     bool isJitCall);
  void pushStandardArguments(Register argcReg, Register scratch,
                             Register scratch2, uint32_t argcFixed,
                             bool isJitCall, bool isConstructing);
  void pushArrayArguments(Register argcReg, Register scratch, Register scratch2,
                          bool isJitCall, bool isConstructing);
  void pushFunCallArguments(Register argcReg, Register calleeReg,
                            Register scratch, Register scratch2,
                            uint32_t argcFixed, bool isJitCall);
  void pushFunApplyArgsObj(Register argcReg, Register calleeReg,
                           Register scratch, Register scratch2, bool isJitCall);
  void pushFunApplyNullUndefinedArguments(Register calleeReg, bool isJitCall);
  void pushBoundFunctionArguments(Register argcReg, Register calleeReg,
                                  Register scratch, Register scratch2,
                                  CallFlags flags, uint32_t numBoundArgs,
                                  bool isJitCall);
  void createThis(Register argcReg, Register calleeReg, Register scratch,
                  CallFlags flags, bool isBoundFunction);
  template <typename T>
  void storeThis(const T& newThis, Register argcReg, CallFlags flags);
  void updateReturnValue();

  enum class NativeCallType { Native, ClassHook };
  enum class ClearLocalAllocSite { No, Yes };
  bool emitCallNativeShared(
      NativeCallType callType, ObjOperandId calleeId, Int32OperandId argcId,
      CallFlags flags, uint32_t argcFixed,
      mozilla::Maybe<bool> ignoresReturnValue,
      mozilla::Maybe<uint32_t> targetOffset,
      ClearLocalAllocSite clearLocalAllocSite = ClearLocalAllocSite::No);
  void loadAllocSiteIntoContext(uint32_t siteOffset);

  enum class StringCode { CodeUnit, CodePoint };
  bool emitStringFromCodeResult(Int32OperandId codeId, StringCode stringCode);

  enum class StringCharOutOfBounds { Failure, EmptyString, UndefinedValue };
  bool emitLoadStringCharResult(StringOperandId strId, Int32OperandId indexId,
                                StringCharOutOfBounds outOfBounds);

  void emitAtomizeString(Register str, Register temp, Label* failure);

  bool emitCallScriptedGetterShared(ValOperandId receiverId,
                                    uint32_t getterOffset, bool sameRealm,
                                    uint32_t nargsAndFlagsOffset,
                                    mozilla::Maybe<uint32_t> icScriptOffset);
  bool emitCallScriptedSetterShared(ObjOperandId receiverId,
                                    uint32_t setterOffset, ValOperandId rhsId,
                                    bool sameRealm,
                                    uint32_t nargsAndFlagsOffset,
                                    mozilla::Maybe<uint32_t> icScriptOffset);

  template <typename IdType>
  bool emitCallScriptedProxyGetShared(ValOperandId targetId,
                                      ObjOperandId receiverId,
                                      ObjOperandId handlerId,
                                      ObjOperandId trapId, IdType id,
                                      uint32_t nargsAndFlags);

  BaselineICPerfSpewer perfSpewer_;

 public:
  BaselineICPerfSpewer& perfSpewer() { return perfSpewer_; }

  friend class AutoStubFrame;

  BaselineCacheIRCompiler(JSContext* cx, TempAllocator& alloc,
                          const CacheIRWriter& writer, uint32_t stubDataOffset);

  [[nodiscard]] bool init(CacheKind kind);

  template <typename Fn, Fn fn>
  void callVM(MacroAssembler& masm);

  JitCode* compile();

  bool makesGCCalls() const;
  bool localTracingSlots() const { return localTracingSlots_; }

  Address stubAddress(uint32_t offset) const;

 private:
  CACHE_IR_COMPILER_UNSHARED_GENERATED
};

// Special object used for storing a list of shapes to guard against. These are
// only used in the fields of CacheIR stubs and do not escape.
class ShapeListObject : public ListObject {
 public:
  static const JSClass class_;
  static const JSClassOps classOps_;
  static ShapeListObject* create(JSContext* cx);
  static void trace(JSTracer* trc, JSObject* obj);

  Shape* get(uint32_t index);
  bool traceWeak(JSTracer* trc);
};

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineCacheIRCompiler_h */
