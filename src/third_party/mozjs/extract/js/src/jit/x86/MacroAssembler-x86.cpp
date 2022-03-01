/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/MacroAssembler-x86.h"

#include "mozilla/Alignment.h"
#include "mozilla/Casting.h"

#include "jit/AtomicOp.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"
#include "util/Memory.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

void MacroAssemblerX86::loadConstantDouble(double d, FloatRegister dest) {
  if (maybeInlineDouble(d, dest)) {
    return;
  }
  Double* dbl = getDouble(d);
  if (!dbl) {
    return;
  }
  masm.vmovsd_mr(nullptr, dest.encoding());
  propagateOOM(dbl->uses.append(CodeOffset(masm.size())));
}

void MacroAssemblerX86::loadConstantFloat32(float f, FloatRegister dest) {
  if (maybeInlineFloat(f, dest)) {
    return;
  }
  Float* flt = getFloat(f);
  if (!flt) {
    return;
  }
  masm.vmovss_mr(nullptr, dest.encoding());
  propagateOOM(flt->uses.append(CodeOffset(masm.size())));
}

void MacroAssemblerX86::loadConstantSimd128Int(const SimdConstant& v,
                                               FloatRegister dest) {
  if (maybeInlineSimd128Int(v, dest)) {
    return;
  }
  SimdData* i4 = getSimdData(v);
  if (!i4) {
    return;
  }
  masm.vmovdqa_mr(nullptr, dest.encoding());
  propagateOOM(i4->uses.append(CodeOffset(masm.size())));
}

void MacroAssemblerX86::loadConstantSimd128Float(const SimdConstant& v,
                                                 FloatRegister dest) {
  if (maybeInlineSimd128Float(v, dest)) {
    return;
  }
  SimdData* f4 = getSimdData(v);
  if (!f4) {
    return;
  }
  masm.vmovaps_mr(nullptr, dest.encoding());
  propagateOOM(f4->uses.append(CodeOffset(masm.size())));
}

void MacroAssemblerX86::vpPatchOpSimd128(
    const SimdConstant& v, FloatRegister reg,
    void (X86Encoding::BaseAssemblerX86::*op)(
        const void* address, X86Encoding::XMMRegisterID srcId,
        X86Encoding::XMMRegisterID destId)) {
  SimdData* val = getSimdData(v);
  if (!val) {
    return;
  }
  (masm.*op)(nullptr, reg.encoding(), reg.encoding());
  propagateOOM(val->uses.append(CodeOffset(masm.size())));
}

void MacroAssemblerX86::vpPatchOpSimd128(
    const SimdConstant& v, FloatRegister reg,
    size_t (X86Encoding::BaseAssemblerX86::*op)(
        const void* address, X86Encoding::XMMRegisterID srcId,
        X86Encoding::XMMRegisterID destId)) {
  SimdData* val = getSimdData(v);
  if (!val) {
    return;
  }
  size_t patchOffsetFromEnd =
      (masm.*op)(nullptr, reg.encoding(), reg.encoding());
  propagateOOM(val->uses.append(CodeOffset(masm.size() - patchOffsetFromEnd)));
}

void MacroAssemblerX86::vpaddbSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddb_mr);
}

void MacroAssemblerX86::vpaddwSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddw_mr);
}

void MacroAssemblerX86::vpadddSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddd_mr);
}

void MacroAssemblerX86::vpaddqSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddq_mr);
}

void MacroAssemblerX86::vpsubbSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubb_mr);
}

void MacroAssemblerX86::vpsubwSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubw_mr);
}

void MacroAssemblerX86::vpsubdSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubd_mr);
}

void MacroAssemblerX86::vpsubqSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubq_mr);
}

void MacroAssemblerX86::vpmullwSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmullw_mr);
}

void MacroAssemblerX86::vpmulldSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmulld_mr);
}

void MacroAssemblerX86::vpaddsbSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddsb_mr);
}

void MacroAssemblerX86::vpaddusbSimd128(const SimdConstant& v,
                                        FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddusb_mr);
}

void MacroAssemblerX86::vpaddswSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddsw_mr);
}

void MacroAssemblerX86::vpadduswSimd128(const SimdConstant& v,
                                        FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpaddusw_mr);
}

void MacroAssemblerX86::vpsubsbSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubsb_mr);
}

void MacroAssemblerX86::vpsubusbSimd128(const SimdConstant& v,
                                        FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubusb_mr);
}

void MacroAssemblerX86::vpsubswSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubsw_mr);
}

void MacroAssemblerX86::vpsubuswSimd128(const SimdConstant& v,
                                        FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpsubusw_mr);
}

void MacroAssemblerX86::vpminsbSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminsb_mr);
}

void MacroAssemblerX86::vpminubSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminub_mr);
}

void MacroAssemblerX86::vpminswSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminsw_mr);
}

void MacroAssemblerX86::vpminuwSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminuw_mr);
}

void MacroAssemblerX86::vpminsdSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminsd_mr);
}

void MacroAssemblerX86::vpminudSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpminud_mr);
}

void MacroAssemblerX86::vpmaxsbSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxsb_mr);
}

void MacroAssemblerX86::vpmaxubSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxub_mr);
}

void MacroAssemblerX86::vpmaxswSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxsw_mr);
}

void MacroAssemblerX86::vpmaxuwSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxuw_mr);
}

void MacroAssemblerX86::vpmaxsdSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxsd_mr);
}

void MacroAssemblerX86::vpmaxudSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaxud_mr);
}

void MacroAssemblerX86::vpandSimd128(const SimdConstant& v,
                                     FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpand_mr);
}

void MacroAssemblerX86::vpxorSimd128(const SimdConstant& v,
                                     FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpxor_mr);
}

void MacroAssemblerX86::vporSimd128(const SimdConstant& v,
                                    FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpor_mr);
}

void MacroAssemblerX86::vaddpsSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vaddps_mr);
}

void MacroAssemblerX86::vaddpdSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vaddpd_mr);
}

void MacroAssemblerX86::vsubpsSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vsubps_mr);
}

void MacroAssemblerX86::vsubpdSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vsubpd_mr);
}

void MacroAssemblerX86::vdivpsSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vdivps_mr);
}

void MacroAssemblerX86::vdivpdSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vdivpd_mr);
}

void MacroAssemblerX86::vmulpsSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vmulps_mr);
}

void MacroAssemblerX86::vmulpdSimd128(const SimdConstant& v,
                                      FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vmulpd_mr);
}

void MacroAssemblerX86::vpacksswbSimd128(const SimdConstant& v,
                                         FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpacksswb_mr);
}

void MacroAssemblerX86::vpackuswbSimd128(const SimdConstant& v,
                                         FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpackuswb_mr);
}

void MacroAssemblerX86::vpackssdwSimd128(const SimdConstant& v,
                                         FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpackssdw_mr);
}

void MacroAssemblerX86::vpackusdwSimd128(const SimdConstant& v,
                                         FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpackusdw_mr);
}

void MacroAssemblerX86::vpshufbSimd128(const SimdConstant& v,
                                       FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpshufb_mr);
}

void MacroAssemblerX86::vptestSimd128(const SimdConstant& v,
                                      FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vptest_mr);
}

void MacroAssemblerX86::vpmaddwdSimd128(const SimdConstant& v,
                                        FloatRegister srcDest) {
  vpPatchOpSimd128(v, srcDest, &X86Encoding::BaseAssemblerX86::vpmaddwd_mr);
}

void MacroAssemblerX86::vpcmpeqbSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpeqb_mr);
}

void MacroAssemblerX86::vpcmpgtbSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpgtb_mr);
}

void MacroAssemblerX86::vpcmpeqwSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpeqw_mr);
}

void MacroAssemblerX86::vpcmpgtwSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpgtw_mr);
}

void MacroAssemblerX86::vpcmpeqdSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpeqd_mr);
}

void MacroAssemblerX86::vpcmpgtdSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vpcmpgtd_mr);
}

void MacroAssemblerX86::vcmpeqpsSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpeqps_mr);
}

void MacroAssemblerX86::vcmpneqpsSimd128(const SimdConstant& v,
                                         FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpneqps_mr);
}

void MacroAssemblerX86::vcmpltpsSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpltps_mr);
}

void MacroAssemblerX86::vcmplepsSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpleps_mr);
}

void MacroAssemblerX86::vcmpeqpdSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpeqpd_mr);
}

void MacroAssemblerX86::vcmpneqpdSimd128(const SimdConstant& v,
                                         FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpneqpd_mr);
}

void MacroAssemblerX86::vcmpltpdSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmpltpd_mr);
}

void MacroAssemblerX86::vcmplepdSimd128(const SimdConstant& v,
                                        FloatRegister src) {
  vpPatchOpSimd128(v, src, &X86Encoding::BaseAssemblerX86::vcmplepd_mr);
}

void MacroAssemblerX86::finish() {
  // Last instruction may be an indirect jump so eagerly insert an undefined
  // instruction byte to prevent processors from decoding data values into
  // their pipelines. See Intel performance guides.
  masm.ud2();

  if (!doubles_.empty()) {
    masm.haltingAlign(sizeof(double));
  }
  for (const Double& d : doubles_) {
    CodeOffset cst(masm.currentOffset());
    for (CodeOffset use : d.uses) {
      addCodeLabel(CodeLabel(use, cst));
    }
    masm.doubleConstant(d.value);
    if (!enoughMemory_) {
      return;
    }
  }

  if (!floats_.empty()) {
    masm.haltingAlign(sizeof(float));
  }
  for (const Float& f : floats_) {
    CodeOffset cst(masm.currentOffset());
    for (CodeOffset use : f.uses) {
      addCodeLabel(CodeLabel(use, cst));
    }
    masm.floatConstant(f.value);
    if (!enoughMemory_) {
      return;
    }
  }

  // SIMD memory values must be suitably aligned.
  if (!simds_.empty()) {
    masm.haltingAlign(SimdMemoryAlignment);
  }
  for (const SimdData& v : simds_) {
    CodeOffset cst(masm.currentOffset());
    for (CodeOffset use : v.uses) {
      addCodeLabel(CodeLabel(use, cst));
    }
    masm.simd128Constant(v.value.bytes());
    if (!enoughMemory_) {
      return;
    }
  }
}

void MacroAssemblerX86::handleFailureWithHandlerTail(Label* profilerExitTail) {
  // Reserve space for exception information.
  subl(Imm32(sizeof(ResumeFromException)), esp);
  movl(esp, eax);

  // Call the handler.
  using Fn = void (*)(ResumeFromException * rfe);
  asMasm().setupUnalignedABICall(ecx);
  asMasm().passABIArg(eax);
  asMasm().callWithABI<Fn, HandleException>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label return_;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  loadPtr(Address(esp, offsetof(ResumeFromException, kind)), eax);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                    &entryFrame);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_CATCH), &catch_);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_FINALLY), &finally);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_WASM), &wasm);
  asMasm().branch32(Assembler::Equal, eax,
                    Imm32(ResumeFromException::RESUME_WASM_CATCH), &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, load the new stack pointer
  // and return from the entry frame.
  bind(&entryFrame);
  asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
  ret();

  // If we found a catch handler, this must be a baseline frame. Restore state
  // and jump to the catch block.
  bind(&catch_);
  loadPtr(Address(esp, offsetof(ResumeFromException, target)), eax);
  loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
  jmp(Operand(eax));

  // If we found a finally block, this must be a baseline frame. Push
  // two values expected by JSOp::Retsub: BooleanValue(true) and the
  // exception.
  bind(&finally);
  ValueOperand exception = ValueOperand(ecx, edx);
  loadValue(Address(esp, offsetof(ResumeFromException, exception)), exception);

  loadPtr(Address(esp, offsetof(ResumeFromException, target)), eax);
  loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);

  pushValue(BooleanValue(true));
  pushValue(exception);
  jmp(Operand(eax));

  // Only used in debug mode. Return BaselineFrame->returnValue() to the caller.
  bind(&return_);
  loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
  loadValue(Address(ebp, BaselineFrame::reverseOffsetOfReturnValue()),
            JSReturnOperand);
  movl(ebp, esp);
  pop(ebp);

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning.
  {
    Label skipProfilingInstrumentation;
    // Test if profiler enabled.
    AbsoluteAddress addressOfEnabled(
        GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  ret();

  // If we are bailing out to baseline to handle an exception, jump to the
  // bailout tail stub. Load 1 (true) in ReturnReg to indicate success.
  bind(&bailout);
  loadPtr(Address(esp, offsetof(ResumeFromException, bailoutInfo)), ecx);
  move32(Imm32(1), ReturnReg);
  jmp(Operand(esp, offsetof(ResumeFromException, target)));

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
  masm.ret();

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  loadPtr(Address(esp, offsetof(ResumeFromException, target)), eax);
  loadPtr(Address(esp, offsetof(ResumeFromException, framePointer)), ebp);
  loadPtr(Address(esp, offsetof(ResumeFromException, stackPointer)), esp);
  jmp(Operand(eax));
}

void MacroAssemblerX86::profilerEnterFrame(Register framePtr,
                                           Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerX86::profilerExitFrame() {
  jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

MacroAssembler& MacroAssemblerX86::asMasm() {
  return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler& MacroAssemblerX86::asMasm() const {
  return *static_cast<const MacroAssembler*>(this);
}

void MacroAssembler::subFromStackPtr(Imm32 imm32) {
  if (imm32.value) {
    // On windows, we cannot skip very far down the stack without touching the
    // memory pages in-between.  This is a corner-case code for situations where
    // the Ion frame data for a piece of code is very large.  To handle this
    // special case, for frames over 4k in size we allocate memory on the stack
    // incrementally, touching it as we go.
    //
    // When the amount is quite large, which it can be, we emit an actual loop,
    // in order to keep the function prologue compact.  Compactness is a
    // requirement for eg Wasm's CodeRange data structure, which can encode only
    // 8-bit offsets.
    uint32_t amountLeft = imm32.value;
    uint32_t fullPages = amountLeft / 4096;
    if (fullPages <= 8) {
      while (amountLeft > 4096) {
        subl(Imm32(4096), StackPointer);
        store32(Imm32(0), Address(StackPointer, 0));
        amountLeft -= 4096;
      }
      subl(Imm32(amountLeft), StackPointer);
    } else {
      // Save scratch register.
      push(eax);
      amountLeft -= 4;
      fullPages = amountLeft / 4096;

      Label top;
      move32(Imm32(fullPages), eax);
      bind(&top);
      subl(Imm32(4096), StackPointer);
      store32(Imm32(0), Address(StackPointer, 0));
      subl(Imm32(1), eax);
      j(Assembler::NonZero, &top);
      amountLeft -= fullPages * 4096;
      if (amountLeft) {
        subl(Imm32(amountLeft), StackPointer);
      }

      // Restore scratch register.
      movl(Operand(StackPointer, uint32_t(imm32.value) - 4), eax);
    }
  }
}

//{{{ check_macroassembler_style
// ===============================================================
// ABI function calls.

void MacroAssembler::setupUnalignedABICall(Register scratch) {
  setupNativeABICall();
  dynamicAlignment_ = true;

  movl(esp, scratch);
  andl(Imm32(~(ABIStackAlignment - 1)), esp);
  push(scratch);
}

void MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm) {
  MOZ_ASSERT(inCall_);
  uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

  if (dynamicAlignment_) {
    // sizeof(intptr_t) accounts for the saved stack pointer pushed by
    // setupUnalignedABICall.
    stackForCall += ComputeByteAlignment(stackForCall + sizeof(intptr_t),
                                         ABIStackAlignment);
  } else {
    uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
    stackForCall += ComputeByteAlignment(
        stackForCall + framePushed() + alignmentAtPrologue, ABIStackAlignment);
  }

  *stackAdjust = stackForCall;
  reserveStack(stackForCall);

  // Position all arguments.
  {
    enoughMemory_ &= moveResolver_.resolve();
    if (!enoughMemory_) {
      return;
    }

    MoveEmitter emitter(*this);
    emitter.emit(moveResolver_);
    emitter.finish();
  }

  assertStackAlignment(ABIStackAlignment);
}

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result,
                                     bool callFromWasm) {
  freeStack(stackAdjust);

  // Calls to native functions in wasm pass through a thunk which already
  // fixes up the return value for us.
  if (!callFromWasm) {
    if (result == MoveOp::DOUBLE) {
      reserveStack(sizeof(double));
      fstp(Operand(esp, 0));
      loadDouble(Operand(esp, 0), ReturnDoubleReg);
      freeStack(sizeof(double));
    } else if (result == MoveOp::FLOAT32) {
      reserveStack(sizeof(float));
      fstp32(Operand(esp, 0));
      loadFloat32(Operand(esp, 0), ReturnFloat32Reg);
      freeStack(sizeof(float));
    }
  }

  if (dynamicAlignment_) {
    pop(esp);
  }

#ifdef DEBUG
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif
}

void MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result) {
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(fun);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun,
                                           MoveOp::Type result) {
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(fun);
  callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Move instructions

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    if (reg.gpr() != dest.payloadReg()) {
      movl(reg.gpr(), dest.payloadReg());
    }
    mov(ImmWord(MIRTypeToTag(type)), dest.typeReg());
    return;
  }

  ScratchDoubleScope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, scratch);
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  Register s0 = src.typeReg();
  Register s1 = src.payloadReg();
  Register d0 = dest.typeReg();
  Register d1 = dest.payloadReg();

  // Either one or both of the source registers could be the same as a
  // destination register.
  if (s1 == d0) {
    if (s0 == d1) {
      // If both are, this is just a swap of two registers.
      xchgl(d0, d1);
      return;
    }
    // If only one is, copy that source first.
    std::swap(s0, s1);
    std::swap(d0, d1);
  }

  if (s0 != d0) {
    movl(s0, d0);
  }
  if (s1 != d1) {
    movl(s1, d1);
  }
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  movl(Imm32(src.toNunboxTag()), dest.typeReg());
  if (src.isGCThing()) {
    movl(ImmGCPtr(src.toGCThing()), dest.payloadReg());
  } else {
    movl(Imm32(src.toNunboxPayload()), dest.payloadReg());
  }
}

// ===============================================================
// Branch functions

void MacroAssembler::loadStoreBuffer(Register ptr, Register buffer) {
  if (ptr != buffer) {
    movePtr(ptr, buffer);
  }
  orPtr(Imm32(gc::ChunkMask), buffer);
  loadPtr(Address(buffer, gc::ChunkStoreBufferOffsetFromLastByte), buffer);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  MOZ_ASSERT(temp != InvalidReg);  // A temp register is required for x86.
  MOZ_ASSERT(ptr != temp);
  movePtr(ptr, temp);
  branchPtrInNurseryChunkImpl(cond, temp, label);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond,
                                             const Address& address,
                                             Register temp, Label* label) {
  MOZ_ASSERT(temp != InvalidReg);  // A temp register is required for x86.
  loadPtr(address, temp);
  branchPtrInNurseryChunkImpl(cond, temp, label);
}

void MacroAssembler::branchPtrInNurseryChunkImpl(Condition cond, Register ptr,
                                                 Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  orPtr(Imm32(gc::ChunkMask), ptr);
  branchPtr(InvertCondition(cond),
            Address(ptr, gc::ChunkStoreBufferOffsetFromLastByte), ImmWord(0),
            label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, address,
                    cond == Assembler::Equal ? &done : label);
  branchPtrInNurseryChunk(cond, ToPayload(address), temp, label);

  bind(&done);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);
  branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);

  bind(&done);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  if (rhs.isGCThing()) {
    cmpPtr(lhs.payloadReg(), ImmGCPtr(rhs.toGCThing()));
  } else {
    cmpPtr(lhs.payloadReg(), ImmWord(rhs.toNunboxPayload()));
  }

  if (cond == Equal) {
    Label done;
    j(NotEqual, &done);
    {
      cmp32(lhs.typeReg(), Imm32(rhs.toNunboxTag()));
      j(Equal, label);
    }
    bind(&done);
  } else {
    j(NotEqual, label);

    cmp32(lhs.typeReg(), Imm32(rhs.toNunboxTag()));
    j(NotEqual, label);
  }
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest,
                                       MIRType slotType) {
  if (valueType == MIRType::Double) {
    storeDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  // Store the type tag if needed.
  if (valueType != slotType) {
    storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), Operand(dest));
  }

  // Store the payload.
  if (value.constant()) {
    storePayload(value.value(), Operand(dest));
  } else {
    storePayload(value.reg().typedReg().gpr(), Operand(dest));
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest,
                                                MIRType slotType);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest, MIRType slotType);

// wasm specific methods, used in both the wasm baseline compiler and ion.

void MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access,
                              Operand srcAddr, AnyRegister out) {
  MOZ_ASSERT(srcAddr.kind() == Operand::MEM_REG_DISP ||
             srcAddr.kind() == Operand::MEM_SCALE);

  MOZ_ASSERT_IF(
      access.isZeroExtendSimd128Load(),
      access.type() == Scalar::Float32 || access.type() == Scalar::Float64);
  MOZ_ASSERT_IF(access.isSplatSimd128Load(), access.type() == Scalar::Float64);
  MOZ_ASSERT_IF(access.isWidenSimd128Load(), access.type() == Scalar::Float64);

  memoryBarrierBefore(access.sync());

  append(access, size());
  switch (access.type()) {
    case Scalar::Int8:
      movsbl(srcAddr, out.gpr());
      break;
    case Scalar::Uint8:
      movzbl(srcAddr, out.gpr());
      break;
    case Scalar::Int16:
      movswl(srcAddr, out.gpr());
      break;
    case Scalar::Uint16:
      movzwl(srcAddr, out.gpr());
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      movl(srcAddr, out.gpr());
      break;
    case Scalar::Float32:
      // vmovss does the right thing also for access.isZeroExtendSimd128Load()
      vmovss(srcAddr, out.fpu());
      break;
    case Scalar::Float64:
      if (access.isSplatSimd128Load()) {
        vmovddup(srcAddr, out.fpu());
      } else if (access.isWidenSimd128Load()) {
        switch (access.widenSimdOp()) {
          case wasm::SimdOp::I16x8LoadS8x8:
            vpmovsxbw(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::I16x8LoadU8x8:
            vpmovzxbw(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::I32x4LoadS16x4:
            vpmovsxwd(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::I32x4LoadU16x4:
            vpmovzxwd(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::I64x2LoadS32x2:
            vpmovsxdq(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::I64x2LoadU32x2:
            vpmovzxdq(srcAddr, out.fpu());
            break;
          default:
            MOZ_CRASH("Unexpected widening op for wasmLoad");
        }
      } else {
        // vmovsd does the right thing also for access.isZeroExtendSimd128Load()
        vmovsd(srcAddr, out.fpu());
      }
      break;
    case Scalar::Simd128:
      vmovups(srcAddr, out.fpu());
      break;
    case Scalar::Int64:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected type");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access,
                                 Operand srcAddr, Register64 out) {
  // Atomic i64 load must use lock_cmpxchg8b.
  MOZ_ASSERT_IF(access.isAtomic(), access.byteSize() <= 4);
  MOZ_ASSERT(srcAddr.kind() == Operand::MEM_REG_DISP ||
             srcAddr.kind() == Operand::MEM_SCALE);
  MOZ_ASSERT(!access.isZeroExtendSimd128Load());  // Use wasmLoad()
  MOZ_ASSERT(!access.isSplatSimd128Load());       // Use wasmLoad()
  MOZ_ASSERT(!access.isWidenSimd128Load());       // Use wasmLoad()

  memoryBarrierBefore(access.sync());

  append(access, size());
  switch (access.type()) {
    case Scalar::Int8:
      MOZ_ASSERT(out == Register64(edx, eax));
      movsbl(srcAddr, out.low);

      cdq();
      break;
    case Scalar::Uint8:
      movzbl(srcAddr, out.low);

      xorl(out.high, out.high);
      break;
    case Scalar::Int16:
      MOZ_ASSERT(out == Register64(edx, eax));
      movswl(srcAddr, out.low);

      cdq();
      break;
    case Scalar::Uint16:
      movzwl(srcAddr, out.low);

      xorl(out.high, out.high);
      break;
    case Scalar::Int32:
      MOZ_ASSERT(out == Register64(edx, eax));
      movl(srcAddr, out.low);

      cdq();
      break;
    case Scalar::Uint32:
      movl(srcAddr, out.low);

      xorl(out.high, out.high);
      break;
    case Scalar::Int64: {
      if (srcAddr.kind() == Operand::MEM_SCALE) {
        MOZ_RELEASE_ASSERT(srcAddr.toBaseIndex().base != out.low &&
                           srcAddr.toBaseIndex().index != out.low);
      }
      if (srcAddr.kind() == Operand::MEM_REG_DISP) {
        MOZ_RELEASE_ASSERT(srcAddr.toAddress().base != out.low);
      }

      movl(LowWord(srcAddr), out.low);

      append(access, size());
      movl(HighWord(srcAddr), out.high);

      break;
    }
    case Scalar::Float32:
    case Scalar::Float64:
      MOZ_CRASH("non-int64 loads should use load()");
    case Scalar::Simd128:
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access,
                               AnyRegister value, Operand dstAddr) {
  MOZ_ASSERT(dstAddr.kind() == Operand::MEM_REG_DISP ||
             dstAddr.kind() == Operand::MEM_SCALE);

  memoryBarrierBefore(access.sync());

  append(access, size());
  switch (access.type()) {
    case Scalar::Int8:
    case Scalar::Uint8Clamped:
    case Scalar::Uint8:
      movb(value.gpr(), dstAddr);
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
      movw(value.gpr(), dstAddr);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      movl(value.gpr(), dstAddr);
      break;
    case Scalar::Float32:
      vmovss(value.fpu(), dstAddr);
      break;
    case Scalar::Float64:
      vmovsd(value.fpu(), dstAddr);
      break;
    case Scalar::Simd128:
      vmovups(value.fpu(), dstAddr);
      break;
    case Scalar::Int64:
      MOZ_CRASH("Should be handled in storeI64.");
    case Scalar::MaxTypedArrayViewType:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      MOZ_CRASH("unexpected type");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access,
                                  Register64 value, Operand dstAddr) {
  // Atomic i64 store must use lock_cmpxchg8b.
  MOZ_ASSERT(!access.isAtomic());
  MOZ_ASSERT(dstAddr.kind() == Operand::MEM_REG_DISP ||
             dstAddr.kind() == Operand::MEM_SCALE);

  // Store the high word first so as to hit guard-page-based OOB checks without
  // writing partial data.
  append(access, size());
  movl(value.high, HighWord(dstAddr));

  append(access, size());
  movl(value.low, LowWord(dstAddr));
}

template <typename T>
static void AtomicLoad64(MacroAssembler& masm,
                         const wasm::MemoryAccessDesc* access, const T& address,
                         Register64 temp, Register64 output) {
  MOZ_ASSERT(temp.low == ebx);
  MOZ_ASSERT(temp.high == ecx);
  MOZ_ASSERT(output.high == edx);
  MOZ_ASSERT(output.low == eax);

  // In the event edx:eax matches what's in memory, ecx:ebx will be
  // stored.  The two pairs must therefore have the same values.
  masm.movl(edx, ecx);
  masm.movl(eax, ebx);

  if (access) {
    masm.append(*access, masm.size());
  }
  masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(address));
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const Address& mem, Register64 temp,
                                      Register64 output) {
  AtomicLoad64(*this, &access, mem, temp, output);
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const BaseIndex& mem, Register64 temp,
                                      Register64 output) {
  AtomicLoad64(*this, &access, mem, temp, output);
}

template <typename T>
static void CompareExchange64(MacroAssembler& masm,
                              const wasm::MemoryAccessDesc* access,
                              const T& mem, Register64 expected,
                              Register64 replacement, Register64 output) {
  MOZ_ASSERT(expected == output);
  MOZ_ASSERT(expected.high == edx);
  MOZ_ASSERT(expected.low == eax);
  MOZ_ASSERT(replacement.high == ecx);
  MOZ_ASSERT(replacement.low == ebx);

  if (access) {
    masm.append(*access, masm.size());
  }
  masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem));
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  CompareExchange64(*this, &access, mem, expected, replacement, output);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  CompareExchange64(*this, &access, mem, expected, replacement, output);
}

template <typename T>
static void AtomicExchange64(MacroAssembler& masm,
                             const wasm::MemoryAccessDesc* access, const T& mem,
                             Register64 value, Register64 output) {
  MOZ_ASSERT(value.low == ebx);
  MOZ_ASSERT(value.high == ecx);
  MOZ_ASSERT(output.high == edx);
  MOZ_ASSERT(output.low == eax);

  // edx:eax has garbage initially, and that is the best we can do unless
  // we can guess with high probability what's in memory.

  MOZ_ASSERT(mem.base != edx && mem.base != eax);
  if constexpr (std::is_same_v<T, BaseIndex>) {
    MOZ_ASSERT(mem.index != edx && mem.index != eax);
  } else {
    static_assert(std::is_same_v<T, Address>);
  }

  Label again;
  masm.bind(&again);
  if (access) {
    masm.append(*access, masm.size());
  }
  masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem));
  masm.j(MacroAssembler::NonZero, &again);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 value,
                                          Register64 output) {
  AtomicExchange64(*this, &access, mem, value, output);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem,
                                          Register64 value, Register64 output) {
  AtomicExchange64(*this, &access, mem, value, output);
}

template <typename T>
static void AtomicFetchOp64(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access, AtomicOp op,
                            const Address& value, const T& mem, Register64 temp,
                            Register64 output) {
  // We don't have enough registers for all the operands on x86, so the rhs
  // operand is in memory.

#define ATOMIC_OP_BODY(OPERATE)                            \
  do {                                                     \
    MOZ_ASSERT(output.low == eax);                         \
    MOZ_ASSERT(output.high == edx);                        \
    MOZ_ASSERT(temp.low == ebx);                           \
    MOZ_ASSERT(temp.high == ecx);                          \
    if (access) {                                          \
      masm.append(*access, masm.size());                   \
    }                                                      \
    masm.load64(mem, output);                              \
    Label again;                                           \
    masm.bind(&again);                                     \
    masm.move64(output, temp);                             \
    masm.OPERATE(Operand(value), temp);                    \
    masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(mem)); \
    masm.j(MacroAssembler::NonZero, &again);               \
  } while (0)

  switch (op) {
    case AtomicFetchAddOp:
      ATOMIC_OP_BODY(add64FromMemory);
      break;
    case AtomicFetchSubOp:
      ATOMIC_OP_BODY(sub64FromMemory);
      break;
    case AtomicFetchAndOp:
      ATOMIC_OP_BODY(and64FromMemory);
      break;
    case AtomicFetchOrOp:
      ATOMIC_OP_BODY(or64FromMemory);
      break;
    case AtomicFetchXorOp:
      ATOMIC_OP_BODY(xor64FromMemory);
      break;
    default:
      MOZ_CRASH();
  }

#undef ATOMIC_OP_BODY
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, const Address& value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, const Address& value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, op, value, mem, temp, output);
}

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  Label done;
  vcvttsd2si(input, output);
  branch32(Assembler::Condition::NotSigned, output, Imm32(0), &done);

  ScratchDoubleScope fpscratch(*this);
  loadConstantDouble(double(int32_t(0x80000000)), fpscratch);
  addDouble(input, fpscratch);
  vcvttsd2si(fpscratch, output);

  branch32(Assembler::Condition::Signed, output, Imm32(0), oolEntry);
  or32(Imm32(0x80000000), output);

  bind(&done);
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  Label done;
  vcvttss2si(input, output);
  branch32(Assembler::Condition::NotSigned, output, Imm32(0), &done);

  ScratchFloat32Scope fpscratch(*this);
  loadConstantFloat32(float(int32_t(0x80000000)), fpscratch);
  addFloat32(input, fpscratch);
  vcvttss2si(fpscratch, output);

  branch32(Assembler::Condition::Signed, output, Imm32(0), oolEntry);
  or32(Imm32(0x80000000), output);

  bind(&done);
}

void MacroAssembler::wasmTruncateDoubleToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  Label ok;
  Register temp = output.high;

  reserveStack(2 * sizeof(int32_t));
  storeDouble(input, Operand(esp, 0));

  truncateDoubleToInt64(Address(esp, 0), Address(esp, 0), temp);
  load64(Address(esp, 0), output);

  cmpl(Imm32(0), Operand(esp, 0));
  j(Assembler::NotEqual, &ok);

  cmpl(Imm32(1), Operand(esp, 4));
  j(Assembler::Overflow, oolEntry);

  bind(&ok);
  bind(oolRejoin);

  freeStack(2 * sizeof(int32_t));
}

void MacroAssembler::wasmTruncateFloat32ToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  Label ok;
  Register temp = output.high;

  reserveStack(2 * sizeof(int32_t));
  storeFloat32(input, Operand(esp, 0));

  truncateFloat32ToInt64(Address(esp, 0), Address(esp, 0), temp);
  load64(Address(esp, 0), output);

  cmpl(Imm32(0), Operand(esp, 0));
  j(Assembler::NotEqual, &ok);

  cmpl(Imm32(1), Operand(esp, 4));
  j(Assembler::Overflow, oolEntry);

  bind(&ok);
  bind(oolRejoin);

  freeStack(2 * sizeof(int32_t));
}

void MacroAssembler::wasmTruncateDoubleToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  Label fail, convert;
  Register temp = output.high;

  // Make sure input fits in uint64.
  reserveStack(2 * sizeof(int32_t));
  storeDouble(input, Operand(esp, 0));
  branchDoubleNotInUInt64Range(Address(esp, 0), temp, &fail);
  size_t stackBeforeBranch = framePushed();
  jump(&convert);

  bind(&fail);
  freeStack(2 * sizeof(int32_t));
  jump(oolEntry);
  if (isSaturating) {
    // The OOL path computes the right values.
    setFramePushed(stackBeforeBranch);
  } else {
    // The OOL path just checks the input values.
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeDouble(input, Operand(esp, 0));
  }

  // Convert the double/float to uint64.
  bind(&convert);
  truncateDoubleToUInt64(Address(esp, 0), Address(esp, 0), temp, tempReg);

  // Load value into int64 register.
  load64(Address(esp, 0), output);
  freeStack(2 * sizeof(int32_t));

  if (isSaturating) {
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateFloat32ToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  Label fail, convert;
  Register temp = output.high;

  // Make sure input fits in uint64.
  reserveStack(2 * sizeof(int32_t));
  storeFloat32(input, Operand(esp, 0));
  branchFloat32NotInUInt64Range(Address(esp, 0), temp, &fail);
  size_t stackBeforeBranch = framePushed();
  jump(&convert);

  bind(&fail);
  freeStack(2 * sizeof(int32_t));
  jump(oolEntry);
  if (isSaturating) {
    // The OOL path computes the right values.
    setFramePushed(stackBeforeBranch);
  } else {
    // The OOL path just checks the input values.
    bind(oolRejoin);
    reserveStack(2 * sizeof(int32_t));
    storeFloat32(input, Operand(esp, 0));
  }

  // Convert the float to uint64.
  bind(&convert);
  truncateFloat32ToUInt64(Address(esp, 0), Address(esp, 0), temp, tempReg);

  // Load value into int64 register.
  load64(Address(esp, 0), output);
  freeStack(2 * sizeof(int32_t));

  if (isSaturating) {
    bind(oolRejoin);
  }
}

// ========================================================================
// Primitive atomic operations.

void MacroAssembler::atomicLoad64(const Synchronization&, const Address& mem,
                                  Register64 temp, Register64 output) {
  AtomicLoad64(*this, nullptr, mem, temp, output);
}

void MacroAssembler::atomicLoad64(const Synchronization&, const BaseIndex& mem,
                                  Register64 temp, Register64 output) {
  AtomicLoad64(*this, nullptr, mem, temp, output);
}

void MacroAssembler::atomicStore64(const Synchronization&, const Address& mem,
                                   Register64 value, Register64 temp) {
  AtomicExchange64(*this, nullptr, mem, value, temp);
}

void MacroAssembler::atomicStore64(const Synchronization&, const BaseIndex& mem,
                                   Register64 value, Register64 temp) {
  AtomicExchange64(*this, nullptr, mem, value, temp);
}

void MacroAssembler::compareExchange64(const Synchronization&,
                                       const Address& mem, Register64 expected,
                                       Register64 replacement,
                                       Register64 output) {
  CompareExchange64(*this, nullptr, mem, expected, replacement, output);
}

void MacroAssembler::compareExchange64(const Synchronization&,
                                       const BaseIndex& mem,
                                       Register64 expected,
                                       Register64 replacement,
                                       Register64 output) {
  CompareExchange64(*this, nullptr, mem, expected, replacement, output);
}

void MacroAssembler::atomicExchange64(const Synchronization&,
                                      const Address& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, mem, value, output);
}

void MacroAssembler::atomicExchange64(const Synchronization&,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, mem, value, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization&, AtomicOp op,
                                     const Address& value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization&, AtomicOp op,
                                     const Address& value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, op, value, mem, temp, output);
}

// ========================================================================
// Convert floating point.

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return HasSSE3(); }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  // SUBPD needs SSE2, HADDPD needs SSE3.
  if (!HasSSE3()) {
    MOZ_ASSERT(temp == Register::Invalid());

    // Zero the dest register to break dependencies, see convertInt32ToDouble.
    zeroDouble(dest);

    Push(src.high);
    Push(src.low);
    fild(Operand(esp, 0));

    Label notNegative;
    branch32(Assembler::NotSigned, src.high, Imm32(0), &notNegative);
    double add_constant = 18446744073709551616.0;  // 2^64
    store64(Imm64(mozilla::BitwiseCast<uint64_t>(add_constant)),
            Address(esp, 0));
    fld(Operand(esp, 0));
    faddp();
    bind(&notNegative);

    fstp(Operand(esp, 0));
    vmovsd(Address(esp, 0), dest);
    freeStack(2 * sizeof(intptr_t));
    return;
  }

  ScratchSimd128Scope scratch(*this);

  // Following operation uses entire 128-bit of dest XMM register.
  // Currently higher 64-bit is free when we have access to lower 64-bit.
  MOZ_ASSERT(dest.size() == 8);
  FloatRegister dest128 =
      FloatRegister(dest.encoding(), FloatRegisters::Simd128);

  // Assume that src is represented as following:
  //   src      = 0x HHHHHHHH LLLLLLLL

  // Move src to dest (=dest128) and ScratchInt32x4Reg (=scratch):
  //   dest     = 0x 00000000 00000000  00000000 LLLLLLLL
  //   scratch  = 0x 00000000 00000000  00000000 HHHHHHHH
  vmovd(src.low, dest128);
  vmovd(src.high, scratch);

  // Unpack and interleave dest and scratch to dest:
  //   dest     = 0x 00000000 00000000  HHHHHHHH LLLLLLLL
  vpunpckldq(scratch, dest128, dest128);

  // Unpack and interleave dest and a constant C1 to dest:
  //   C1       = 0x 00000000 00000000  45300000 43300000
  //   dest     = 0x 45300000 HHHHHHHH  43300000 LLLLLLLL
  // here, each 64-bit part of dest represents following double:
  //   HI(dest) = 0x 1.00000HHHHHHHH * 2**84 == 2**84 + 0x HHHHHHHH 00000000
  //   LO(dest) = 0x 1.00000LLLLLLLL * 2**52 == 2**52 + 0x 00000000 LLLLLLLL
  // See convertUInt64ToDouble for the details.
  static const int32_t CST1[4] = {
      0x43300000,
      0x45300000,
      0x0,
      0x0,
  };

  loadConstantSimd128Int(SimdConstant::CreateX4(CST1), scratch);
  vpunpckldq(scratch, dest128, dest128);

  // Subtract a constant C2 from dest, for each 64-bit part:
  //   C2       = 0x 45300000 00000000  43300000 00000000
  // here, each 64-bit part of C2 represents following double:
  //   HI(C2)   = 0x 1.0000000000000 * 2**84 == 2**84
  //   LO(C2)   = 0x 1.0000000000000 * 2**52 == 2**52
  // after the operation each 64-bit part of dest represents following:
  //   HI(dest) = double(0x HHHHHHHH 00000000)
  //   LO(dest) = double(0x 00000000 LLLLLLLL)
  static const int32_t CST2[4] = {
      0x0,
      0x43300000,
      0x0,
      0x45300000,
  };

  loadConstantSimd128Int(SimdConstant::CreateX4(CST2), scratch);
  vsubpd(Operand(scratch), dest128, dest128);

  // Add HI(dest) and LO(dest) in double and store it into LO(dest),
  //   LO(dest) = double(0x HHHHHHHH 00000000) + double(0x 00000000 LLLLLLLL)
  //            = double(0x HHHHHHHH LLLLLLLL)
  //            = double(src)
  vhaddpd(dest128, dest128);
}

void MacroAssembler::convertInt64ToDouble(Register64 input,
                                          FloatRegister output) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroDouble(output);

  Push(input.high);
  Push(input.low);
  fild(Operand(esp, 0));

  fstp(Operand(esp, 0));
  vmovsd(Address(esp, 0), output);
  freeStack(2 * sizeof(intptr_t));
}

void MacroAssembler::convertUInt64ToFloat32(Register64 input,
                                            FloatRegister output,
                                            Register temp) {
  // Zero the dest register to break dependencies, see convertInt32ToDouble.
  zeroDouble(output);

  // Set the FPU precision to 80 bits.
  reserveStack(2 * sizeof(intptr_t));
  fnstcw(Operand(esp, 0));
  load32(Operand(esp, 0), temp);
  orl(Imm32(0x300), temp);
  store32(temp, Operand(esp, sizeof(intptr_t)));
  fldcw(Operand(esp, sizeof(intptr_t)));

  Push(input.high);
  Push(input.low);
  fild(Operand(esp, 0));

  Label notNegative;
  branch32(Assembler::NotSigned, input.high, Imm32(0), &notNegative);
  double add_constant = 18446744073709551616.0;  // 2^64
  uint64_t add_constant_u64 = mozilla::BitwiseCast<uint64_t>(add_constant);
  store64(Imm64(add_constant_u64), Address(esp, 0));

  fld(Operand(esp, 0));
  faddp();
  bind(&notNegative);

  fstp32(Operand(esp, 0));
  vmovss(Address(esp, 0), output);
  freeStack(2 * sizeof(intptr_t));

  // Restore FPU precision to the initial value.
  fldcw(Operand(esp, 0));
  freeStack(2 * sizeof(intptr_t));
}

void MacroAssembler::convertInt64ToFloat32(Register64 input,
                                           FloatRegister output) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroDouble(output);

  Push(input.high);
  Push(input.low);
  fild(Operand(esp, 0));

  fstp32(Operand(esp, 0));
  vmovss(Address(esp, 0), output);
  freeStack(2 * sizeof(intptr_t));
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt32ToDouble(src, dest);
}

void MacroAssembler::PushBoxed(FloatRegister reg) { Push(reg); }

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest) {
  return movWithPatch(ImmPtr(nullptr), dest);
}

void MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                          CodeLocationLabel target) {
  PatchDataWithValueCheck(loc, ImmPtr(target.raw()), ImmPtr(nullptr));
}

//}}} check_macroassembler_style
