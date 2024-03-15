/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/wasm32/CodeGenerator-wasm32.h"

#include "jit/CodeGenerator.h"

using namespace js::jit;

void CodeGenerator::visitDouble(LDouble*) { MOZ_CRASH(); }
void CodeGenerator::visitFloat32(LFloat32* ins) { MOZ_CRASH(); }
void CodeGenerator::visitValue(LValue* value) { MOZ_CRASH(); }
void CodeGenerator::visitWasmReinterpret(LWasmReinterpret* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitRotateI64(LRotateI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitTestIAndBranch(LTestIAndBranch* test) { MOZ_CRASH(); }
void CodeGenerator::visitTestI64AndBranch(LTestI64AndBranch* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitTestDAndBranch(LTestDAndBranch* test) { MOZ_CRASH(); }
void CodeGenerator::visitTestFAndBranch(LTestFAndBranch* test) { MOZ_CRASH(); }
void CodeGenerator::visitCompare(LCompare* comp) { MOZ_CRASH(); }
void CodeGenerator::visitCompareI64(LCompareI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitCompareI64AndBranch(LCompareI64AndBranch* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitCompareAndBranch(LCompareAndBranch* comp) {
  MOZ_CRASH();
}
void CodeGenerator::visitCompareD(LCompareD* comp) { MOZ_CRASH(); }
void CodeGenerator::visitCompareF(LCompareF* comp) { MOZ_CRASH(); }
void CodeGenerator::visitCompareDAndBranch(LCompareDAndBranch* comp) {
  MOZ_CRASH();
}
void CodeGenerator::visitCompareFAndBranch(LCompareFAndBranch* comp) {
  MOZ_CRASH();
}
void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* lir) { MOZ_CRASH(); }
void CodeGenerator::visitNotI(LNotI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNotI64(LNotI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitNotD(LNotD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNotF(LNotF* ins) { MOZ_CRASH(); }
void CodeGenerator::visitBitNotI(LBitNotI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitBitNotI64(LBitNotI64* ins) { MOZ_CRASH(); }
void CodeGenerator::visitBitOpI(LBitOpI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitBitOpI64(LBitOpI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitShiftI(LShiftI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitShiftI64(LShiftI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitSignExtendInt64(LSignExtendInt64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitUrshD(LUrshD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitMinMaxD(LMinMaxD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitMinMaxF(LMinMaxF* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNegI(LNegI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNegI64(LNegI64* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNegD(LNegD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitNegF(LNegF* ins) { MOZ_CRASH(); }
void CodeGenerator::visitCopySignD(LCopySignD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitCopySignF(LCopySignF* ins) { MOZ_CRASH(); }
void CodeGenerator::visitClzI(LClzI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitClzI64(LClzI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitCtzI(LCtzI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitCtzI64(LCtzI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitPopcntI(LPopcntI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitPopcntI64(LPopcntI64* ins) { MOZ_CRASH(); }
void CodeGenerator::visitAddI(LAddI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitAddI64(LAddI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitSubI(LSubI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitSubI64(LSubI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitMulI64(LMulI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitMathD(LMathD* math) { MOZ_CRASH(); }
void CodeGenerator::visitMathF(LMathF* math) { MOZ_CRASH(); }
void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmTruncateToInt32(LWasmTruncateToInt32* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitPowHalfD(LPowHalfD* ins) { MOZ_CRASH(); }
void CodeGenerator::visitCompareExchangeTypedArrayElement(
    LCompareExchangeTypedArrayElement* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicExchangeTypedArrayElement(
    LAtomicExchangeTypedArrayElement* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicTypedArrayElementBinop64(
    LAtomicTypedArrayElementBinop64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitAtomicStore64(LAtomicStore64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitEffectiveAddress(LEffectiveAddress* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitNearbyInt(LNearbyInt*) { MOZ_CRASH(); }
void CodeGenerator::visitNearbyIntF(LNearbyIntF*) { MOZ_CRASH(); }
void CodeGenerator::visitWasmSelectI64(LWasmSelectI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmAddOffset(LWasmAddOffset* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmAddOffset64(LWasmAddOffset64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmExtendU32Index(LWasmExtendU32Index* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmWrapU32Index(LWasmWrapU32Index* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicTypedArrayElementBinop(
    LAtomicTypedArrayElementBinop* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect(
    LAtomicTypedArrayElementBinopForEffect* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmSelect(LWasmSelect* ins) { MOZ_CRASH(); }
void CodeGenerator::visitWasmHeapBase(LWasmHeapBase* ins) { MOZ_CRASH(); }
void CodeGenerator::visitWasmLoad(LWasmLoad* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmLoadI64(LWasmLoadI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmStore(LWasmStore* lir) { MOZ_CRASH(); }
void CodeGenerator::visitWasmStoreI64(LWasmStoreI64* lir) { MOZ_CRASH(); }
void CodeGenerator::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins) { MOZ_CRASH(); }
void CodeGenerator::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) { MOZ_CRASH(); }
void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmStackArg(LWasmStackArg* ins) { MOZ_CRASH(); }
void CodeGenerator::visitWasmStackArgI64(LWasmStackArgI64* ins) { MOZ_CRASH(); }
void CodeGenerator::visitMemoryBarrier(LMemoryBarrier* ins) { MOZ_CRASH(); }
void CodeGenerator::visitSimd128(LSimd128* ins) { MOZ_CRASH(); }
void CodeGenerator::visitWasmTernarySimd128(LWasmTernarySimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmBinarySimd128(LWasmBinarySimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmBinarySimd128WithConstant(
    LWasmBinarySimd128WithConstant* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmVariableShiftSimd128(
    LWasmVariableShiftSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmConstantShiftSimd128(
    LWasmConstantShiftSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmSignReplicationSimd128(
    LWasmSignReplicationSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmShuffleSimd128(LWasmShuffleSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmPermuteSimd128(LWasmPermuteSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReplaceLaneSimd128(LWasmReplaceLaneSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReplaceInt64LaneSimd128(
    LWasmReplaceInt64LaneSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmScalarToSimd128(LWasmScalarToSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmInt64ToSimd128(LWasmInt64ToSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmUnarySimd128(LWasmUnarySimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReduceSimd128(LWasmReduceSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReduceAndBranchSimd128(
    LWasmReduceAndBranchSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmReduceSimd128ToInt64(
    LWasmReduceSimd128ToInt64* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmLoadLaneSimd128(LWasmLoadLaneSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmStoreLaneSimd128(LWasmStoreLaneSimd128* ins) {
  MOZ_CRASH();
}
void CodeGenerator::visitUnbox(LUnbox* unbox) { MOZ_CRASH(); }
void CodeGenerator::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir) {
  MOZ_CRASH();
}
void CodeGenerator::visitDivI(LDivI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitModI(LModI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitDivPowTwoI(LDivPowTwoI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitModPowTwoI(LModPowTwoI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitMulI(LMulI* ins) { MOZ_CRASH(); }
void CodeGenerator::visitBox(LBox* box) { MOZ_CRASH(); }
