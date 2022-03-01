/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCacheIRCompiler_h
#define jit_BaselineCacheIRCompiler_h

#include "mozilla/Maybe.h"

#include "gc/Barrier.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"

namespace js {
namespace jit {

class ICCacheIRStub;
class ICFallbackStub;

ICCacheIRStub* AttachBaselineCacheIRStub(JSContext* cx,
                                         const CacheIRWriter& writer,
                                         CacheKind kind, JSScript* outerScript,
                                         ICScript* icScript,
                                         ICFallbackStub* stub, bool* attached);

// BaselineCacheIRCompiler compiles CacheIR to BaselineIC native code.
class MOZ_RAII BaselineCacheIRCompiler : public CacheIRCompiler {
  bool makesGCCalls_;

  void tailCallVMInternal(MacroAssembler& masm, TailCallVMFunctionId id);

  template <typename Fn, Fn fn>
  void tailCallVM(MacroAssembler& masm);

  [[nodiscard]] bool emitStoreSlotShared(bool isFixed, ObjOperandId objId,
                                         uint32_t offsetOffset,
                                         ValOperandId rhsId);
  [[nodiscard]] bool emitAddAndStoreSlotShared(
      CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
      uint32_t newShapeOffset, mozilla::Maybe<uint32_t> numNewSlotsOffset);

  bool updateArgc(CallFlags flags, Register argcReg, Register scratch);
  void loadStackObject(ArgumentKind kind, CallFlags flags, size_t stackPushed,
                       Register argcReg, Register dest);
  void pushArguments(Register argcReg, Register calleeReg, Register scratch,
                     Register scratch2, CallFlags flags, bool isJitCall);
  void pushStandardArguments(Register argcReg, Register scratch,
                             Register scratch2, bool isJitCall,
                             bool isConstructing);
  void pushArrayArguments(Register argcReg, Register scratch, Register scratch2,
                          bool isJitCall, bool isConstructing);
  void pushFunCallArguments(Register argcReg, Register calleeReg,
                            Register scratch, Register scratch2,
                            bool isJitCall);
  void pushFunApplyArgsObj(Register argcReg, Register calleeReg,
                           Register scratch, Register scratch2, bool isJitCall);
  void createThis(Register argcReg, Register calleeReg, Register scratch,
                  CallFlags flags);
  template <typename T>
  void storeThis(const T& newThis, Register argcReg, CallFlags flags);
  void updateReturnValue();

  enum class NativeCallType { Native, ClassHook };
  bool emitCallNativeShared(NativeCallType callType, ObjOperandId calleeId,
                            Int32OperandId argcId, CallFlags flags,
                            mozilla::Maybe<bool> ignoresReturnValue,
                            mozilla::Maybe<uint32_t> targetOffset);

  enum class StringCode { CodeUnit, CodePoint };
  bool emitStringFromCodeResult(Int32OperandId codeId, StringCode stringCode);

  bool emitCallScriptedGetterShared(ValOperandId receiverId,
                                    uint32_t getterOffset, bool sameRealm,
                                    uint32_t nargsAndFlagsOffset,
                                    mozilla::Maybe<uint32_t> icScriptOffset);
  bool emitCallScriptedSetterShared(ObjOperandId receiverId,
                                    uint32_t setterOffset, ValOperandId rhsId,
                                    bool sameRealm,
                                    uint32_t nargsAndFlagsOffset,
                                    mozilla::Maybe<uint32_t> icScriptOffset);

 public:
  friend class AutoStubFrame;

  BaselineCacheIRCompiler(JSContext* cx, const CacheIRWriter& writer,
                          uint32_t stubDataOffset);

  [[nodiscard]] bool init(CacheKind kind);

  template <typename Fn, Fn fn>
  void callVM(MacroAssembler& masm);

  JitCode* compile();

  bool makesGCCalls() const;

  Address stubAddress(uint32_t offset) const;

 private:
  CACHE_IR_COMPILER_UNSHARED_GENERATED
};

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineCacheIRCompiler_h */
