/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonCacheIRCompiler_h
#define jit_IonCacheIRCompiler_h

#include "mozilla/Maybe.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/IonIC.h"

namespace js {
namespace jit {

// IonCacheIRCompiler compiles CacheIR to IonIC native code.
class MOZ_RAII IonCacheIRCompiler : public CacheIRCompiler {
 public:
  friend class AutoSaveLiveRegisters;
  friend class AutoCallVM;

  IonCacheIRCompiler(JSContext* cx, const CacheIRWriter& writer, IonIC* ic,
                     IonScript* ionScript, uint32_t stubDataOffset);

  [[nodiscard]] bool init();
  JitCode* compile(IonICStub* stub);

#ifdef DEBUG
  void assertFloatRegisterAvailable(FloatRegister reg);
#endif

 private:
  const CacheIRWriter& writer_;
  IonIC* ic_;
  IonScript* ionScript_;

  Vector<CodeOffset, 4, SystemAllocPolicy> nextCodeOffsets_;
  mozilla::Maybe<LiveRegisterSet> liveRegs_;
  mozilla::Maybe<CodeOffset> stubJitCodeOffset_;

  bool savedLiveRegs_;

  template <typename T>
  T rawPointerStubField(uint32_t offset);

  template <typename T>
  T rawInt64StubField(uint32_t offset);

  void prepareVMCall(MacroAssembler& masm, const AutoSaveLiveRegisters&);

  template <typename Fn, Fn fn>
  void callVM(MacroAssembler& masm);

  [[nodiscard]] bool emitAddAndStoreSlotShared(
      CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
      uint32_t newShapeOffset, mozilla::Maybe<uint32_t> numNewSlotsOffset);

  void pushStubCodePointer();

  CACHE_IR_COMPILER_UNSHARED_GENERATED
};

}  // namespace jit
}  // namespace js

#endif /* jit_IonCacheIRCompiler_h */
