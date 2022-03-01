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

using namespace js;
using namespace js::jit;
using namespace js::wasm;

#ifdef ENABLE_WASM_GC
#  ifndef ENABLE_WASM_FUNCTION_REFERENCES
#    error "GC types require the function-references feature"
#  endif
#endif

#ifdef DEBUG

#  ifdef ENABLE_WASM_FUNCTION_REFERENCES
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
#  ifdef ENABLE_WASM_EXCEPTIONS
#    define WASM_EXN_OP(code) return code
#  else
#    define WASM_EXN_OP(code) break
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
    case Op::I32TruncSF32:
    case Op::I32TruncUF32:
    case Op::I32ReinterpretF32:
    case Op::I32TruncSF64:
    case Op::I32TruncUF64:
    case Op::I64ExtendSI32:
    case Op::I64ExtendUI32:
    case Op::I64TruncSF32:
    case Op::I64TruncUF32:
    case Op::I64TruncSF64:
    case Op::I64TruncUF64:
    case Op::I64ReinterpretF64:
    case Op::I64Eqz:
    case Op::F32ConvertSI32:
    case Op::F32ConvertUI32:
    case Op::F32ReinterpretI32:
    case Op::F32ConvertSI64:
    case Op::F32ConvertUI64:
    case Op::F32DemoteF64:
    case Op::F64ConvertSI32:
    case Op::F64ConvertUI32:
    case Op::F64ConvertSI64:
    case Op::F64ConvertUI64:
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
    case Op::GetLocal:
      return OpKind::GetLocal;
    case Op::SetLocal:
      return OpKind::SetLocal;
    case Op::TeeLocal:
      return OpKind::TeeLocal;
    case Op::GetGlobal:
      return OpKind::GetGlobal;
    case Op::SetGlobal:
      return OpKind::SetGlobal;
    case Op::TableGet:
      return OpKind::TableGet;
    case Op::TableSet:
      return OpKind::TableSet;
    case Op::Call:
      return OpKind::Call;
    case Op::CallIndirect:
      return OpKind::CallIndirect;
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
#  ifdef ENABLE_WASM_EXCEPTIONS
    case Op::Catch:
      WASM_EXN_OP(OpKind::Catch);
    case Op::CatchAll:
      WASM_EXN_OP(OpKind::CatchAll);
    case Op::Delegate:
      WASM_EXN_OP(OpKind::Delegate);
    case Op::Throw:
      WASM_EXN_OP(OpKind::Throw);
    case Op::Rethrow:
      WASM_EXN_OP(OpKind::Rethrow);
    case Op::Try:
      WASM_EXN_OP(OpKind::Try);
#  endif
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
    case Op::RefEq:
      WASM_GC_OP(OpKind::Comparison);
    case Op::GcPrefix: {
      switch (GcOp(op.b1)) {
        case GcOp::Limit:
          // Reject Limit for GcPrefix encoding
          break;
        case GcOp::StructNewWithRtt:
          WASM_GC_OP(OpKind::StructNewWithRtt);
        case GcOp::StructNewDefaultWithRtt:
          WASM_GC_OP(OpKind::StructNewDefaultWithRtt);
        case GcOp::StructGet:
        case GcOp::StructGetS:
        case GcOp::StructGetU:
          WASM_GC_OP(OpKind::StructGet);
        case GcOp::StructSet:
          WASM_GC_OP(OpKind::StructSet);
        case GcOp::ArrayNewWithRtt:
          WASM_GC_OP(OpKind::ArrayNewWithRtt);
        case GcOp::ArrayNewDefaultWithRtt:
          WASM_GC_OP(OpKind::ArrayNewDefaultWithRtt);
        case GcOp::ArrayGet:
        case GcOp::ArrayGetS:
        case GcOp::ArrayGetU:
          WASM_GC_OP(OpKind::ArrayGet);
        case GcOp::ArraySet:
          WASM_GC_OP(OpKind::ArraySet);
        case GcOp::ArrayLen:
          WASM_GC_OP(OpKind::ArrayLen);
        case GcOp::RttCanon:
          WASM_GC_OP(OpKind::RttCanon);
        case GcOp::RttSub:
          WASM_GC_OP(OpKind::RttSub);
        case GcOp::RefTest:
          WASM_GC_OP(OpKind::RefTest);
        case GcOp::RefCast:
          WASM_GC_OP(OpKind::RefCast);
        case GcOp::BrOnCast:
          WASM_GC_OP(OpKind::BrOnCast);
      }
      break;
    }
    case Op::SimdPrefix: {
      switch (SimdOp(op.b1)) {
        case SimdOp::Limit:
          // Reject Limit for SimdPrefix encoding
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
        case SimdOp::I8x16AddSaturateS:
        case SimdOp::I8x16AddSaturateU:
        case SimdOp::I8x16Sub:
        case SimdOp::I8x16SubSaturateS:
        case SimdOp::I8x16SubSaturateU:
        case SimdOp::I8x16MinS:
        case SimdOp::I8x16MaxS:
        case SimdOp::I8x16MinU:
        case SimdOp::I8x16MaxU:
        case SimdOp::I16x8Add:
        case SimdOp::I16x8AddSaturateS:
        case SimdOp::I16x8AddSaturateU:
        case SimdOp::I16x8Sub:
        case SimdOp::I16x8SubSaturateS:
        case SimdOp::I16x8SubSaturateU:
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
        case SimdOp::I8x16NarrowSI16x8:
        case SimdOp::I8x16NarrowUI16x8:
        case SimdOp::I16x8NarrowSI32x4:
        case SimdOp::I16x8NarrowUI32x4:
        case SimdOp::V8x16Swizzle:
        case SimdOp::F32x4PMin:
        case SimdOp::F32x4PMax:
        case SimdOp::F64x2PMin:
        case SimdOp::F64x2PMax:
        case SimdOp::I32x4DotSI16x8:
        case SimdOp::I16x8ExtMulLowSI8x16:
        case SimdOp::I16x8ExtMulHighSI8x16:
        case SimdOp::I16x8ExtMulLowUI8x16:
        case SimdOp::I16x8ExtMulHighUI8x16:
        case SimdOp::I32x4ExtMulLowSI16x8:
        case SimdOp::I32x4ExtMulHighSI16x8:
        case SimdOp::I32x4ExtMulLowUI16x8:
        case SimdOp::I32x4ExtMulHighUI16x8:
        case SimdOp::I64x2ExtMulLowSI32x4:
        case SimdOp::I64x2ExtMulHighSI32x4:
        case SimdOp::I64x2ExtMulLowUI32x4:
        case SimdOp::I64x2ExtMulHighUI32x4:
        case SimdOp::I16x8Q15MulrSatS:
          WASM_SIMD_OP(OpKind::Binary);
        case SimdOp::I8x16Neg:
        case SimdOp::I16x8Neg:
        case SimdOp::I16x8WidenLowSI8x16:
        case SimdOp::I16x8WidenHighSI8x16:
        case SimdOp::I16x8WidenLowUI8x16:
        case SimdOp::I16x8WidenHighUI8x16:
        case SimdOp::I32x4Neg:
        case SimdOp::I32x4WidenLowSI16x8:
        case SimdOp::I32x4WidenHighSI16x8:
        case SimdOp::I32x4WidenLowUI16x8:
        case SimdOp::I32x4WidenHighUI16x8:
        case SimdOp::I32x4TruncSSatF32x4:
        case SimdOp::I32x4TruncUSatF32x4:
        case SimdOp::I64x2Neg:
        case SimdOp::I64x2WidenLowSI32x4:
        case SimdOp::I64x2WidenHighSI32x4:
        case SimdOp::I64x2WidenLowUI32x4:
        case SimdOp::I64x2WidenHighUI32x4:
        case SimdOp::F32x4Abs:
        case SimdOp::F32x4Neg:
        case SimdOp::F32x4Sqrt:
        case SimdOp::F32x4ConvertSI32x4:
        case SimdOp::F32x4ConvertUI32x4:
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
        case SimdOp::I16x8ExtAddPairwiseI8x16S:
        case SimdOp::I16x8ExtAddPairwiseI8x16U:
        case SimdOp::I32x4ExtAddPairwiseI16x8S:
        case SimdOp::I32x4ExtAddPairwiseI16x8U:
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
          WASM_SIMD_OP(OpKind::VectorSelect);
        case SimdOp::V8x16Shuffle:
          WASM_SIMD_OP(OpKind::VectorShuffle);
        case SimdOp::V128Const:
          WASM_SIMD_OP(OpKind::V128);
        case SimdOp::V128Load:
        case SimdOp::V8x16LoadSplat:
        case SimdOp::V16x8LoadSplat:
        case SimdOp::V32x4LoadSplat:
        case SimdOp::V64x2LoadSplat:
        case SimdOp::I16x8LoadS8x8:
        case SimdOp::I16x8LoadU8x8:
        case SimdOp::I32x4LoadS16x4:
        case SimdOp::I32x4LoadU16x4:
        case SimdOp::I64x2LoadS32x2:
        case SimdOp::I64x2LoadU32x2:
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
#  ifdef ENABLE_WASM_SIMD_WORMHOLE
        case SimdOp::MozWHSELFTEST:
        case SimdOp::MozWHPMADDUBSW:
        case SimdOp::MozWHPMADDWD:
          MOZ_CRASH("Should not be seen");
#  endif
      }
      break;
    }
    case Op::MiscPrefix: {
      switch (MiscOp(op.b1)) {
        case MiscOp::Limit:
          // Reject Limit for MiscPrefix encoding
          break;
        case MiscOp::I32TruncSSatF32:
        case MiscOp::I32TruncUSatF32:
        case MiscOp::I32TruncSSatF64:
        case MiscOp::I32TruncUSatF64:
        case MiscOp::I64TruncSSatF32:
        case MiscOp::I64TruncUSatF32:
        case MiscOp::I64TruncSSatF64:
        case MiscOp::I64TruncUSatF64:
          return OpKind::Conversion;
        case MiscOp::MemCopy:
        case MiscOp::TableCopy:
          return OpKind::MemOrTableCopy;
        case MiscOp::DataDrop:
        case MiscOp::ElemDrop:
          return OpKind::DataOrElemDrop;
        case MiscOp::MemFill:
          return OpKind::MemFill;
        case MiscOp::MemInit:
        case MiscOp::TableInit:
          return OpKind::MemOrTableInit;
        case MiscOp::TableFill:
          return OpKind::TableFill;
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
        case MozOp::F64Sin:
        case MozOp::F64Cos:
        case MozOp::F64Tan:
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
      }
      break;
    }
  }
  MOZ_CRASH("unimplemented opcode");
}

#  undef WASM_EXN_OP
#  undef WASM_GC_OP
#  undef WASM_REF_OP

#endif
