/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonCacheIRCompiler_h
#define jit_IonCacheIRCompiler_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "jstypes.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CacheIRReader.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "js/Vector.h"

struct JS_PUBLIC_API JSContext;

namespace js {
namespace jit {

class CacheIRWriter;
class CodeOffset;
class IonIC;
class IonICStub;
class IonScript;
class JitCode;
class MacroAssembler;

// IonCacheIRCompiler compiles CacheIR to IonIC native code.
class MOZ_RAII IonCacheIRCompiler : public CacheIRCompiler {
 public:
  friend class AutoSaveLiveRegisters;
  friend class AutoCallVM;

  IonCacheIRCompiler(JSContext* cx, TempAllocator& alloc,
                     const CacheIRWriter& writer, IonIC* ic,
                     IonScript* ionScript, uint32_t stubDataOffset);

  [[nodiscard]] bool init();
  JitCode* compile(IonICStub* stub);

#ifdef DEBUG
  void assertFloatRegisterAvailable(FloatRegister reg);
#endif

  IonICPerfSpewer& perfSpewer() { return perfSpewer_; }
  uint8_t localTracingSlots() const { return localTracingSlots_; }

 private:
  const CacheIRWriter& writer_;
  IonIC* ic_;
  IonScript* ionScript_;

  Vector<CodeOffset, 4, SystemAllocPolicy> nextCodeOffsets_;
  mozilla::Maybe<LiveRegisterSet> liveRegs_;
  mozilla::Maybe<CodeOffset> stubJitCodeOffset_;

  bool savedLiveRegs_;
  uint8_t localTracingSlots_;

  IonICPerfSpewer perfSpewer_;

  template <typename T>
  T rawPointerStubField(uint32_t offset);

  template <typename T>
  T rawInt64StubField(uint32_t offset);

  void enterStubFrame(MacroAssembler& masm, const AutoSaveLiveRegisters&);
  void storeTracedValue(MacroAssembler& masm, ValueOperand value);
  void loadTracedValue(MacroAssembler& masm, uint8_t slotIndex,
                       ValueOperand value);

  template <typename Fn, Fn fn>
  void callVM(MacroAssembler& masm);

  [[nodiscard]] bool emitAddAndStoreSlotShared(
      CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
      uint32_t newShapeOffset, mozilla::Maybe<uint32_t> numNewSlotsOffset);

  template <typename IdType>
  [[nodiscard]] bool emitCallScriptedProxyGetShared(
      ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
      ObjOperandId trapId, IdType id, uint32_t nargsAndFlags);

  void pushStubCodePointer();

  CACHE_IR_COMPILER_UNSHARED_GENERATED
};

}  // namespace jit
}  // namespace js

#endif /* jit_IonCacheIRCompiler_h */
