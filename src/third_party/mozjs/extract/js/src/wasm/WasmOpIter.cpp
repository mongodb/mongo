/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmOpIter.h"

#include "jit/AtomicOp.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

#ifdef ENABLE_WASM_GC
#  ifndef ENABLE_WASM_GC
#    error "GC types require the function-references feature"
#  endif
#endif

#ifdef DEBUG

#  ifdef ENABLE_WASM_GC
#    define WASM_FUNCTION_REFERENCES_OP(code) return code
#  else
#    define WASM_FUNCTION_REFERENCES_OP(code) break
#  endif
#  ifdef ENABLE_WASM_GC
#    define WASM_GC_OP(code) return code
#  else
#    define WASM_GC_OP(code) break
#  endif
#  ifdef ENABLE_WASM_SIMD
#    define WASM_SIMD_OP(code) return code
#  else
#    define WASM_SIMD_OP(code) break
#  endif

OpKind wasm::Classify(OpBytes op) {
  switch (Op(op.b0)) {
    case Op::Block:
      return OpKind::Block;
    case Op::Loop:
      return OpKind::Loop;
    case Op::Unreachable:
      return OpKind::Unreachable;
    case Op::Drop:
      return OpKind::Drop;
    case Op::I32Const:
      return OpKind::I32;
    case Op::I64Const:
      return OpKind::I64;
    case Op::F32Const:
      return OpKind::F32;
    case Op::F64Const:
      return OpKind::F64;
    case Op::Br:
      return OpKind::Br;
    case Op::BrIf:
      return OpKind::BrIf;
    case Op::BrTable:
      return OpKind::BrTable;
    case Op::Nop:
      return OpKind::Nop;
    case Op::I32Clz:
    case Op::I32Ctz:
    case Op::I32Popcnt:
    case Op::I64Clz:
    case Op::I64Ctz:
    case Op::I64Popcnt:
    case Op::F32Abs:
    case Op::F32Neg:
    case Op::F32Ceil:
    case Op::F32Floor:
    case Op::F32Trunc:
    case Op::F32Nearest:
    case Op::F32Sqrt:
    case Op::F64Abs:
    case Op::F64Neg:
    case Op::F64Ceil:
    case Op::F64Floor:
    case Op::F64Trunc:
    case Op::F64Nearest:
    case Op::F64Sqrt:
      return OpKind::Unary;
    case Op::I32Add:
    case Op::I32Sub:
    case Op::I32Mul:
    case Op::I32DivS:
    case Op::I32DivU:
    case Op::I32RemS:
    case Op::I32RemU:
    case Op::I32And:
    case Op::I32Or:
    case Op::I32Xor:
    case Op::I32Shl:
    case Op::I32ShrS:
    case Op::I32ShrU:
    case Op::I32Rotl:
    case Op::I32Rotr:
    case Op::I64Add:
    case Op::I64Sub:
    case Op::I64Mul:
    case Op::I64DivS:
    case Op::I64DivU:
    case Op::I64RemS:
    case Op::I64RemU:
    case Op::I64And:
    case Op::I64Or:
    case Op::I64Xor:
    case Op::I64Shl:
    case Op::I64ShrS:
    case Op::I64ShrU:
    case Op::I64Rotl:
    case Op::I64Rotr:
    case Op::F32Add:
    case Op::F32Sub:
    case Op::F32Mul:
    case Op::F32Div:
    case Op::F32Min:
    case Op::F32Max:
    case Op::F32CopySign:
    case Op::F64Add:
    case Op::F64Sub:
    case Op::F64Mul:
    case Op::F64Div:
    case Op::F64Min:
    case Op::F64Max:
    case Op::F64CopySign:
      return OpKind::Binary;
    case Op::I32Eq:
    case Op::I32Ne:
    case Op::I32LtS:
    case Op::I32LtU:
    case Op::I32LeS:
    case Op::I32LeU:
    case Op::I32GtS:
    case Op::I32GtU:
    case Op::I32GeS:
    case Op::I32GeU:
    case Op::I64Eq:
    case Op::I64Ne:
    case Op::I64LtS:
    case Op::I64LtU:
    case Op::I64LeS:
    case Op::I64LeU:
    case Op::I64GtS:
    case Op::I64GtU:
    case Op::I64GeS:
    case Op::I64GeU:
    case Op::F32Eq:
    case Op::F32Ne:
    case Op::F32Lt:
    case Op::F32Le:
    case Op::F32Gt:
    case Op::F32Ge:
    case Op::F64Eq:
    case Op::F64Ne:
    case Op::F64Lt:
    case Op::F64Le:
    case Op::F64Gt:
    case Op::F64Ge:
      return OpKind::Comparison;
    case Op::I32Eqz:
    case Op::I32WrapI64:
    case Op::I32TruncF32S:
    case Op::I32TruncF32U:
    case Op::I32ReinterpretF32:
    case Op::I32TruncF64S:
    case Op::I32TruncF64U:
    case Op::I64ExtendI32S:
    case Op::I64ExtendI32U:
    case Op::I64TruncF32S:
    case Op::I64TruncF32U:
    case Op::I64TruncF64S:
    case Op::I64TruncF64U:
    case Op::I64ReinterpretF64:
    case Op::I64Eqz:
    case Op::F32ConvertI32S:
    case Op::F32ConvertI32U:
    case Op::F32ReinterpretI32:
    case Op::F32ConvertI64S:
    case Op::F32ConvertI64U:
    case Op::F32DemoteF64:
    case Op::F64ConvertI32S:
    case Op::F64ConvertI32U:
    case Op::F64ConvertI64S:
    case Op::F64ConvertI64U:
    case Op::F64ReinterpretI64:
    case Op::F64PromoteF32:
    case Op::I32Extend8S:
    case Op::I32Extend16S:
    case Op::I64Extend8S:
    case Op::I64Extend16S:
    case Op::I64Extend32S:
      return OpKind::Conversion;
    case Op::I32Load8S:
    case Op::I32Load8U:
    case Op::I32Load16S:
    case Op::I32Load16U:
    case Op::I64Load8S:
    case Op::I64Load8U:
    case Op::I64Load16S:
    case Op::I64Load16U:
    case Op::I64Load32S:
    case Op::I64Load32U:
    case Op::I32Load:
    case Op::I64Load:
    case Op::F32Load:
    case Op::F64Load:
      return OpKind::Load;
    case Op::I32Store8:
    case Op::I32Store16:
    case Op::I64Store8:
    case Op::I64Store16:
    case Op::I64Store32:
    case Op::I32Store:
    case Op::I64Store:
    case Op::F32Store:
    case Op::F64Store:
      return OpKind::Store;
    case Op::SelectNumeric:
    case Op::SelectTyped:
      return OpKind::Select;
    case Op::LocalGet:
      return OpKind::GetLocal;
    case Op::LocalSet:
      return OpKind::SetLocal;
    case Op::LocalTee:
      return OpKind::TeeLocal;
    case Op::GlobalGet:
      return OpKind::GetGlobal;
    case Op::GlobalSet:
      return OpKind::SetGlobal;
    case Op::TableGet:
      return OpKind::TableGet;
    case Op::TableSet:
      return OpKind::TableSet;
    case Op::Call:
      return OpKind::Call;
    case Op::ReturnCall:
      return OpKind::ReturnCall;
    case Op::CallIndirect:
      return OpKind::CallIndirect;
    case Op::ReturnCallIndirect:
      return OpKind::ReturnCallIndirect;
    case Op::CallRef:
      WASM_FUNCTION_REFERENCES_OP(OpKind::CallRef);
    case Op::ReturnCallRef:
      WASM_FUNCTION_REFERENCES_OP(OpKind::ReturnCallRef);
    case Op::Return:
    case Op::Limit:
      // Accept Limit, for use in decoding the end of a function after the body.
      return OpKind::Return;
    case Op::If:
      return OpKind::If;
    case Op::Else:
      return OpKind::Else;
    case Op::End:
      return OpKind::End;
    case Op::Catch:
      return OpKind::Catch;
    case Op::CatchAll:
      return OpKind::CatchAll;
    case Op::Delegate:
      return OpKind::Delegate;
    case Op::Throw:
      return OpKind::Throw;
    case Op::Rethrow:
      return OpKind::Rethrow;
    case Op::Try:
      return OpKind::Try;
    case Op::ThrowRef:
      return OpKind::ThrowRef;
    case Op::TryTable:
      return OpKind::TryTable;
    case Op::MemorySize:
      return OpKind::MemorySize;
    case Op::MemoryGrow:
      return OpKind::MemoryGrow;
    case Op::RefNull:
      return OpKind::RefNull;
    case Op::RefIsNull:
      return OpKind::Conversion;
    case Op::RefFunc:
      return OpKind::RefFunc;
    case Op::RefAsNonNull:
      WASM_FUNCTION_REFERENCES_OP(OpKind::RefAsNonNull);
    case Op::BrOnNull:
      WASM_FUNCTION_REFERENCES_OP(OpKind::BrOnNull);
    case Op::BrOnNonNull:
      WASM_FUNCTION_REFERENCES_OP(OpKind::BrOnNonNull);
    case Op::RefEq:
      WASM_GC_OP(OpKind::Comparison);
    case Op::GcPrefix: {
      switch (GcOp(op.b1)) {
        case GcOp::Limit:
          // Reject Limit for GcPrefix encoding
          break;
        case GcOp::StructNew:
          WASM_GC_OP(OpKind::StructNew);
        case GcOp::StructNewDefault:
          WASM_GC_OP(OpKind::StructNewDefault);
        case GcOp::StructGet:
        case GcOp::StructGetS:
        case GcOp::StructGetU:
          WASM_GC_OP(OpKind::StructGet);
        case GcOp::StructSet:
          WASM_GC_OP(OpKind::StructSet);
        case GcOp::ArrayNew:
          WASM_GC_OP(OpKind::ArrayNew);
        case GcOp::ArrayNewFixed:
          WASM_GC_OP(OpKind::ArrayNewFixed);
        case GcOp::ArrayNewDefault:
          WASM_GC_OP(OpKind::ArrayNewDefault);
        case GcOp::ArrayNewData:
          WASM_GC_OP(OpKind::ArrayNewData);
        case GcOp::ArrayNewElem:
          WASM_GC_OP(OpKind::ArrayNewElem);
        case GcOp::ArrayInitData:
          WASM_GC_OP(OpKind::ArrayInitData);
        case GcOp::ArrayInitElem:
          WASM_GC_OP(OpKind::ArrayInitElem);
        case GcOp::ArrayGet:
        case GcOp::ArrayGetS:
        case GcOp::ArrayGetU:
          WASM_GC_OP(OpKind::ArrayGet);
        case GcOp::ArraySet:
          WASM_GC_OP(OpKind::ArraySet);
        case GcOp::ArrayLen:
          WASM_GC_OP(OpKind::ArrayLen);
        case GcOp::ArrayCopy:
          WASM_GC_OP(OpKind::ArrayCopy);
        case GcOp::ArrayFill:
          WASM_GC_OP(OpKind::ArrayFill);
        case GcOp::RefI31:
        case GcOp::I31GetS:
        case GcOp::I31GetU:
          WASM_GC_OP(OpKind::Conversion);
        case GcOp::RefTest:
        case GcOp::RefTestNull:
          WASM_GC_OP(OpKind::RefTest);
        case GcOp::RefCast:
        case GcOp::RefCastNull:
          WASM_GC_OP(OpKind::RefCast);
        case GcOp::BrOnCast:
        case GcOp::BrOnCastFail:
          WASM_GC_OP(OpKind::BrOnCast);
        case GcOp::AnyConvertExtern:
          WASM_GC_OP(OpKind::RefConversion);
        case GcOp::ExternConvertAny:
          WASM_GC_OP(OpKind::RefConversion);
      }
      break;
    }
    case Op::SimdPrefix: {
      switch (SimdOp(op.b1)) {
        case SimdOp::MozPMADDUBSW:
        case SimdOp::Limit:
          // Reject Limit and reserved codes for SimdPrefix encoding
          break;
        case SimdOp::I8x16ExtractLaneS:
        case SimdOp::I8x16ExtractLaneU:
        case SimdOp::I16x8ExtractLaneS:
        case SimdOp::I16x8ExtractLaneU:
        case SimdOp::I32x4ExtractLane:
        case SimdOp::I64x2ExtractLane:
        case SimdOp::F32x4ExtractLane:
        case SimdOp::F64x2ExtractLane:
          WASM_SIMD_OP(OpKind::ExtractLane);
        case SimdOp::I8x16Splat:
        case SimdOp::I16x8Splat:
        case SimdOp::I32x4Splat:
        case SimdOp::I64x2Splat:
        case SimdOp::F32x4Splat:
        case SimdOp::F64x2Splat:
        case SimdOp::V128AnyTrue:
        case SimdOp::I8x16AllTrue:
        case SimdOp::I16x8AllTrue:
        case SimdOp::I32x4AllTrue:
        case SimdOp::I64x2AllTrue:
        case SimdOp::I8x16Bitmask:
        case SimdOp::I16x8Bitmask:
        case SimdOp::I32x4Bitmask:
        case SimdOp::I64x2Bitmask:
          WASM_SIMD_OP(OpKind::Conversion);
        case SimdOp::I8x16ReplaceLane:
        case SimdOp::I16x8ReplaceLane:
        case SimdOp::I32x4ReplaceLane:
        case SimdOp::I64x2ReplaceLane:
        case SimdOp::F32x4ReplaceLane:
        case SimdOp::F64x2ReplaceLane:
          WASM_SIMD_OP(OpKind::ReplaceLane);
        case SimdOp::I8x16Eq:
        case SimdOp::I8x16Ne:
        case SimdOp::I8x16LtS:
        case SimdOp::I8x16LtU:
        case SimdOp::I8x16GtS:
        case SimdOp::I8x16GtU:
        case SimdOp::I8x16LeS:
        case SimdOp::I8x16LeU:
        case SimdOp::I8x16GeS:
        case SimdOp::I8x16GeU:
        case SimdOp::I16x8Eq:
        case SimdOp::I16x8Ne:
        case SimdOp::I16x8LtS:
        case SimdOp::I16x8LtU:
        case SimdOp::I16x8GtS:
        case SimdOp::I16x8GtU:
        case SimdOp::I16x8LeS:
        case SimdOp::I16x8LeU:
        case SimdOp::I16x8GeS:
        case SimdOp::I16x8GeU:
        case SimdOp::I32x4Eq:
        case SimdOp::I32x4Ne:
        case SimdOp::I32x4LtS:
        case SimdOp::I32x4LtU:
        case SimdOp::I32x4GtS:
        case SimdOp::I32x4GtU:
        case SimdOp::I32x4LeS:
        case SimdOp::I32x4LeU:
        case SimdOp::I32x4GeS:
        case SimdOp::I32x4GeU:
        case SimdOp::I64x2Eq:
        case SimdOp::I64x2Ne:
        case SimdOp::I64x2LtS:
        case SimdOp::I64x2GtS:
        case SimdOp::I64x2LeS:
        case SimdOp::I64x2GeS:
        case SimdOp::F32x4Eq:
        case SimdOp::F32x4Ne:
        case SimdOp::F32x4Lt:
        case SimdOp::F32x4Gt:
        case SimdOp::F32x4Le:
        case SimdOp::F32x4Ge:
        case SimdOp::F64x2Eq:
        case SimdOp::F64x2Ne:
        case SimdOp::F64x2Lt:
        case SimdOp::F64x2Gt:
        case SimdOp::F64x2Le:
        case SimdOp::F64x2Ge:
        case SimdOp::V128And:
        case SimdOp::V128Or:
        case SimdOp::V128Xor:
        case SimdOp::V128AndNot:
        case SimdOp::I8x16AvgrU:
        case SimdOp::I16x8AvgrU:
        case SimdOp::I8x16Add:
        case SimdOp::I8x16AddSatS:
        case SimdOp::I8x16AddSatU:
        case SimdOp::I8x16Sub:
        case SimdOp::I8x16SubSatS:
        case SimdOp::I8x16SubSatU:
        case SimdOp::I8x16MinS:
        case SimdOp::I8x16MaxS:
        case SimdOp::I8x16MinU:
        case SimdOp::I8x16MaxU:
        case SimdOp::I16x8Add:
        case SimdOp::I16x8AddSatS:
        case SimdOp::I16x8AddSatU:
        case SimdOp::I16x8Sub:
        case SimdOp::I16x8SubSatS:
        case SimdOp::I16x8SubSatU:
        case SimdOp::I16x8Mul:
        case SimdOp::I16x8MinS:
        case SimdOp::I16x8MaxS:
        case SimdOp::I16x8MinU:
        case SimdOp::I16x8MaxU:
        case SimdOp::I32x4Add:
        case SimdOp::I32x4Sub:
        case SimdOp::I32x4Mul:
        case SimdOp::I32x4MinS:
        case SimdOp::I32x4MaxS:
        case SimdOp::I32x4MinU:
        case SimdOp::I32x4MaxU:
        case SimdOp::I64x2Add:
        case SimdOp::I64x2Sub:
        case SimdOp::I64x2Mul:
        case SimdOp::F32x4Add:
        case SimdOp::F32x4Sub:
        case SimdOp::F32x4Mul:
        case SimdOp::F32x4Div:
        case SimdOp::F32x4Min:
        case SimdOp::F32x4Max:
        case SimdOp::F64x2Add:
        case SimdOp::F64x2Sub:
        case SimdOp::F64x2Mul:
        case SimdOp::F64x2Div:
        case SimdOp::F64x2Min:
        case SimdOp::F64x2Max:
        case SimdOp::I8x16NarrowI16x8S:
        case SimdOp::I8x16NarrowI16x8U:
        case SimdOp::I16x8NarrowI32x4S:
        case SimdOp::I16x8NarrowI32x4U:
        case SimdOp::I8x16Swizzle:
        case SimdOp::F32x4PMin:
        case SimdOp::F32x4PMax:
        case SimdOp::F64x2PMin:
        case SimdOp::F64x2PMax:
        case SimdOp::I32x4DotI16x8S:
        case SimdOp::I16x8ExtmulLowI8x16S:
        case SimdOp::I16x8ExtmulHighI8x16S:
        case SimdOp::I16x8ExtmulLowI8x16U:
        case SimdOp::I16x8ExtmulHighI8x16U:
        case SimdOp::I32x4ExtmulLowI16x8S:
        case SimdOp::I32x4ExtmulHighI16x8S:
        case SimdOp::I32x4ExtmulLowI16x8U:
        case SimdOp::I32x4ExtmulHighI16x8U:
        case SimdOp::I64x2ExtmulLowI32x4S:
        case SimdOp::I64x2ExtmulHighI32x4S:
        case SimdOp::I64x2ExtmulLowI32x4U:
        case SimdOp::I64x2ExtmulHighI32x4U:
        case SimdOp::I16x8Q15MulrSatS:
        case SimdOp::F32x4RelaxedMin:
        case SimdOp::F32x4RelaxedMax:
        case SimdOp::F64x2RelaxedMin:
        case SimdOp::F64x2RelaxedMax:
        case SimdOp::I8x16RelaxedSwizzle:
        case SimdOp::I16x8RelaxedQ15MulrS:
        case SimdOp::I16x8DotI8x16I7x16S:
          WASM_SIMD_OP(OpKind::Binary);
        case SimdOp::I8x16Neg:
        case SimdOp::I16x8Neg:
        case SimdOp::I16x8ExtendLowI8x16S:
        case SimdOp::I16x8ExtendHighI8x16S:
        case SimdOp::I16x8ExtendLowI8x16U:
        case SimdOp::I16x8ExtendHighI8x16U:
        case SimdOp::I32x4Neg:
        case SimdOp::I32x4ExtendLowI16x8S:
        case SimdOp::I32x4ExtendHighI16x8S:
        case SimdOp::I32x4ExtendLowI16x8U:
        case SimdOp::I32x4ExtendHighI16x8U:
        case SimdOp::I32x4TruncSatF32x4S:
        case SimdOp::I32x4TruncSatF32x4U:
        case SimdOp::I64x2Neg:
        case SimdOp::I64x2ExtendLowI32x4S:
        case SimdOp::I64x2ExtendHighI32x4S:
        case SimdOp::I64x2ExtendLowI32x4U:
        case SimdOp::I64x2ExtendHighI32x4U:
        case SimdOp::F32x4Abs:
        case SimdOp::F32x4Neg:
        case SimdOp::F32x4Sqrt:
        case SimdOp::F32x4ConvertI32x4S:
        case SimdOp::F32x4ConvertI32x4U:
        case SimdOp::F64x2Abs:
        case SimdOp::F64x2Neg:
        case SimdOp::F64x2Sqrt:
        case SimdOp::V128Not:
        case SimdOp::I8x16Popcnt:
        case SimdOp::I8x16Abs:
        case SimdOp::I16x8Abs:
        case SimdOp::I32x4Abs:
        case SimdOp::I64x2Abs:
        case SimdOp::F32x4Ceil:
        case SimdOp::F32x4Floor:
        case SimdOp::F32x4Trunc:
        case SimdOp::F32x4Nearest:
        case SimdOp::F64x2Ceil:
        case SimdOp::F64x2Floor:
        case SimdOp::F64x2Trunc:
        case SimdOp::F64x2Nearest:
        case SimdOp::F32x4DemoteF64x2Zero:
        case SimdOp::F64x2PromoteLowF32x4:
        case SimdOp::F64x2ConvertLowI32x4S:
        case SimdOp::F64x2ConvertLowI32x4U:
        case SimdOp::I32x4TruncSatF64x2SZero:
        case SimdOp::I32x4TruncSatF64x2UZero:
        case SimdOp::I16x8ExtaddPairwiseI8x16S:
        case SimdOp::I16x8ExtaddPairwiseI8x16U:
        case SimdOp::I32x4ExtaddPairwiseI16x8S:
        case SimdOp::I32x4ExtaddPairwiseI16x8U:
        case SimdOp::I32x4RelaxedTruncF32x4S:
        case SimdOp::I32x4RelaxedTruncF32x4U:
        case SimdOp::I32x4RelaxedTruncF64x2SZero:
        case SimdOp::I32x4RelaxedTruncF64x2UZero:
          WASM_SIMD_OP(OpKind::Unary);
        case SimdOp::I8x16Shl:
        case SimdOp::I8x16ShrS:
        case SimdOp::I8x16ShrU:
        case SimdOp::I16x8Shl:
        case SimdOp::I16x8ShrS:
        case SimdOp::I16x8ShrU:
        case SimdOp::I32x4Shl:
        case SimdOp::I32x4ShrS:
        case SimdOp::I32x4ShrU:
        case SimdOp::I64x2Shl:
        case SimdOp::I64x2ShrS:
        case SimdOp::I64x2ShrU:
          WASM_SIMD_OP(OpKind::VectorShift);
        case SimdOp::V128Bitselect:
          WASM_SIMD_OP(OpKind::Ternary);
        case SimdOp::I8x16Shuffle:
          WASM_SIMD_OP(OpKind::VectorShuffle);
        case SimdOp::V128Const:
          WASM_SIMD_OP(OpKind::V128);
        case SimdOp::V128Load:
        case SimdOp::V128Load8Splat:
        case SimdOp::V128Load16Splat:
        case SimdOp::V128Load32Splat:
        case SimdOp::V128Load64Splat:
        case SimdOp::V128Load8x8S:
        case SimdOp::V128Load8x8U:
        case SimdOp::V128Load16x4S:
        case SimdOp::V128Load16x4U:
        case SimdOp::V128Load32x2S:
        case SimdOp::V128Load32x2U:
        case SimdOp::V128Load32Zero:
        case SimdOp::V128Load64Zero:
          WASM_SIMD_OP(OpKind::Load);
        case SimdOp::V128Store:
          WASM_SIMD_OP(OpKind::Store);
        case SimdOp::V128Load8Lane:
        case SimdOp::V128Load16Lane:
        case SimdOp::V128Load32Lane:
        case SimdOp::V128Load64Lane:
          WASM_SIMD_OP(OpKind::LoadLane);
        case SimdOp::V128Store8Lane:
        case SimdOp::V128Store16Lane:
        case SimdOp::V128Store32Lane:
        case SimdOp::V128Store64Lane:
          WASM_SIMD_OP(OpKind::StoreLane);
        case SimdOp::F32x4RelaxedMadd:
        case SimdOp::F32x4RelaxedNmadd:
        case SimdOp::F64x2RelaxedMadd:
        case SimdOp::F64x2RelaxedNmadd:
        case SimdOp::I8x16RelaxedLaneSelect:
        case SimdOp::I16x8RelaxedLaneSelect:
        case SimdOp::I32x4RelaxedLaneSelect:
        case SimdOp::I64x2RelaxedLaneSelect:
        case SimdOp::I32x4DotI8x16I7x16AddS:
          WASM_SIMD_OP(OpKind::Ternary);
      }
      break;
    }
    case Op::MiscPrefix: {
      switch (MiscOp(op.b1)) {
        case MiscOp::Limit:
          // Reject Limit for MiscPrefix encoding
          break;
        case MiscOp::I32TruncSatF32S:
        case MiscOp::I32TruncSatF32U:
        case MiscOp::I32TruncSatF64S:
        case MiscOp::I32TruncSatF64U:
        case MiscOp::I64TruncSatF32S:
        case MiscOp::I64TruncSatF32U:
        case MiscOp::I64TruncSatF64S:
        case MiscOp::I64TruncSatF64U:
          return OpKind::Conversion;
        case MiscOp::MemoryCopy:
        case MiscOp::TableCopy:
          return OpKind::MemOrTableCopy;
        case MiscOp::DataDrop:
        case MiscOp::ElemDrop:
          return OpKind::DataOrElemDrop;
        case MiscOp::MemoryFill:
          return OpKind::MemFill;
        case MiscOp::MemoryInit:
        case MiscOp::TableInit:
          return OpKind::MemOrTableInit;
        case MiscOp::TableFill:
          return OpKind::TableFill;
        case MiscOp::MemoryDiscard:
          return OpKind::MemDiscard;
        case MiscOp::TableGrow:
          return OpKind::TableGrow;
        case MiscOp::TableSize:
          return OpKind::TableSize;
      }
      break;
    }
    case Op::ThreadPrefix: {
      switch (ThreadOp(op.b1)) {
        case ThreadOp::Limit:
          // Reject Limit for ThreadPrefix encoding
          break;
        case ThreadOp::Wake:
          return OpKind::Wake;
        case ThreadOp::I32Wait:
        case ThreadOp::I64Wait:
          return OpKind::Wait;
        case ThreadOp::Fence:
          return OpKind::Fence;
        case ThreadOp::I32AtomicLoad:
        case ThreadOp::I64AtomicLoad:
        case ThreadOp::I32AtomicLoad8U:
        case ThreadOp::I32AtomicLoad16U:
        case ThreadOp::I64AtomicLoad8U:
        case ThreadOp::I64AtomicLoad16U:
        case ThreadOp::I64AtomicLoad32U:
          return OpKind::AtomicLoad;
        case ThreadOp::I32AtomicStore:
        case ThreadOp::I64AtomicStore:
        case ThreadOp::I32AtomicStore8U:
        case ThreadOp::I32AtomicStore16U:
        case ThreadOp::I64AtomicStore8U:
        case ThreadOp::I64AtomicStore16U:
        case ThreadOp::I64AtomicStore32U:
          return OpKind::AtomicStore;
        case ThreadOp::I32AtomicAdd:
        case ThreadOp::I64AtomicAdd:
        case ThreadOp::I32AtomicAdd8U:
        case ThreadOp::I32AtomicAdd16U:
        case ThreadOp::I64AtomicAdd8U:
        case ThreadOp::I64AtomicAdd16U:
        case ThreadOp::I64AtomicAdd32U:
        case ThreadOp::I32AtomicSub:
        case ThreadOp::I64AtomicSub:
        case ThreadOp::I32AtomicSub8U:
        case ThreadOp::I32AtomicSub16U:
        case ThreadOp::I64AtomicSub8U:
        case ThreadOp::I64AtomicSub16U:
        case ThreadOp::I64AtomicSub32U:
        case ThreadOp::I32AtomicAnd:
        case ThreadOp::I64AtomicAnd:
        case ThreadOp::I32AtomicAnd8U:
        case ThreadOp::I32AtomicAnd16U:
        case ThreadOp::I64AtomicAnd8U:
        case ThreadOp::I64AtomicAnd16U:
        case ThreadOp::I64AtomicAnd32U:
        case ThreadOp::I32AtomicOr:
        case ThreadOp::I64AtomicOr:
        case ThreadOp::I32AtomicOr8U:
        case ThreadOp::I32AtomicOr16U:
        case ThreadOp::I64AtomicOr8U:
        case ThreadOp::I64AtomicOr16U:
        case ThreadOp::I64AtomicOr32U:
        case ThreadOp::I32AtomicXor:
        case ThreadOp::I64AtomicXor:
        case ThreadOp::I32AtomicXor8U:
        case ThreadOp::I32AtomicXor16U:
        case ThreadOp::I64AtomicXor8U:
        case ThreadOp::I64AtomicXor16U:
        case ThreadOp::I64AtomicXor32U:
        case ThreadOp::I32AtomicXchg:
        case ThreadOp::I64AtomicXchg:
        case ThreadOp::I32AtomicXchg8U:
        case ThreadOp::I32AtomicXchg16U:
        case ThreadOp::I64AtomicXchg8U:
        case ThreadOp::I64AtomicXchg16U:
        case ThreadOp::I64AtomicXchg32U:
          return OpKind::AtomicBinOp;
        case ThreadOp::I32AtomicCmpXchg:
        case ThreadOp::I64AtomicCmpXchg:
        case ThreadOp::I32AtomicCmpXchg8U:
        case ThreadOp::I32AtomicCmpXchg16U:
        case ThreadOp::I64AtomicCmpXchg8U:
        case ThreadOp::I64AtomicCmpXchg16U:
        case ThreadOp::I64AtomicCmpXchg32U:
          return OpKind::AtomicCompareExchange;
        default:
          break;
      }
      break;
    }
    case Op::MozPrefix: {
      switch (MozOp(op.b1)) {
        case MozOp::Limit:
          // Reject Limit for the MozPrefix encoding
          break;
        case MozOp::TeeGlobal:
          return OpKind::TeeGlobal;
        case MozOp::I32BitNot:
        case MozOp::I32Abs:
        case MozOp::I32Neg:
          return OpKind::Unary;
        case MozOp::I32Min:
        case MozOp::I32Max:
        case MozOp::F64Mod:
        case MozOp::F64Pow:
        case MozOp::F64Atan2:
          return OpKind::Binary;
        case MozOp::F64SinNative:
        case MozOp::F64SinFdlibm:
        case MozOp::F64CosNative:
        case MozOp::F64CosFdlibm:
        case MozOp::F64TanNative:
        case MozOp::F64TanFdlibm:
        case MozOp::F64Asin:
        case MozOp::F64Acos:
        case MozOp::F64Atan:
        case MozOp::F64Exp:
        case MozOp::F64Log:
          return OpKind::Unary;
        case MozOp::I32TeeStore8:
        case MozOp::I32TeeStore16:
        case MozOp::I64TeeStore8:
        case MozOp::I64TeeStore16:
        case MozOp::I64TeeStore32:
        case MozOp::I32TeeStore:
        case MozOp::I64TeeStore:
        case MozOp::F32TeeStore:
        case MozOp::F64TeeStore:
        case MozOp::F32TeeStoreF64:
        case MozOp::F64TeeStoreF32:
          return OpKind::TeeStore;
        case MozOp::OldCallDirect:
          return OpKind::OldCallDirect;
        case MozOp::OldCallIndirect:
          return OpKind::OldCallIndirect;
        case MozOp::CallBuiltinModuleFunc:
          return OpKind::CallBuiltinModuleFunc;
        case MozOp::StackSwitch:
          return OpKind::StackSwitch;
      }
      break;
    }
    case Op::FirstPrefix:
      break;
  }
  MOZ_CRASH("unimplemented opcode");
}

#  undef WASM_GC_OP
#  undef WASM_REF_OP

#endif  // DEBUG

bool UnsetLocalsState::init(const ValTypeVector& locals, size_t numParams) {
  MOZ_ASSERT(setLocalsStack_.empty());

  // Find the first and total count of non-defaultable locals.
  size_t firstNonDefaultable = UINT32_MAX;
  size_t countNonDefaultable = 0;
  for (size_t i = numParams; i < locals.length(); i++) {
    if (!locals[i].isDefaultable()) {
      firstNonDefaultable = std::min(i, firstNonDefaultable);
      countNonDefaultable++;
    }
  }
  firstNonDefaultLocal_ = firstNonDefaultable;
  if (countNonDefaultable == 0) {
    // No locals to track, saving CPU cycles.
    MOZ_ASSERT(firstNonDefaultable == UINT32_MAX);
    return true;
  }

  // setLocalsStack_ cannot be deeper than amount of non-defaultable locals.
  if (!setLocalsStack_.reserve(countNonDefaultable)) {
    return false;
  }

  // Allocate a bitmap for locals starting at the first non-defaultable local.
  size_t bitmapSize =
      ((locals.length() - firstNonDefaultable) + (WordBits - 1)) / WordBits;
  if (!unsetLocals_.resize(bitmapSize)) {
    return false;
  }
  memset(unsetLocals_.begin(), 0, bitmapSize * WordSize);
  for (size_t i = firstNonDefaultable; i < locals.length(); i++) {
    if (!locals[i].isDefaultable()) {
      size_t localUnsetIndex = i - firstNonDefaultable;
      unsetLocals_[localUnsetIndex / WordBits] |=
          1 << (localUnsetIndex % WordBits);
    }
  }
  return true;
}
