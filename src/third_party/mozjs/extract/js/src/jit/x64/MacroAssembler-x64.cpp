/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x64/MacroAssembler-x64.h"

#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"
#include "util/Memory.h"
#include "vm/BigIntType.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"
#include "vm/StringType.h"
#include "wasm/WasmStubs.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void MacroAssemblerX64::loadConstantDouble(double d, FloatRegister dest) {
  if (maybeInlineDouble(d, dest)) {
    return;
  }
  Double* dbl = getDouble(d);
  if (!dbl) {
    return;
  }
  // The constants will be stored in a pool appended to the text (see
  // finish()), so they will always be a fixed distance from the
  // instructions which reference them. This allows the instructions to use
  // PC-relative addressing. Use "jump" label support code, because we need
  // the same PC-relative address patching that jumps use.
  JmpSrc j = masm.vmovsd_ripr(dest.encoding());
  propagateOOM(dbl->uses.append(j));
}

void MacroAssemblerX64::loadConstantFloat32(float f, FloatRegister dest) {
  if (maybeInlineFloat(f, dest)) {
    return;
  }
  Float* flt = getFloat(f);
  if (!flt) {
    return;
  }
  // See comment in loadConstantDouble
  JmpSrc j = masm.vmovss_ripr(dest.encoding());
  propagateOOM(flt->uses.append(j));
}

void MacroAssemblerX64::vpRiprOpSimd128(
    const SimdConstant& v, FloatRegister reg,
    JmpSrc (X86Encoding::BaseAssemblerX64::*op)(
        X86Encoding::XMMRegisterID id)) {
  SimdData* val = getSimdData(v);
  if (!val) {
    return;
  }
  JmpSrc j = (masm.*op)(reg.encoding());
  propagateOOM(val->uses.append(j));
}

void MacroAssemblerX64::vpRiprOpSimd128(
    const SimdConstant& v, FloatRegister src, FloatRegister dest,
    JmpSrc (X86Encoding::BaseAssemblerX64::*op)(
        X86Encoding::XMMRegisterID srcId, X86Encoding::XMMRegisterID destId)) {
  SimdData* val = getSimdData(v);
  if (!val) {
    return;
  }
  JmpSrc j = (masm.*op)(src.encoding(), dest.encoding());
  propagateOOM(val->uses.append(j));
}

void MacroAssemblerX64::loadConstantSimd128Int(const SimdConstant& v,
                                               FloatRegister dest) {
  if (maybeInlineSimd128Int(v, dest)) {
    return;
  }
  vpRiprOpSimd128(v, dest, &X86Encoding::BaseAssemblerX64::vmovdqa_ripr);
}

void MacroAssemblerX64::loadConstantSimd128Float(const SimdConstant& v,
                                                 FloatRegister dest) {
  if (maybeInlineSimd128Float(v, dest)) {
    return;
  }
  vpRiprOpSimd128(v, dest, &X86Encoding::BaseAssemblerX64::vmovaps_ripr);
}

void MacroAssemblerX64::vpaddbSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddb_ripr);
}

void MacroAssemblerX64::vpaddwSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddw_ripr);
}

void MacroAssemblerX64::vpadddSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddd_ripr);
}

void MacroAssemblerX64::vpaddqSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddq_ripr);
}

void MacroAssemblerX64::vpsubbSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubb_ripr);
}

void MacroAssemblerX64::vpsubwSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubw_ripr);
}

void MacroAssemblerX64::vpsubdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubd_ripr);
}

void MacroAssemblerX64::vpsubqSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubq_ripr);
}

void MacroAssemblerX64::vpmullwSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmullw_ripr);
}

void MacroAssemblerX64::vpmulldSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmulld_ripr);
}

void MacroAssemblerX64::vpaddsbSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddsb_ripr);
}

void MacroAssemblerX64::vpaddusbSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddusb_ripr);
}

void MacroAssemblerX64::vpaddswSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddsw_ripr);
}

void MacroAssemblerX64::vpadduswSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpaddusw_ripr);
}

void MacroAssemblerX64::vpsubsbSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubsb_ripr);
}

void MacroAssemblerX64::vpsubusbSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubusb_ripr);
}

void MacroAssemblerX64::vpsubswSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubsw_ripr);
}

void MacroAssemblerX64::vpsubuswSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpsubusw_ripr);
}

void MacroAssemblerX64::vpminsbSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminsb_ripr);
}

void MacroAssemblerX64::vpminubSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminub_ripr);
}

void MacroAssemblerX64::vpminswSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminsw_ripr);
}

void MacroAssemblerX64::vpminuwSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminuw_ripr);
}

void MacroAssemblerX64::vpminsdSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminsd_ripr);
}

void MacroAssemblerX64::vpminudSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpminud_ripr);
}

void MacroAssemblerX64::vpmaxsbSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxsb_ripr);
}

void MacroAssemblerX64::vpmaxubSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxub_ripr);
}

void MacroAssemblerX64::vpmaxswSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxsw_ripr);
}

void MacroAssemblerX64::vpmaxuwSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxuw_ripr);
}

void MacroAssemblerX64::vpmaxsdSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxsd_ripr);
}

void MacroAssemblerX64::vpmaxudSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaxud_ripr);
}

void MacroAssemblerX64::vpandSimd128(const SimdConstant& v, FloatRegister lhs,
                                     FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpand_ripr);
}

void MacroAssemblerX64::vpxorSimd128(const SimdConstant& v, FloatRegister lhs,
                                     FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpxor_ripr);
}

void MacroAssemblerX64::vporSimd128(const SimdConstant& v, FloatRegister lhs,
                                    FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpor_ripr);
}

void MacroAssemblerX64::vaddpsSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vaddps_ripr);
}

void MacroAssemblerX64::vaddpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vaddpd_ripr);
}

void MacroAssemblerX64::vsubpsSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vsubps_ripr);
}

void MacroAssemblerX64::vsubpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vsubpd_ripr);
}

void MacroAssemblerX64::vdivpsSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vdivps_ripr);
}

void MacroAssemblerX64::vdivpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vdivpd_ripr);
}

void MacroAssemblerX64::vmulpsSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vmulps_ripr);
}

void MacroAssemblerX64::vmulpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vmulpd_ripr);
}

void MacroAssemblerX64::vandpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vandpd_ripr);
}

void MacroAssemblerX64::vminpdSimd128(const SimdConstant& v, FloatRegister lhs,
                                      FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vminpd_ripr);
}

void MacroAssemblerX64::vpacksswbSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpacksswb_ripr);
}

void MacroAssemblerX64::vpackuswbSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpackuswb_ripr);
}

void MacroAssemblerX64::vpackssdwSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpackssdw_ripr);
}

void MacroAssemblerX64::vpackusdwSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpackusdw_ripr);
}

void MacroAssemblerX64::vpunpckldqSimd128(const SimdConstant& v,
                                          FloatRegister lhs,
                                          FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest,
                  &X86Encoding::BaseAssemblerX64::vpunpckldq_ripr);
}

void MacroAssemblerX64::vunpcklpsSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vunpcklps_ripr);
}

void MacroAssemblerX64::vpshufbSimd128(const SimdConstant& v, FloatRegister lhs,
                                       FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpshufb_ripr);
}

void MacroAssemblerX64::vptestSimd128(const SimdConstant& v,
                                      FloatRegister lhs) {
  vpRiprOpSimd128(v, lhs, &X86Encoding::BaseAssemblerX64::vptest_ripr);
}

void MacroAssemblerX64::vpmaddwdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmaddwd_ripr);
}

void MacroAssemblerX64::vpcmpeqbSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpeqb_ripr);
}

void MacroAssemblerX64::vpcmpgtbSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpgtb_ripr);
}

void MacroAssemblerX64::vpcmpeqwSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpeqw_ripr);
}

void MacroAssemblerX64::vpcmpgtwSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpgtw_ripr);
}

void MacroAssemblerX64::vpcmpeqdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpeqd_ripr);
}

void MacroAssemblerX64::vpcmpgtdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpcmpgtd_ripr);
}

void MacroAssemblerX64::vcmpeqpsSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpeqps_ripr);
}

void MacroAssemblerX64::vcmpneqpsSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpneqps_ripr);
}

void MacroAssemblerX64::vcmpltpsSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpltps_ripr);
}

void MacroAssemblerX64::vcmplepsSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpleps_ripr);
}

void MacroAssemblerX64::vcmpgepsSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpgeps_ripr);
}

void MacroAssemblerX64::vcmpeqpdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpeqpd_ripr);
}

void MacroAssemblerX64::vcmpneqpdSimd128(const SimdConstant& v,
                                         FloatRegister lhs,
                                         FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpneqpd_ripr);
}

void MacroAssemblerX64::vcmpltpdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmpltpd_ripr);
}

void MacroAssemblerX64::vcmplepdSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vcmplepd_ripr);
}

void MacroAssemblerX64::vpmaddubswSimd128(const SimdConstant& v,
                                          FloatRegister lhs,
                                          FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest,
                  &X86Encoding::BaseAssemblerX64::vpmaddubsw_ripr);
}

void MacroAssemblerX64::vpmuludqSimd128(const SimdConstant& v,
                                        FloatRegister lhs, FloatRegister dest) {
  vpRiprOpSimd128(v, lhs, dest, &X86Encoding::BaseAssemblerX64::vpmuludq_ripr);
}

void MacroAssemblerX64::bindOffsets(
    const MacroAssemblerX86Shared::UsesVector& uses) {
  for (JmpSrc src : uses) {
    JmpDst dst(currentOffset());
    // Using linkJump here is safe, as explained in the comment in
    // loadConstantDouble.
    masm.linkJump(src, dst);
  }
}

void MacroAssemblerX64::finish() {
  if (!doubles_.empty()) {
    masm.haltingAlign(sizeof(double));
  }
  for (const Double& d : doubles_) {
    bindOffsets(d.uses);
    masm.doubleConstant(d.value);
  }

  if (!floats_.empty()) {
    masm.haltingAlign(sizeof(float));
  }
  for (const Float& f : floats_) {
    bindOffsets(f.uses);
    masm.floatConstant(f.value);
  }

  // SIMD memory values must be suitably aligned.
  if (!simds_.empty()) {
    masm.haltingAlign(SimdMemoryAlignment);
  }
  for (const SimdData& v : simds_) {
    bindOffsets(v.uses);
    masm.simd128Constant(v.value.bytes());
  }

  MacroAssemblerX86Shared::finish();
}

void MacroAssemblerX64::boxValue(JSValueType type, Register src,
                                 Register dest) {
  MOZ_ASSERT(src != dest);

  JSValueShiftedTag tag = (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
#ifdef DEBUG
  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    Label upper32BitsZeroed;
    movePtr(ImmWord(UINT32_MAX), dest);
    asMasm().branchPtr(Assembler::BelowOrEqual, src, dest, &upper32BitsZeroed);
    breakpoint();
    bind(&upper32BitsZeroed);
  }
#endif
  mov(ImmShiftedTag(tag), dest);
  orq(src, dest);
}

void MacroAssemblerX64::handleFailureWithHandlerTail(Label* profilerExitTail,
                                                     Label* bailoutTail) {
  // Reserve space for exception information.
  subq(Imm32(sizeof(ResumeFromException)), rsp);
  movq(rsp, rax);

  // Call the handler.
  using Fn = void (*)(ResumeFromException * rfe);
  asMasm().setupUnalignedABICall(rcx);
  asMasm().passABIArg(rax);
  asMasm().callWithABI<Fn, HandleException>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label returnBaseline;
  Label returnIon;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  load32(Address(rsp, ResumeFromException::offsetOfKind()), rax);
  asMasm().branch32(Assembler::Equal, rax,
                    Imm32(ExceptionResumeKind::EntryFrame), &entryFrame);
  asMasm().branch32(Assembler::Equal, rax, Imm32(ExceptionResumeKind::Catch),
                    &catch_);
  asMasm().branch32(Assembler::Equal, rax, Imm32(ExceptionResumeKind::Finally),
                    &finally);
  asMasm().branch32(Assembler::Equal, rax,
                    Imm32(ExceptionResumeKind::ForcedReturnBaseline),
                    &returnBaseline);
  asMasm().branch32(Assembler::Equal, rax,
                    Imm32(ExceptionResumeKind::ForcedReturnIon), &returnIon);
  asMasm().branch32(Assembler::Equal, rax, Imm32(ExceptionResumeKind::Bailout),
                    &bailout);
  asMasm().branch32(Assembler::Equal, rax, Imm32(ExceptionResumeKind::Wasm),
                    &wasm);
  asMasm().branch32(Assembler::Equal, rax,
                    Imm32(ExceptionResumeKind::WasmCatch), &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, restore state and return from
  // the entry frame.
  bind(&entryFrame);
  asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);
  ret();

  // If we found a catch handler, this must be a baseline frame. Restore state
  // and jump to the catch block.
  bind(&catch_);
  loadPtr(Address(rsp, ResumeFromException::offsetOfTarget()), rax);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);
  jmp(Operand(rax));

  // If we found a finally block, this must be a baseline frame. Push two
  // values expected by the finally block: the exception and BooleanValue(true).
  bind(&finally);
  ValueOperand exception = ValueOperand(rcx);
  loadValue(Address(esp, ResumeFromException::offsetOfException()), exception);

  loadPtr(Address(rsp, ResumeFromException::offsetOfTarget()), rax);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);

  pushValue(exception);
  pushValue(BooleanValue(true));
  jmp(Operand(rax));

  // Return BaselineFrame->returnValue() to the caller.
  // Used in debug mode and for GeneratorReturn.
  Label profilingInstrumentation;
  bind(&returnBaseline);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);
  loadValue(Address(rbp, BaselineFrame::reverseOffsetOfReturnValue()),
            JSReturnOperand);
  jmp(&profilingInstrumentation);

  // Return the given value to the caller.
  bind(&returnIon);
  loadValue(Address(rsp, ResumeFromException::offsetOfException()),
            JSReturnOperand);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning. This code is shared by ForcedReturnIon
  // and ForcedReturnBaseline.
  bind(&profilingInstrumentation);
  {
    Label skipProfilingInstrumentation;
    AbsoluteAddress addressOfEnabled(
        asMasm().runtime()->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  movq(rbp, rsp);
  pop(rbp);
  ret();

  // If we are bailing out to baseline to handle an exception, jump to the
  // bailout tail stub. Load 1 (true) in ReturnReg to indicate success.
  bind(&bailout);
  loadPtr(Address(rsp, ResumeFromException::offsetOfBailoutInfo()), r9);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);
  move32(Imm32(1), ReturnReg);
  jump(bailoutTail);

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  loadPtr(Address(rsp, ResumeFromException::offsetOfFramePointer()), rbp);
  loadPtr(Address(rsp, ResumeFromException::offsetOfStackPointer()), rsp);
  movePtr(ImmPtr((const void*)wasm::FailInstanceReg), InstanceReg);
  masm.ret();

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  wasm::GenerateJumpToCatchHandler(asMasm(), rsp, rax, rbx);
}

void MacroAssemblerX64::profilerEnterFrame(Register framePtr,
                                           Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerX64::profilerExitFrame() {
  jump(asMasm().runtime()->jitRuntime()->getProfilerExitFrameTail());
}

Assembler::Condition MacroAssemblerX64::testStringTruthy(
    bool truthy, const ValueOperand& value) {
  ScratchRegisterScope scratch(asMasm());
  unboxString(value, scratch);
  cmp32(Operand(scratch, JSString::offsetOfLength()), Imm32(0));
  return truthy ? Assembler::NotEqual : Assembler::Equal;
}

Assembler::Condition MacroAssemblerX64::testBigIntTruthy(
    bool truthy, const ValueOperand& value) {
  ScratchRegisterScope scratch(asMasm());
  unboxBigInt(value, scratch);
  cmp32(Operand(scratch, JS::BigInt::offsetOfDigitLength()), Imm32(0));
  return truthy ? Assembler::NotEqual : Assembler::Equal;
}

MacroAssembler& MacroAssemblerX64::asMasm() {
  return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler& MacroAssemblerX64::asMasm() const {
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
        subq(Imm32(4096), StackPointer);
        store32(Imm32(0), Address(StackPointer, 0));
        amountLeft -= 4096;
      }
      subq(Imm32(amountLeft), StackPointer);
    } else {
      ScratchRegisterScope scratch(*this);
      Label top;
      move32(Imm32(fullPages), scratch);
      bind(&top);
      subq(Imm32(4096), StackPointer);
      store32(Imm32(0), Address(StackPointer, 0));
      subl(Imm32(1), scratch);
      j(Assembler::NonZero, &top);
      amountLeft -= fullPages * 4096;
      if (amountLeft) {
        subq(Imm32(amountLeft), StackPointer);
      }
    }
  }
}

void MacroAssemblerX64::convertDoubleToPtr(FloatRegister src, Register dest,
                                           Label* fail,
                                           bool negativeZeroCheck) {
  // Check for -0.0
  if (negativeZeroCheck) {
    branchNegativeZero(src, dest, fail);
  }

  ScratchDoubleScope scratch(asMasm());
  vcvttsd2sq(src, dest);
  asMasm().convertInt64ToDouble(Register64(dest), scratch);
  vucomisd(scratch, src);
  j(Assembler::Parity, fail);
  j(Assembler::NotEqual, fail);
}

//{{{ check_macroassembler_style
// ===============================================================
// ABI function calls.

void MacroAssembler::setupUnalignedABICall(Register scratch) {
  setupNativeABICall();
  dynamicAlignment_ = true;

  movq(rsp, scratch);
  andq(Imm32(~(ABIStackAlignment - 1)), rsp);
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
                                     bool cleanupArg) {
  freeStack(stackAdjust);
  if (dynamicAlignment_) {
    pop(rsp);
  }

#ifdef DEBUG
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif
}

static bool IsIntArgReg(Register reg) {
  for (uint32_t i = 0; i < NumIntArgRegs; i++) {
    if (IntArgRegs[i] == reg) {
      return true;
    }
  }

  return false;
}

void MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result) {
  if (IsIntArgReg(fun)) {
    // Callee register may be clobbered for an argument. Move the callee to
    // r10, a volatile, non-argument register.
    propagateOOM(moveResolver_.addMove(MoveOperand(fun), MoveOperand(r10),
                                       MoveOp::GENERAL));
    fun = r10;
  }

  MOZ_ASSERT(!IsIntArgReg(fun));

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(fun);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun,
                                           MoveOp::Type result) {
  Address safeFun = fun;
  if (IsIntArgReg(safeFun.base)) {
    // Callee register may be clobbered for an argument. Move the callee to
    // r10, a volatile, non-argument register.
    propagateOOM(moveResolver_.addMove(MoveOperand(fun.base), MoveOperand(r10),
                                       MoveOp::GENERAL));
    safeFun.base = r10;
  }

  MOZ_ASSERT(!IsIntArgReg(safeFun.base));

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(safeFun);
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
    boxValue(ValueTypeFromMIRType(type), reg.gpr(), dest.valueReg());
    return;
  }

  ScratchDoubleScope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, freg);
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  if (src == dest) {
    return;
  }
  movq(src.valueReg(), dest.valueReg());
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  movWithPatch(ImmWord(src.asRawBits()), dest.valueReg());
  writeDataRelocation(src);
}

// ===============================================================
// Branch functions

void MacroAssembler::loadStoreBuffer(Register ptr, Register buffer) {
  if (ptr != buffer) {
    movePtr(ptr, buffer);
  }
  andPtr(Imm32(int32_t(~gc::ChunkMask)), buffer);
  loadPtr(Address(buffer, gc::ChunkStoreBufferOffset), buffer);
}

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  ScratchRegisterScope scratch(*this);
  MOZ_ASSERT(ptr != temp);
  MOZ_ASSERT(ptr != scratch);

  movePtr(ptr, scratch);
  andPtr(Imm32(int32_t(~gc::ChunkMask)), scratch);
  branchPtr(InvertCondition(cond), Address(scratch, gc::ChunkStoreBufferOffset),
            ImmWord(0), label);
}

template <typename T>
void MacroAssembler::branchValueIsNurseryCellImpl(Condition cond,
                                                  const T& value, Register temp,
                                                  Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(temp != InvalidReg);

  Label done;
  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);

  getGCThingValueChunk(value, temp);
  branchPtr(InvertCondition(cond), Address(temp, gc::ChunkStoreBufferOffset),
            ImmWord(0), label);

  bind(&done);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  branchValueIsNurseryCellImpl(cond, address, temp, label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  branchValueIsNurseryCellImpl(cond, value, temp, label);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(*this);
  MOZ_ASSERT(lhs.valueReg() != scratch);
  moveValue(rhs, ValueOperand(scratch));
  cmpPtr(lhs.valueReg(), scratch);
  j(cond, label);
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest) {
  MOZ_ASSERT(valueType < MIRType::Value);

  if (valueType == MIRType::Double) {
    boxDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  if (value.constant()) {
    storeValue(value.value(), dest);
  } else {
    storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(),
               dest);
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest);

void MacroAssembler::PushBoxed(FloatRegister reg) {
  subq(Imm32(sizeof(double)), StackPointer);
  boxDouble(reg, Address(StackPointer, 0));
  adjustFrame(sizeof(double));
}

// ========================================================================
// wasm support

void MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access,
                              Operand srcAddr, AnyRegister out) {
  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  memoryBarrierBefore(access.sync());

  MOZ_ASSERT_IF(
      access.isZeroExtendSimd128Load(),
      access.type() == Scalar::Float32 || access.type() == Scalar::Float64);
  MOZ_ASSERT_IF(
      access.isSplatSimd128Load(),
      access.type() == Scalar::Uint8 || access.type() == Scalar::Uint16 ||
          access.type() == Scalar::Float32 || access.type() == Scalar::Float64);
  MOZ_ASSERT_IF(access.isWidenSimd128Load(), access.type() == Scalar::Float64);

  append(access, size());
  switch (access.type()) {
    case Scalar::Int8:
      movsbl(srcAddr, out.gpr());
      break;
    case Scalar::Uint8:
      if (access.isSplatSimd128Load()) {
        vbroadcastb(srcAddr, out.fpu());
      } else {
        movzbl(srcAddr, out.gpr());
      }
      break;
    case Scalar::Int16:
      movswl(srcAddr, out.gpr());
      break;
    case Scalar::Uint16:
      if (access.isSplatSimd128Load()) {
        vbroadcastw(srcAddr, out.fpu());
      } else {
        movzwl(srcAddr, out.gpr());
      }
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      movl(srcAddr, out.gpr());
      break;
    case Scalar::Float32:
      if (access.isSplatSimd128Load()) {
        vbroadcastss(srcAddr, out.fpu());
      } else {
        // vmovss does the right thing also for access.isZeroExtendSimd128Load()
        vmovss(srcAddr, out.fpu());
      }
      break;
    case Scalar::Float64:
      if (access.isSplatSimd128Load()) {
        vmovddup(srcAddr, out.fpu());
      } else if (access.isWidenSimd128Load()) {
        switch (access.widenSimdOp()) {
          case wasm::SimdOp::V128Load8x8S:
            vpmovsxbw(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::V128Load8x8U:
            vpmovzxbw(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::V128Load16x4S:
            vpmovsxwd(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::V128Load16x4U:
            vpmovzxwd(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::V128Load32x2S:
            vpmovsxdq(srcAddr, out.fpu());
            break;
          case wasm::SimdOp::V128Load32x2U:
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
      MacroAssemblerX64::loadUnalignedSimd128(srcAddr, out.fpu());
      break;
    case Scalar::Int64:
      MOZ_CRASH("int64 loads must use load64");
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::Uint8Clamped:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected scalar type for wasmLoad");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access,
                                 Operand srcAddr, Register64 out) {
  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  memoryBarrierBefore(access.sync());

  append(access, size());
  switch (access.type()) {
    case Scalar::Int8:
      movsbq(srcAddr, out.reg);
      break;
    case Scalar::Uint8:
      movzbq(srcAddr, out.reg);
      break;
    case Scalar::Int16:
      movswq(srcAddr, out.reg);
      break;
    case Scalar::Uint16:
      movzwq(srcAddr, out.reg);
      break;
    case Scalar::Int32:
      movslq(srcAddr, out.reg);
      break;
    // Int32 to int64 moves zero-extend by default.
    case Scalar::Uint32:
      movl(srcAddr, out.reg);
      break;
    case Scalar::Int64:
      movq(srcAddr, out.reg);
      break;
    case Scalar::Float32:
    case Scalar::Float64:
    case Scalar::Simd128:
      MOZ_CRASH("float loads must use wasmLoad");
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected scalar type for wasmLoadI64");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access,
                               AnyRegister value, Operand dstAddr) {
  // NOTE: the generated code must match the assembly code in gen_store in
  // GenerateAtomicOperations.py
  memoryBarrierBefore(access.sync());

  append(access, masm.size());
  switch (access.type()) {
    case Scalar::Int8:
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
    case Scalar::Int64:
      movq(value.gpr(), dstAddr);
      break;
    case Scalar::Float32:
      storeUncanonicalizedFloat32(value.fpu(), dstAddr);
      break;
    case Scalar::Float64:
      storeUncanonicalizedDouble(value.fpu(), dstAddr);
      break;
    case Scalar::Simd128:
      MacroAssemblerX64::storeUnalignedSimd128(value.fpu(), dstAddr);
      break;
    case Scalar::Uint8Clamped:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    case Scalar::MaxTypedArrayViewType:
      MOZ_CRASH("unexpected array type");
  }

  memoryBarrierAfter(access.sync());
}

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  vcvttsd2sq(input, output);

  // Check that the result is in the uint32_t range.
  ScratchRegisterScope scratch(*this);
  move32(Imm32(0xffffffff), scratch);
  cmpq(scratch, output);
  j(Assembler::Above, oolEntry);
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  vcvttss2sq(input, output);

  // Check that the result is in the uint32_t range.
  ScratchRegisterScope scratch(*this);
  move32(Imm32(0xffffffff), scratch);
  cmpq(scratch, output);
  j(Assembler::Above, oolEntry);
}

void MacroAssembler::wasmTruncateDoubleToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  vcvttsd2sq(input, output.reg);
  cmpq(Imm32(1), output.reg);
  j(Assembler::Overflow, oolEntry);
  bind(oolRejoin);
}

void MacroAssembler::wasmTruncateFloat32ToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  vcvttss2sq(input, output.reg);
  cmpq(Imm32(1), output.reg);
  j(Assembler::Overflow, oolEntry);
  bind(oolRejoin);
}

void MacroAssembler::wasmTruncateDoubleToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  // If the input < INT64_MAX, vcvttsd2sq will do the right thing, so
  // we use it directly. Else, we subtract INT64_MAX, convert to int64,
  // and then add INT64_MAX to the result.

  Label isLarge;

  ScratchDoubleScope scratch(*this);
  loadConstantDouble(double(0x8000000000000000), scratch);
  branchDouble(Assembler::DoubleGreaterThanOrEqual, input, scratch, &isLarge);
  vcvttsd2sq(input, output.reg);
  testq(output.reg, output.reg);
  j(Assembler::Signed, oolEntry);
  jump(oolRejoin);

  bind(&isLarge);

  moveDouble(input, tempReg);
  vsubsd(scratch, tempReg, tempReg);
  vcvttsd2sq(tempReg, output.reg);
  testq(output.reg, output.reg);
  j(Assembler::Signed, oolEntry);
  or64(Imm64(0x8000000000000000), output);

  bind(oolRejoin);
}

void MacroAssembler::wasmTruncateFloat32ToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempReg) {
  // If the input < INT64_MAX, vcvttss2sq will do the right thing, so
  // we use it directly. Else, we subtract INT64_MAX, convert to int64,
  // and then add INT64_MAX to the result.

  Label isLarge;

  ScratchFloat32Scope scratch(*this);
  loadConstantFloat32(float(0x8000000000000000), scratch);
  branchFloat(Assembler::DoubleGreaterThanOrEqual, input, scratch, &isLarge);
  vcvttss2sq(input, output.reg);
  testq(output.reg, output.reg);
  j(Assembler::Signed, oolEntry);
  jump(oolRejoin);

  bind(&isLarge);

  moveFloat32(input, tempReg);
  vsubss(scratch, tempReg, tempReg);
  vcvttss2sq(tempReg, output.reg);
  testq(output.reg, output.reg);
  j(Assembler::Signed, oolEntry);
  or64(Imm64(0x8000000000000000), output);

  bind(oolRejoin);
}

void MacroAssembler::widenInt32(Register r) {
  move32To64ZeroExtend(r, Register64(r));
}

// ========================================================================
// Convert floating point.

void MacroAssembler::convertInt64ToDouble(Register64 input,
                                          FloatRegister output) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroDouble(output);

  vcvtsq2sd(input.reg, output, output);
}

void MacroAssembler::convertInt64ToFloat32(Register64 input,
                                           FloatRegister output) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroFloat32(output);

  vcvtsq2ss(input.reg, output, output);
}

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return true; }

void MacroAssembler::convertUInt64ToDouble(Register64 input,
                                           FloatRegister output,
                                           Register temp) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroDouble(output);

  // If the input's sign bit is not set we use vcvtsq2sd directly.
  // Else, we divide by 2 and keep the LSB, convert to double, and multiply
  // the result by 2.
  Label done;
  Label isSigned;

  testq(input.reg, input.reg);
  j(Assembler::Signed, &isSigned);
  vcvtsq2sd(input.reg, output, output);
  jump(&done);

  bind(&isSigned);

  ScratchRegisterScope scratch(*this);
  mov(input.reg, scratch);
  mov(input.reg, temp);
  shrq(Imm32(1), scratch);
  andq(Imm32(1), temp);
  orq(temp, scratch);

  vcvtsq2sd(scratch, output, output);
  vaddsd(output, output, output);

  bind(&done);
}

void MacroAssembler::convertUInt64ToFloat32(Register64 input,
                                            FloatRegister output,
                                            Register temp) {
  // Zero the output register to break dependencies, see convertInt32ToDouble.
  zeroFloat32(output);

  // See comment in convertUInt64ToDouble.
  Label done;
  Label isSigned;

  testq(input.reg, input.reg);
  j(Assembler::Signed, &isSigned);
  vcvtsq2ss(input.reg, output, output);
  jump(&done);

  bind(&isSigned);

  ScratchRegisterScope scratch(*this);
  mov(input.reg, scratch);
  mov(input.reg, temp);
  shrq(Imm32(1), scratch);
  andq(Imm32(1), temp);
  orq(temp, scratch);

  vcvtsq2ss(scratch, output, output);
  vaddss(output, output, output);

  bind(&done);
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt64ToDouble(Register64(src), dest);
}

// ========================================================================
// Primitive atomic operations.

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  MOZ_ASSERT(output.reg == rax);
  if (expected != output) {
    movq(expected.reg, output.reg);
  }
  append(access, size());
  lock_cmpxchgq(replacement.reg, Operand(mem));
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  MOZ_ASSERT(output.reg == rax);
  if (expected != output) {
    movq(expected.reg, output.reg);
  }
  append(access, size());
  lock_cmpxchgq(replacement.reg, Operand(mem));
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 value,
                                          Register64 output) {
  if (value != output) {
    movq(value.reg, output.reg);
  }
  append(access, masm.size());
  xchgq(output.reg, Operand(mem));
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem,
                                          Register64 value, Register64 output) {
  if (value != output) {
    movq(value.reg, output.reg);
  }
  append(access, masm.size());
  xchgq(output.reg, Operand(mem));
}

template <typename T>
static void AtomicFetchOp64(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access, AtomicOp op,
                            Register value, const T& mem, Register temp,
                            Register output) {
  // NOTE: the generated code must match the assembly code in gen_fetchop in
  // GenerateAtomicOperations.py
  if (op == AtomicFetchAddOp) {
    if (value != output) {
      masm.movq(value, output);
    }
    if (access) {
      masm.append(*access, masm.size());
    }
    masm.lock_xaddq(output, Operand(mem));
  } else if (op == AtomicFetchSubOp) {
    if (value != output) {
      masm.movq(value, output);
    }
    masm.negq(output);
    if (access) {
      masm.append(*access, masm.size());
    }
    masm.lock_xaddq(output, Operand(mem));
  } else {
    Label again;
    MOZ_ASSERT(output == rax);
    MOZ_ASSERT(value != output);
    MOZ_ASSERT(value != temp);
    MOZ_ASSERT(temp != output);
    if (access) {
      masm.append(*access, masm.size());
    }
    masm.movq(Operand(mem), rax);
    masm.bind(&again);
    masm.movq(rax, temp);
    switch (op) {
      case AtomicFetchAndOp:
        masm.andq(value, temp);
        break;
      case AtomicFetchOrOp:
        masm.orq(value, temp);
        break;
      case AtomicFetchXorOp:
        masm.xorq(value, temp);
        break;
      default:
        MOZ_CRASH();
    }
    masm.lock_cmpxchgq(temp, Operand(mem));
    masm.j(MacroAssembler::NonZero, &again);
  }
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, op, value.reg, mem, temp.reg, output.reg);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, op, value.reg, mem, temp.reg, output.reg);
}

template <typename T>
static void AtomicEffectOp64(MacroAssembler& masm,
                             const wasm::MemoryAccessDesc* access, AtomicOp op,
                             Register value, const T& mem) {
  if (access) {
    masm.append(*access, masm.size());
  }
  switch (op) {
    case AtomicFetchAddOp:
      masm.lock_addq(value, Operand(mem));
      break;
    case AtomicFetchSubOp:
      masm.lock_subq(value, Operand(mem));
      break;
    case AtomicFetchAndOp:
      masm.lock_andq(value, Operand(mem));
      break;
    case AtomicFetchOrOp:
      masm.lock_orq(value, Operand(mem));
      break;
    case AtomicFetchXorOp:
      masm.lock_xorq(value, Operand(mem));
      break;
    default:
      MOZ_CRASH();
  }
}

void MacroAssembler::wasmAtomicEffectOp64(const wasm::MemoryAccessDesc& access,
                                          AtomicOp op, Register64 value,
                                          const BaseIndex& mem) {
  AtomicEffectOp64(*this, &access, op, value.reg, mem);
}

void MacroAssembler::compareExchange64(const Synchronization&,
                                       const Address& mem, Register64 expected,
                                       Register64 replacement,
                                       Register64 output) {
  // NOTE: the generated code must match the assembly code in gen_cmpxchg in
  // GenerateAtomicOperations.py
  MOZ_ASSERT(output.reg == rax);
  if (expected != output) {
    movq(expected.reg, output.reg);
  }
  lock_cmpxchgq(replacement.reg, Operand(mem));
}

void MacroAssembler::compareExchange64(const Synchronization&,
                                       const BaseIndex& mem,
                                       Register64 expected,
                                       Register64 replacement,
                                       Register64 output) {
  MOZ_ASSERT(output.reg == rax);
  if (expected != output) {
    movq(expected.reg, output.reg);
  }
  lock_cmpxchgq(replacement.reg, Operand(mem));
}

void MacroAssembler::atomicExchange64(const Synchronization&,
                                      const Address& mem, Register64 value,
                                      Register64 output) {
  // NOTE: the generated code must match the assembly code in gen_exchange in
  // GenerateAtomicOperations.py
  if (value != output) {
    movq(value.reg, output.reg);
  }
  xchgq(output.reg, Operand(mem));
}

void MacroAssembler::atomicExchange64(const Synchronization&,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  if (value != output) {
    movq(value.reg, output.reg);
  }
  xchgq(output.reg, Operand(mem));
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, op, value.reg, mem, temp.reg, output.reg);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, op, value.reg, mem, temp.reg, output.reg);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const Address& mem) {
  AtomicEffectOp64(*this, nullptr, op, value.reg, mem);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const BaseIndex& mem) {
  AtomicEffectOp64(*this, nullptr, op, value.reg, mem);
}

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest) {
  return leaRipRelative(dest);
}

void MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                          CodeLocationLabel target) {
  ptrdiff_t off = target - loc;
  MOZ_ASSERT(off > ptrdiff_t(INT32_MIN));
  MOZ_ASSERT(off < ptrdiff_t(INT32_MAX));
  PatchWrite_Imm32(loc, Imm32(off));
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Register64 boundsCheckLimit, Label* ok) {
  cmpPtr(index.reg, boundsCheckLimit.reg);
  j(cond, ok);
  if (JitOptions.spectreIndexMasking) {
    cmovCCq(cond, Operand(boundsCheckLimit.reg), index.reg);
  }
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Address boundsCheckLimit, Label* ok) {
  cmpPtr(index.reg, Operand(boundsCheckLimit));
  j(cond, ok);
  if (JitOptions.spectreIndexMasking) {
    cmovCCq(cond, Operand(boundsCheckLimit), index.reg);
  }
}

// ========================================================================
// Integer compare-then-conditionally-load/move operations.

// cmpMove, Cond-Reg-Reg-Reg-Reg cases

template <size_t CmpSize, size_t MoveSize>
void MacroAssemblerX64::cmpMove(Condition cond, Register lhs, Register rhs,
                                Register falseVal, Register trueValAndDest) {
  if constexpr (CmpSize == 32) {
    cmp32(lhs, rhs);
  } else {
    static_assert(CmpSize == 64);
    cmpPtr(lhs, rhs);
  }
  if constexpr (MoveSize == 32) {
    cmovCCl(cond, Operand(falseVal), trueValAndDest);
  } else {
    static_assert(MoveSize == 64);
    cmovCCq(cond, Operand(falseVal), trueValAndDest);
  }
}
template void MacroAssemblerX64::cmpMove<32, 32>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<32, 64>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<64, 32>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<64, 64>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);

// cmpMove, Cond-Reg-Addr-Reg-Reg cases

template <size_t CmpSize, size_t MoveSize>
void MacroAssemblerX64::cmpMove(Condition cond, Register lhs,
                                const Address& rhs, Register falseVal,
                                Register trueValAndDest) {
  if constexpr (CmpSize == 32) {
    cmp32(lhs, Operand(rhs));
  } else {
    static_assert(CmpSize == 64);
    cmpPtr(lhs, Operand(rhs));
  }
  if constexpr (MoveSize == 32) {
    cmovCCl(cond, Operand(falseVal), trueValAndDest);
  } else {
    static_assert(MoveSize == 64);
    cmovCCq(cond, Operand(falseVal), trueValAndDest);
  }
}
template void MacroAssemblerX64::cmpMove<32, 32>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<32, 64>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<64, 32>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpMove<64, 64>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 Register falseVal,
                                                 Register trueValAndDest);

// cmpLoad, Cond-Reg-Reg-Addr-Reg cases

template <size_t CmpSize, size_t LoadSize>
void MacroAssemblerX64::cmpLoad(Condition cond, Register lhs, Register rhs,
                                const Address& falseVal,
                                Register trueValAndDest) {
  if constexpr (CmpSize == 32) {
    cmp32(lhs, rhs);
  } else {
    static_assert(CmpSize == 64);
    cmpPtr(lhs, rhs);
  }
  if constexpr (LoadSize == 32) {
    cmovCCl(cond, Operand(falseVal), trueValAndDest);
  } else {
    static_assert(LoadSize == 64);
    cmovCCq(cond, Operand(falseVal), trueValAndDest);
  }
}
template void MacroAssemblerX64::cmpLoad<32, 32>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<32, 64>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<64, 32>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<64, 64>(Condition cond, Register lhs,
                                                 Register rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);

// cmpLoad, Cond-Reg-Addr-Addr-Reg cases

template <size_t CmpSize, size_t LoadSize>
void MacroAssemblerX64::cmpLoad(Condition cond, Register lhs,
                                const Address& rhs, const Address& falseVal,
                                Register trueValAndDest) {
  if constexpr (CmpSize == 32) {
    cmp32(lhs, Operand(rhs));
  } else {
    static_assert(CmpSize == 64);
    cmpPtr(lhs, Operand(rhs));
  }
  if constexpr (LoadSize == 32) {
    cmovCCl(cond, Operand(falseVal), trueValAndDest);
  } else {
    static_assert(LoadSize == 64);
    cmovCCq(cond, Operand(falseVal), trueValAndDest);
  }
}
template void MacroAssemblerX64::cmpLoad<32, 32>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<32, 64>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<64, 32>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);
template void MacroAssemblerX64::cmpLoad<64, 64>(Condition cond, Register lhs,
                                                 const Address& rhs,
                                                 const Address& falseVal,
                                                 Register trueValAndDest);

//}}} check_macroassembler_style
