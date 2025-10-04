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

#ifndef wasm_constants_h
#define wasm_constants_h

#include <stdint.h>

#include "wasm/WasmBuiltinModuleGenerated.h"
#include "wasm/WasmSerialize.h"

namespace js {
namespace wasm {

static const uint32_t MagicNumber = 0x6d736100;  // "\0asm"
static const uint32_t EncodingVersion = 0x01;

enum class SectionId {
  Custom = 0,
  Type = 1,
  Import = 2,
  Function = 3,
  Table = 4,
  Memory = 5,
  Global = 6,
  Export = 7,
  Start = 8,
  Elem = 9,
  Code = 10,
  Data = 11,
  DataCount = 12,
  Tag = 13,
};

// WebAssembly type encodings are all single-byte negative SLEB128s, hence:
//  forall tc:TypeCode. ((tc & SLEB128SignMask) == SLEB128SignBit
static const uint8_t SLEB128SignMask = 0xc0;
static const uint8_t SLEB128SignBit = 0x40;

enum class TypeCode {

  // If more "simple primitive" (non-reference, non-constructor,
  // non-special-purpose) types are added here then you MUST update
  // LowestPrimitiveTypeCode, below.

  I32 = 0x7f,   // SLEB128(-0x01)
  I64 = 0x7e,   // SLEB128(-0x02)
  F32 = 0x7d,   // SLEB128(-0x03)
  F64 = 0x7c,   // SLEB128(-0x04)
  V128 = 0x7b,  // SLEB128(-0x05)

  I8 = 0x78,   // SLEB128(-0x08)
  I16 = 0x77,  // SLEB128(-0x09)

  // A function pointer with any signature
  FuncRef = 0x70,  // SLEB128(-0x10)

  // A reference to any host value.
  ExternRef = 0x6f,  // SLEB128(-0x11)

  // A reference to any wasm gc value.
  AnyRef = 0x6e,  // SLEB128(-0x12)

  // A reference to a struct/array value.
  EqRef = 0x6d,  // SLEB128(-0x13)

  // Type constructor for nullable reference types.
  NullableRef = 0x63,  // SLEB128(-0x1D)

  // Type constructor for non-nullable reference types.
  Ref = 0x64,  // SLEB128(-0x1C)

  // A reference to an unboxed 31-bit integer.
  I31Ref = 0x6c,  // SLEB128(-0x14)

  // A null reference in the extern hierarchy.
  NullExternRef = 0x72,  // SLEB128(-0x0E)

  // A null reference in the func hierarchy.
  NullFuncRef = 0x73,  // SLEB128(-0x0D)

  // A null reference in the exn hierarchy.
  NullExnRef = 0x74,  // SLEB128(-0x0C)

  // A reference to any struct value.
  StructRef = 0x6b,  // SLEB128(-0x15)

  // A reference to any array value.
  ArrayRef = 0x6a,  // SLEB128(-0x16)

  // A reference to an exception value.
  ExnRef = 0x69,  // SLEB128(-0x17)

  // A null reference in the any hierarchy.
  NullAnyRef = 0x71,  // SLEB128(-0x0F)

  // Type constructor for function types
  Func = 0x60,  // SLEB128(-0x20)

  // Type constructor for structure types - gc proposal
  Struct = 0x5f,  // SLEB128(-0x21)

  // Type constructor for array types - gc proposal
  Array = 0x5e,  // SLEB128(-0x22)

  // Value for non-nullable type present.
  TableHasInitExpr = 0x40,

  // The 'empty' case of blocktype.
  BlockVoid = 0x40,  // SLEB128(-0x40)

  // Type constructor for recursion groups - gc proposal
  RecGroup = 0x4e,  // SLEB128(-0x31)

  // Type prefix for parent types - gc proposal
  SubNoFinalType = 0x50,  // SLEB128(-0x30)

  // Type prefix for final types - gc proposal
  SubFinalType = 0x4f,  // SLEB128(-0x32)

  Limit = 0x80
};

// This is the lowest-valued TypeCode that is a primitive type, used in
// UnpackTypeCodeTypeAbstracted().  If primitive typecodes are added below any
// reference typecode then the logic in that function MUST change.

static constexpr TypeCode LowestPrimitiveTypeCode = TypeCode::I16;

// An arbitrary reference type used as the result of
// UnpackTypeCodeTypeAbstracted() when a value type is a reference.

static constexpr TypeCode AbstractReferenceTypeCode = TypeCode::ExternRef;

// A type code used to represent (ref null? T) whether or not the type
// is encoded with 'Ref' or 'NullableRef'.

static constexpr TypeCode AbstractTypeRefCode = TypeCode::Ref;

// wasm traps are machine instructions that can fail to execute for reasons
// that have to do with some condition in the wasm that they were compiled
// from.  The failure manifests as a (machine-level) exception of some sort,
// which leads execution to a signal handler, and is eventually reported as a
// WebAssembly.RuntimeError.  Generated code may also jump to a Trap
// symbolically, passing the bytecode offset to report as the trap offset.
// The generated jump will be bound to a tiny stub which fills the offset and
// then jumps to a per-Trap shared stub at the end of the module.
//
// Traps are described by a value from Trap and, in debug builds only, a value
// from TrapInsn.
//
// * A Trap indicates why the trap has happened and is used to construct the
//   WebAssembly.Runtime message.
//
// * A TrapMachineInsn (not defined in this file) describes roughly what kind
//   of machine instruction has caused the trap.  This is used only for
//   validation of trap placement in debug builds, in
//   ModuleGenerator::finishMetadataTier, and is not necessary for execution
//   of wasm code.

enum class Trap {
  // The Unreachable opcode has been executed.
  Unreachable,
  // An integer arithmetic operation led to an overflow.
  IntegerOverflow,
  // Trying to coerce NaN to an integer.
  InvalidConversionToInteger,
  // Integer division by zero.
  IntegerDivideByZero,
  // Out of bounds on wasm memory accesses.
  OutOfBounds,
  // Unaligned on wasm atomic accesses; also used for non-standard ARM
  // unaligned access faults.
  UnalignedAccess,
  // call_indirect to null.
  IndirectCallToNull,
  // call_indirect signature mismatch.
  IndirectCallBadSig,
  // Dereference null pointer in operation on (Ref T)
  NullPointerDereference,
  // Failed to cast a (Ref T) in a ref.cast instruction
  BadCast,

  // The internal stack space was exhausted. For compatibility, this throws
  // the same over-recursed error as JS.
  StackOverflow,

  // The wasm execution has potentially run too long and the engine must call
  // CheckForInterrupt(). This trap is resumable.
  CheckInterrupt,

  // Signal an error that was reported in C++ code.
  ThrowReported,

  Limit
};

enum class DefinitionKind {
  Function = 0x00,
  Table = 0x01,
  Memory = 0x02,
  Global = 0x03,
  Tag = 0x04,
};

enum class GlobalTypeImmediate { IsMutable = 0x1, AllowedMask = 0x1 };

enum class LimitsFlags {
  Default = 0x0,
  HasMaximum = 0x1,
  IsShared = 0x2,
  IsI64 = 0x4,
};

enum class LimitsMask {
  Table = uint8_t(LimitsFlags::HasMaximum),
#ifdef ENABLE_WASM_MEMORY64
  Memory = uint8_t(LimitsFlags::HasMaximum) | uint8_t(LimitsFlags::IsShared) |
           uint8_t(LimitsFlags::IsI64),
#else
  Memory = uint8_t(LimitsFlags::HasMaximum) | uint8_t(LimitsFlags::IsShared),
#endif
};

enum class DataSegmentKind {
  Active = 0x00,
  Passive = 0x01,
  ActiveWithMemoryIndex = 0x02
};

enum class ElemSegmentKind : uint32_t {
  Active = 0x0,
  Passive = 0x1,
  ActiveWithTableIndex = 0x2,
  Declared = 0x3,
};

enum class ElemSegmentPayload : uint32_t {
  Indices = 0x0,
  Expressions = 0x4,
};

enum class TagKind {
  Exception = 0x0,
};

enum class Op {
  // Control flow operators
  Unreachable = 0x00,
  Nop = 0x01,
  Block = 0x02,
  Loop = 0x03,
  If = 0x04,
  Else = 0x05,
  Try = 0x06,
  Catch = 0x07,
  Throw = 0x08,
  Rethrow = 0x09,
  ThrowRef = 0x0a,
  End = 0x0b,
  Br = 0x0c,
  BrIf = 0x0d,
  BrTable = 0x0e,
  Return = 0x0f,

  // Call operators
  Call = 0x10,
  CallIndirect = 0x11,
  ReturnCall = 0x12,
  ReturnCallIndirect = 0x13,
  CallRef = 0x14,
  ReturnCallRef = 0x15,

  // Additional exception operators
  Delegate = 0x18,
  CatchAll = 0x19,

  // Parametric operators
  Drop = 0x1a,
  SelectNumeric = 0x1b,
  SelectTyped = 0x1c,

  // Additional exception operators
  TryTable = 0x1f,

  // Variable access
  LocalGet = 0x20,
  LocalSet = 0x21,
  LocalTee = 0x22,
  GlobalGet = 0x23,
  GlobalSet = 0x24,
  TableGet = 0x25,  // Reftypes,
  TableSet = 0x26,  //   per proposal as of February 2019

  // Memory-related operators
  I32Load = 0x28,
  I64Load = 0x29,
  F32Load = 0x2a,
  F64Load = 0x2b,
  I32Load8S = 0x2c,
  I32Load8U = 0x2d,
  I32Load16S = 0x2e,
  I32Load16U = 0x2f,
  I64Load8S = 0x30,
  I64Load8U = 0x31,
  I64Load16S = 0x32,
  I64Load16U = 0x33,
  I64Load32S = 0x34,
  I64Load32U = 0x35,
  I32Store = 0x36,
  I64Store = 0x37,
  F32Store = 0x38,
  F64Store = 0x39,
  I32Store8 = 0x3a,
  I32Store16 = 0x3b,
  I64Store8 = 0x3c,
  I64Store16 = 0x3d,
  I64Store32 = 0x3e,
  MemorySize = 0x3f,
  MemoryGrow = 0x40,

  // Constants
  I32Const = 0x41,
  I64Const = 0x42,
  F32Const = 0x43,
  F64Const = 0x44,

  // Comparison operators
  I32Eqz = 0x45,
  I32Eq = 0x46,
  I32Ne = 0x47,
  I32LtS = 0x48,
  I32LtU = 0x49,
  I32GtS = 0x4a,
  I32GtU = 0x4b,
  I32LeS = 0x4c,
  I32LeU = 0x4d,
  I32GeS = 0x4e,
  I32GeU = 0x4f,
  I64Eqz = 0x50,
  I64Eq = 0x51,
  I64Ne = 0x52,
  I64LtS = 0x53,
  I64LtU = 0x54,
  I64GtS = 0x55,
  I64GtU = 0x56,
  I64LeS = 0x57,
  I64LeU = 0x58,
  I64GeS = 0x59,
  I64GeU = 0x5a,
  F32Eq = 0x5b,
  F32Ne = 0x5c,
  F32Lt = 0x5d,
  F32Gt = 0x5e,
  F32Le = 0x5f,
  F32Ge = 0x60,
  F64Eq = 0x61,
  F64Ne = 0x62,
  F64Lt = 0x63,
  F64Gt = 0x64,
  F64Le = 0x65,
  F64Ge = 0x66,

  // Numeric operators
  I32Clz = 0x67,
  I32Ctz = 0x68,
  I32Popcnt = 0x69,
  I32Add = 0x6a,
  I32Sub = 0x6b,
  I32Mul = 0x6c,
  I32DivS = 0x6d,
  I32DivU = 0x6e,
  I32RemS = 0x6f,
  I32RemU = 0x70,
  I32And = 0x71,
  I32Or = 0x72,
  I32Xor = 0x73,
  I32Shl = 0x74,
  I32ShrS = 0x75,
  I32ShrU = 0x76,
  I32Rotl = 0x77,
  I32Rotr = 0x78,
  I64Clz = 0x79,
  I64Ctz = 0x7a,
  I64Popcnt = 0x7b,
  I64Add = 0x7c,
  I64Sub = 0x7d,
  I64Mul = 0x7e,
  I64DivS = 0x7f,
  I64DivU = 0x80,
  I64RemS = 0x81,
  I64RemU = 0x82,
  I64And = 0x83,
  I64Or = 0x84,
  I64Xor = 0x85,
  I64Shl = 0x86,
  I64ShrS = 0x87,
  I64ShrU = 0x88,
  I64Rotl = 0x89,
  I64Rotr = 0x8a,
  F32Abs = 0x8b,
  F32Neg = 0x8c,
  F32Ceil = 0x8d,
  F32Floor = 0x8e,
  F32Trunc = 0x8f,
  F32Nearest = 0x90,
  F32Sqrt = 0x91,
  F32Add = 0x92,
  F32Sub = 0x93,
  F32Mul = 0x94,
  F32Div = 0x95,
  F32Min = 0x96,
  F32Max = 0x97,
  F32CopySign = 0x98,
  F64Abs = 0x99,
  F64Neg = 0x9a,
  F64Ceil = 0x9b,
  F64Floor = 0x9c,
  F64Trunc = 0x9d,
  F64Nearest = 0x9e,
  F64Sqrt = 0x9f,
  F64Add = 0xa0,
  F64Sub = 0xa1,
  F64Mul = 0xa2,
  F64Div = 0xa3,
  F64Min = 0xa4,
  F64Max = 0xa5,
  F64CopySign = 0xa6,

  // Conversions
  I32WrapI64 = 0xa7,
  I32TruncF32S = 0xa8,
  I32TruncF32U = 0xa9,
  I32TruncF64S = 0xaa,
  I32TruncF64U = 0xab,
  I64ExtendI32S = 0xac,
  I64ExtendI32U = 0xad,
  I64TruncF32S = 0xae,
  I64TruncF32U = 0xaf,
  I64TruncF64S = 0xb0,
  I64TruncF64U = 0xb1,
  F32ConvertI32S = 0xb2,
  F32ConvertI32U = 0xb3,
  F32ConvertI64S = 0xb4,
  F32ConvertI64U = 0xb5,
  F32DemoteF64 = 0xb6,
  F64ConvertI32S = 0xb7,
  F64ConvertI32U = 0xb8,
  F64ConvertI64S = 0xb9,
  F64ConvertI64U = 0xba,
  F64PromoteF32 = 0xbb,

  // Reinterpretations
  I32ReinterpretF32 = 0xbc,
  I64ReinterpretF64 = 0xbd,
  F32ReinterpretI32 = 0xbe,
  F64ReinterpretI64 = 0xbf,

  // Sign extension
  I32Extend8S = 0xc0,
  I32Extend16S = 0xc1,
  I64Extend8S = 0xc2,
  I64Extend16S = 0xc3,
  I64Extend32S = 0xc4,

  // Reference types
  RefNull = 0xd0,
  RefIsNull = 0xd1,
  RefFunc = 0xd2,

  // Function references
  RefAsNonNull = 0xd4,
  BrOnNull = 0xd5,

  // GC (experimental)
  RefEq = 0xd3,

  // Function references
  BrOnNonNull = 0xd6,

  FirstPrefix = 0xfa,
  GcPrefix = 0xfb,
  MiscPrefix = 0xfc,
  SimdPrefix = 0xfd,
  ThreadPrefix = 0xfe,
  MozPrefix = 0xff,

  Limit = 0x100
};

inline bool IsPrefixByte(uint8_t b) { return b >= uint8_t(Op::FirstPrefix); }

// Opcodes in the GC opcode space.
enum class GcOp {
  // Structure operations
  StructNew = 0x0,
  StructNewDefault = 0x1,
  StructGet = 0x02,
  StructGetS = 0x03,
  StructGetU = 0x04,
  StructSet = 0x05,

  // Array operations
  ArrayNew = 0x6,
  ArrayNewDefault = 0x7,
  ArrayNewFixed = 0x8,
  ArrayNewData = 0x9,
  ArrayNewElem = 0xa,
  ArrayGet = 0xb,
  ArrayGetS = 0xc,
  ArrayGetU = 0xd,
  ArraySet = 0xe,
  ArrayLen = 0xf,
  ArrayFill = 0x10,
  ArrayCopy = 0x11,
  ArrayInitData = 0x12,
  ArrayInitElem = 0x13,

  // Ref operations
  RefTest = 0x14,
  RefTestNull = 0x15,
  RefCast = 0x16,
  RefCastNull = 0x17,
  BrOnCast = 0x18,
  BrOnCastFail = 0x19,

  // Extern/any coercion operations
  AnyConvertExtern = 0x1a,
  ExternConvertAny = 0x1b,

  // I31 operations
  RefI31 = 0x1c,
  I31GetS = 0x1d,
  I31GetU = 0x1e,

  Limit
};

// Opcode list from the SIMD proposal post-renumbering in May, 2020.

// Opcodes with suffix 'Experimental' are proposed but not standardized, and are
// compatible with those same opcodes in V8.  No opcode labeled 'Experimental'
// will ship in a Release build where SIMD is enabled by default.

enum class SimdOp {
  V128Load = 0x00,
  V128Load8x8S = 0x01,
  V128Load8x8U = 0x02,
  V128Load16x4S = 0x03,
  V128Load16x4U = 0x04,
  V128Load32x2S = 0x05,
  V128Load32x2U = 0x06,
  V128Load8Splat = 0x07,
  V128Load16Splat = 0x08,
  V128Load32Splat = 0x09,
  V128Load64Splat = 0x0a,
  V128Store = 0x0b,
  V128Const = 0x0c,
  I8x16Shuffle = 0x0d,
  I8x16Swizzle = 0x0e,
  I8x16Splat = 0x0f,
  I16x8Splat = 0x10,
  I32x4Splat = 0x11,
  I64x2Splat = 0x12,
  F32x4Splat = 0x13,
  F64x2Splat = 0x14,
  I8x16ExtractLaneS = 0x15,
  I8x16ExtractLaneU = 0x16,
  I8x16ReplaceLane = 0x17,
  I16x8ExtractLaneS = 0x18,
  I16x8ExtractLaneU = 0x19,
  I16x8ReplaceLane = 0x1a,
  I32x4ExtractLane = 0x1b,
  I32x4ReplaceLane = 0x1c,
  I64x2ExtractLane = 0x1d,
  I64x2ReplaceLane = 0x1e,
  F32x4ExtractLane = 0x1f,
  F32x4ReplaceLane = 0x20,
  F64x2ExtractLane = 0x21,
  F64x2ReplaceLane = 0x22,
  I8x16Eq = 0x23,
  I8x16Ne = 0x24,
  I8x16LtS = 0x25,
  I8x16LtU = 0x26,
  I8x16GtS = 0x27,
  I8x16GtU = 0x28,
  I8x16LeS = 0x29,
  I8x16LeU = 0x2a,
  I8x16GeS = 0x2b,
  I8x16GeU = 0x2c,
  I16x8Eq = 0x2d,
  I16x8Ne = 0x2e,
  I16x8LtS = 0x2f,
  I16x8LtU = 0x30,
  I16x8GtS = 0x31,
  I16x8GtU = 0x32,
  I16x8LeS = 0x33,
  I16x8LeU = 0x34,
  I16x8GeS = 0x35,
  I16x8GeU = 0x36,
  I32x4Eq = 0x37,
  I32x4Ne = 0x38,
  I32x4LtS = 0x39,
  I32x4LtU = 0x3a,
  I32x4GtS = 0x3b,
  I32x4GtU = 0x3c,
  I32x4LeS = 0x3d,
  I32x4LeU = 0x3e,
  I32x4GeS = 0x3f,
  I32x4GeU = 0x40,
  F32x4Eq = 0x41,
  F32x4Ne = 0x42,
  F32x4Lt = 0x43,
  F32x4Gt = 0x44,
  F32x4Le = 0x45,
  F32x4Ge = 0x46,
  F64x2Eq = 0x47,
  F64x2Ne = 0x48,
  F64x2Lt = 0x49,
  F64x2Gt = 0x4a,
  F64x2Le = 0x4b,
  F64x2Ge = 0x4c,
  V128Not = 0x4d,
  V128And = 0x4e,
  V128AndNot = 0x4f,
  V128Or = 0x50,
  V128Xor = 0x51,
  V128Bitselect = 0x52,
  V128AnyTrue = 0x53,
  V128Load8Lane = 0x54,
  V128Load16Lane = 0x55,
  V128Load32Lane = 0x56,
  V128Load64Lane = 0x57,
  V128Store8Lane = 0x58,
  V128Store16Lane = 0x59,
  V128Store32Lane = 0x5a,
  V128Store64Lane = 0x5b,
  V128Load32Zero = 0x5c,
  V128Load64Zero = 0x5d,
  F32x4DemoteF64x2Zero = 0x5e,
  F64x2PromoteLowF32x4 = 0x5f,
  I8x16Abs = 0x60,
  I8x16Neg = 0x61,
  I8x16Popcnt = 0x62,
  I8x16AllTrue = 0x63,
  I8x16Bitmask = 0x64,
  I8x16NarrowI16x8S = 0x65,
  I8x16NarrowI16x8U = 0x66,
  F32x4Ceil = 0x67,
  F32x4Floor = 0x68,
  F32x4Trunc = 0x69,
  F32x4Nearest = 0x6a,
  I8x16Shl = 0x6b,
  I8x16ShrS = 0x6c,
  I8x16ShrU = 0x6d,
  I8x16Add = 0x6e,
  I8x16AddSatS = 0x6f,
  I8x16AddSatU = 0x70,
  I8x16Sub = 0x71,
  I8x16SubSatS = 0x72,
  I8x16SubSatU = 0x73,
  F64x2Ceil = 0x74,
  F64x2Floor = 0x75,
  I8x16MinS = 0x76,
  I8x16MinU = 0x77,
  I8x16MaxS = 0x78,
  I8x16MaxU = 0x79,
  F64x2Trunc = 0x7a,
  I8x16AvgrU = 0x7b,
  I16x8ExtaddPairwiseI8x16S = 0x7c,
  I16x8ExtaddPairwiseI8x16U = 0x7d,
  I32x4ExtaddPairwiseI16x8S = 0x7e,
  I32x4ExtaddPairwiseI16x8U = 0x7f,
  I16x8Abs = 0x80,
  I16x8Neg = 0x81,
  I16x8Q15MulrSatS = 0x82,
  I16x8AllTrue = 0x83,
  I16x8Bitmask = 0x84,
  I16x8NarrowI32x4S = 0x85,
  I16x8NarrowI32x4U = 0x86,
  I16x8ExtendLowI8x16S = 0x87,
  I16x8ExtendHighI8x16S = 0x88,
  I16x8ExtendLowI8x16U = 0x89,
  I16x8ExtendHighI8x16U = 0x8a,
  I16x8Shl = 0x8b,
  I16x8ShrS = 0x8c,
  I16x8ShrU = 0x8d,
  I16x8Add = 0x8e,
  I16x8AddSatS = 0x8f,
  I16x8AddSatU = 0x90,
  I16x8Sub = 0x91,
  I16x8SubSatS = 0x92,
  I16x8SubSatU = 0x93,
  F64x2Nearest = 0x94,
  I16x8Mul = 0x95,
  I16x8MinS = 0x96,
  I16x8MinU = 0x97,
  I16x8MaxS = 0x98,
  I16x8MaxU = 0x99,
  // Unused = 0x9a
  I16x8AvgrU = 0x9b,
  I16x8ExtmulLowI8x16S = 0x9c,
  I16x8ExtmulHighI8x16S = 0x9d,
  I16x8ExtmulLowI8x16U = 0x9e,
  I16x8ExtmulHighI8x16U = 0x9f,
  I32x4Abs = 0xa0,
  I32x4Neg = 0xa1,
  // Unused = 0xa2
  I32x4AllTrue = 0xa3,
  I32x4Bitmask = 0xa4,
  // Unused = 0xa5
  // Unused = 0xa6
  I32x4ExtendLowI16x8S = 0xa7,
  I32x4ExtendHighI16x8S = 0xa8,
  I32x4ExtendLowI16x8U = 0xa9,
  I32x4ExtendHighI16x8U = 0xaa,
  I32x4Shl = 0xab,
  I32x4ShrS = 0xac,
  I32x4ShrU = 0xad,
  I32x4Add = 0xae,
  // Unused = 0xaf
  // Unused = 0xb0
  I32x4Sub = 0xb1,
  // Unused = 0xb2
  // Unused = 0xb3
  // Unused = 0xb4
  I32x4Mul = 0xb5,
  I32x4MinS = 0xb6,
  I32x4MinU = 0xb7,
  I32x4MaxS = 0xb8,
  I32x4MaxU = 0xb9,
  I32x4DotI16x8S = 0xba,
  // Unused = 0xbb
  I32x4ExtmulLowI16x8S = 0xbc,
  I32x4ExtmulHighI16x8S = 0xbd,
  I32x4ExtmulLowI16x8U = 0xbe,
  I32x4ExtmulHighI16x8U = 0xbf,
  I64x2Abs = 0xc0,
  I64x2Neg = 0xc1,
  // AnyTrue = 0xc2
  I64x2AllTrue = 0xc3,
  I64x2Bitmask = 0xc4,
  // Unused = 0xc5
  // Unused = 0xc6
  I64x2ExtendLowI32x4S = 0xc7,
  I64x2ExtendHighI32x4S = 0xc8,
  I64x2ExtendLowI32x4U = 0xc9,
  I64x2ExtendHighI32x4U = 0xca,
  I64x2Shl = 0xcb,
  I64x2ShrS = 0xcc,
  I64x2ShrU = 0xcd,
  I64x2Add = 0xce,
  // Unused = 0xcf
  // Unused = 0xd0
  I64x2Sub = 0xd1,
  // Unused = 0xd2
  // Unused = 0xd3
  // Unused = 0xd4
  I64x2Mul = 0xd5,
  I64x2Eq = 0xd6,
  I64x2Ne = 0xd7,
  I64x2LtS = 0xd8,
  I64x2GtS = 0xd9,
  I64x2LeS = 0xda,
  I64x2GeS = 0xdb,
  I64x2ExtmulLowI32x4S = 0xdc,
  I64x2ExtmulHighI32x4S = 0xdd,
  I64x2ExtmulLowI32x4U = 0xde,
  I64x2ExtmulHighI32x4U = 0xdf,
  F32x4Abs = 0xe0,
  F32x4Neg = 0xe1,
  // Unused = 0xe2
  F32x4Sqrt = 0xe3,
  F32x4Add = 0xe4,
  F32x4Sub = 0xe5,
  F32x4Mul = 0xe6,
  F32x4Div = 0xe7,
  F32x4Min = 0xe8,
  F32x4Max = 0xe9,
  F32x4PMin = 0xea,
  F32x4PMax = 0xeb,
  F64x2Abs = 0xec,
  F64x2Neg = 0xed,
  // Unused = 0xee
  F64x2Sqrt = 0xef,
  F64x2Add = 0xf0,
  F64x2Sub = 0xf1,
  F64x2Mul = 0xf2,
  F64x2Div = 0xf3,
  F64x2Min = 0xf4,
  F64x2Max = 0xf5,
  F64x2PMin = 0xf6,
  F64x2PMax = 0xf7,
  I32x4TruncSatF32x4S = 0xf8,
  I32x4TruncSatF32x4U = 0xf9,
  F32x4ConvertI32x4S = 0xfa,
  F32x4ConvertI32x4U = 0xfb,
  I32x4TruncSatF64x2SZero = 0xfc,
  I32x4TruncSatF64x2UZero = 0xfd,
  F64x2ConvertLowI32x4S = 0xfe,
  F64x2ConvertLowI32x4U = 0xff,
  I8x16RelaxedSwizzle = 0x100,
  I32x4RelaxedTruncF32x4S = 0x101,
  I32x4RelaxedTruncF32x4U = 0x102,
  I32x4RelaxedTruncF64x2SZero = 0x103,
  I32x4RelaxedTruncF64x2UZero = 0x104,
  F32x4RelaxedMadd = 0x105,
  F32x4RelaxedNmadd = 0x106,
  F64x2RelaxedMadd = 0x107,
  F64x2RelaxedNmadd = 0x108,
  I8x16RelaxedLaneSelect = 0x109,
  I16x8RelaxedLaneSelect = 0x10a,
  I32x4RelaxedLaneSelect = 0x10b,
  I64x2RelaxedLaneSelect = 0x10c,
  F32x4RelaxedMin = 0x10d,
  F32x4RelaxedMax = 0x10e,
  F64x2RelaxedMin = 0x10f,
  F64x2RelaxedMax = 0x110,
  I16x8RelaxedQ15MulrS = 0x111,
  I16x8DotI8x16I7x16S = 0x112,
  I32x4DotI8x16I7x16AddS = 0x113,

  // Reserved for Relaxed SIMD = 0x114-0x12f

  // Unused = 0x130 and up

  // Mozilla extensions
  MozPMADDUBSW = 0x201,

  Limit
};

// Opcodes in the "miscellaneous" opcode space.
enum class MiscOp {
  // Saturating float-to-int conversions
  I32TruncSatF32S = 0x00,
  I32TruncSatF32U = 0x01,
  I32TruncSatF64S = 0x02,
  I32TruncSatF64U = 0x03,
  I64TruncSatF32S = 0x04,
  I64TruncSatF32U = 0x05,
  I64TruncSatF64S = 0x06,
  I64TruncSatF64U = 0x07,

  // Bulk memory operations, per proposal as of February 2019.
  MemoryInit = 0x08,
  DataDrop = 0x09,
  MemoryCopy = 0x0a,
  MemoryFill = 0x0b,
  TableInit = 0x0c,
  ElemDrop = 0x0d,
  TableCopy = 0x0e,

  // Reftypes, per proposal as of February 2019.
  TableGrow = 0x0f,
  TableSize = 0x10,
  TableFill = 0x11,

  MemoryDiscard = 0x12,

  Limit
};

// Opcodes from threads proposal as of June 30, 2017
enum class ThreadOp {
  // Wait and wake
  Wake = 0x00,
  I32Wait = 0x01,
  I64Wait = 0x02,
  Fence = 0x03,

  // Load and store
  I32AtomicLoad = 0x10,
  I64AtomicLoad = 0x11,
  I32AtomicLoad8U = 0x12,
  I32AtomicLoad16U = 0x13,
  I64AtomicLoad8U = 0x14,
  I64AtomicLoad16U = 0x15,
  I64AtomicLoad32U = 0x16,
  I32AtomicStore = 0x17,
  I64AtomicStore = 0x18,
  I32AtomicStore8U = 0x19,
  I32AtomicStore16U = 0x1a,
  I64AtomicStore8U = 0x1b,
  I64AtomicStore16U = 0x1c,
  I64AtomicStore32U = 0x1d,

  // Read-modify-write operations
  I32AtomicAdd = 0x1e,
  I64AtomicAdd = 0x1f,
  I32AtomicAdd8U = 0x20,
  I32AtomicAdd16U = 0x21,
  I64AtomicAdd8U = 0x22,
  I64AtomicAdd16U = 0x23,
  I64AtomicAdd32U = 0x24,

  I32AtomicSub = 0x25,
  I64AtomicSub = 0x26,
  I32AtomicSub8U = 0x27,
  I32AtomicSub16U = 0x28,
  I64AtomicSub8U = 0x29,
  I64AtomicSub16U = 0x2a,
  I64AtomicSub32U = 0x2b,

  I32AtomicAnd = 0x2c,
  I64AtomicAnd = 0x2d,
  I32AtomicAnd8U = 0x2e,
  I32AtomicAnd16U = 0x2f,
  I64AtomicAnd8U = 0x30,
  I64AtomicAnd16U = 0x31,
  I64AtomicAnd32U = 0x32,

  I32AtomicOr = 0x33,
  I64AtomicOr = 0x34,
  I32AtomicOr8U = 0x35,
  I32AtomicOr16U = 0x36,
  I64AtomicOr8U = 0x37,
  I64AtomicOr16U = 0x38,
  I64AtomicOr32U = 0x39,

  I32AtomicXor = 0x3a,
  I64AtomicXor = 0x3b,
  I32AtomicXor8U = 0x3c,
  I32AtomicXor16U = 0x3d,
  I64AtomicXor8U = 0x3e,
  I64AtomicXor16U = 0x3f,
  I64AtomicXor32U = 0x40,

  I32AtomicXchg = 0x41,
  I64AtomicXchg = 0x42,
  I32AtomicXchg8U = 0x43,
  I32AtomicXchg16U = 0x44,
  I64AtomicXchg8U = 0x45,
  I64AtomicXchg16U = 0x46,
  I64AtomicXchg32U = 0x47,

  // CompareExchange
  I32AtomicCmpXchg = 0x48,
  I64AtomicCmpXchg = 0x49,
  I32AtomicCmpXchg8U = 0x4a,
  I32AtomicCmpXchg16U = 0x4b,
  I64AtomicCmpXchg8U = 0x4c,
  I64AtomicCmpXchg16U = 0x4d,
  I64AtomicCmpXchg32U = 0x4e,

  Limit
};

enum class BuiltinModuleFuncId {
// ------------------------------------------------------------------------
// These are part/suffix of the MozOp::CallBuiltinModuleFunc operators that are
// emitted internally when compiling intrinsic modules and are rejected by wasm
// validation.
// See wasm/WasmBuiltinModule.yaml for the list.
#define VISIT_BUILTIN_FUNC(op, export, sa_name, abitype, entry, has_memory, \
                           idx)                                             \
  op = idx,  // NOLINT
  FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC

  // Op limit.
  Limit
};

enum class BuiltinModuleId {
  SelfTest = 0,
  IntGemm,
  JSString,
};

struct BuiltinModuleIds {
  BuiltinModuleIds() = default;

  bool selfTest = false;
  bool intGemm = false;
  bool jsString = false;

  bool hasNone() const { return !selfTest && !intGemm && !jsString; }

  WASM_CHECK_CACHEABLE_POD(selfTest, intGemm, jsString)
};

WASM_DECLARE_CACHEABLE_POD(BuiltinModuleIds)

enum class StackSwitchKind {
  SwitchToSuspendable,
  SwitchToMain,
  ContinueOnSuspendable,
};

enum class UpdateSuspenderStateAction {
  Enter,
  Suspend,
  Resume,
  Leave,
};

enum class MozOp {
  // ------------------------------------------------------------------------
  // These operators are emitted internally when compiling asm.js and are
  // rejected by wasm validation.  They are prefixed by MozPrefix.

  // asm.js-specific operators.  They start at 1 so as to check for
  // uninitialized (zeroed) storage.
  TeeGlobal = 0x01,
  I32Min,
  I32Max,
  I32Neg,
  I32BitNot,
  I32Abs,
  F32TeeStoreF64,
  F64TeeStoreF32,
  I32TeeStore8,
  I32TeeStore16,
  I64TeeStore8,
  I64TeeStore16,
  I64TeeStore32,
  I32TeeStore,
  I64TeeStore,
  F32TeeStore,
  F64TeeStore,
  F64Mod,
  F64SinNative,
  F64SinFdlibm,
  F64CosNative,
  F64CosFdlibm,
  F64TanNative,
  F64TanFdlibm,
  F64Asin,
  F64Acos,
  F64Atan,
  F64Exp,
  F64Log,
  F64Pow,
  F64Atan2,

  // asm.js-style call_indirect with the callee evaluated first.
  OldCallDirect,
  OldCallIndirect,

  // Call a builtin module funcs. The operator has argument leb u32 to specify
  // particular operation id. See BuiltinModuleFuncId above.
  CallBuiltinModuleFunc,

  StackSwitch,

  Limit
};

struct OpBytes {
  // b0 is a byte value but has a 16-bit representation to allow for a full
  // 256-value range plus a sentinel Limit value.
  uint16_t b0;
  // b1 is a LEB128 value but 32 bits is enough for now.
  uint32_t b1;

  explicit OpBytes(Op x) {
    b0 = uint16_t(x);
    b1 = 0;
  }
  OpBytes() = default;

  // Whether this opcode should have a breakpoint site inserted directly before
  // the opcode in baseline when debugging. We use this as a heuristic to
  // reduce the number of breakpoint sites.
  bool shouldHaveBreakpoint() const {
    switch (Op(b0)) {
      // Block-like instructions don't get their own breakpoint site, a
      // breakpoint can be used on instructions in the block.
      case Op::Block:
      case Op::Loop:
      case Op::If:
      case Op::Else:
      case Op::Try:
      case Op::Delegate:
      case Op::Catch:
      case Op::CatchAll:
      case Op::End:
      // Effect-less instructions without inputs are leaf nodes in expressions,
      // a breakpoint can be used on instructions that consume these values.
      case Op::LocalGet:
      case Op::GlobalGet:
      case Op::I32Const:
      case Op::I64Const:
      case Op::F32Const:
      case Op::F64Const:
      case Op::RefNull:
      case Op::Drop:
        return false;
      default:
        return true;
    }
  }
};

static const char NameSectionName[] = "name";
static const char SourceMappingURLSectionName[] = "sourceMappingURL";
static const char BranchHintingSectionName[] = "metadata.code.branch_hint";

enum class NameType { Module = 0, Function = 1, Local = 2 };

enum class FieldFlags { Mutable = 0x01, AllowedMask = 0x01 };

enum class FieldWideningOp { None, Signed, Unsigned };

// The WebAssembly spec hard-codes the virtual page size to be 64KiB and
// requires the size of linear memory to always be a multiple of 64KiB.

static const unsigned PageSize = 64 * 1024;
static const unsigned PageBits = 16;
static_assert(PageSize == (1u << PageBits));

static const unsigned PageMask = ((1u << PageBits) - 1);

// These limits are agreed upon with other engines for consistency.

static const unsigned MaxRecGroups = 1000000;
static const unsigned MaxTypes = 1000000;
static const unsigned MaxSubTypingDepth = 63;
static const unsigned MaxTags = 1000000;
static const unsigned MaxFuncs = 1000000;
static const unsigned MaxTables = 100000;
static const unsigned MaxMemories = 100000;
static const unsigned MaxImports = 100000;
static const unsigned MaxExports = 100000;
static const unsigned MaxGlobals = 1000000;
static const unsigned MaxDataSegments = 100000;
static const unsigned MaxDataSegmentLengthPages = 16384;
static const unsigned MaxElemSegments = 10000000;
static const unsigned MaxElemSegmentLength = 10000000;
static const unsigned MaxTableLimitField = UINT32_MAX;
static const unsigned MaxTableLength = 10000000;
static const unsigned MaxLocals = 50000;
static const unsigned MaxParams = 1000;
static const unsigned MaxResults = 1000;
static const unsigned MaxStructFields = 10000;
static const uint64_t MaxMemory32LimitField = uint64_t(1) << 16;
static const uint64_t MaxMemory64LimitField = uint64_t(1) << 48;
static const unsigned MaxStringBytes = 100000;
static const unsigned MaxModuleBytes = 1024 * 1024 * 1024;
static const unsigned MaxFunctionBytes = 7654321;
static const unsigned MaxArrayNewFixedElements = 10000;

// Maximum payload size, in bytes, of a gc-proposal Array.  Puts it fairly
// close to 2^31 without exposing us to potential danger at the signed-i32
// wraparound boundary.  Note that gc-proposal Struct sizes are limited by
// MaxStructFields above.  Some code assumes that the payload size will fit in
// a uint32_t, hence the static assert.
static const unsigned MaxArrayPayloadBytes = 1987654321;
static_assert(uint64_t(MaxArrayPayloadBytes) <
              (uint64_t(1) << (8 * sizeof(uint32_t))));

// These limits pertain to our WebAssembly implementation only.

static const unsigned MaxTryTableCatches = 10000;
static const unsigned MaxBrTableElems = 1000000;
static const unsigned MaxCodeSectionBytes = MaxModuleBytes;
static const unsigned MaxBranchHintValue = 2;

// 512KiB should be enough, considering how Rabaldr uses the stack and
// what the standard limits are:
//
// - 1,000 parameters
// - 50,000 locals
// - 10,000 values on the eval stack (not an official limit)
//
// At sizeof(int64) bytes per slot this works out to about 480KiB.

static const unsigned MaxFrameSize = 512 * 1024;

// Limit for the amount of stacks present in the runtime.
static const size_t SuspendableStacksMaxCount = 100;

// Max size of an allocated stack.
static const size_t SuspendableStackSize = 0x100000;

// Size of additional space at the top of a suspendable stack.
// The space is allocated to C++ handlers such as error/trap handlers,
// or stack snapshots utilities.
static const size_t SuspendableRedZoneSize = 0x6000;

// Total size of a suspendable stack to be reserved.
static constexpr size_t SuspendableStackPlusRedZoneSize =
    SuspendableStackSize + SuspendableRedZoneSize;

// Asserted by Decoder::readVarU32.

static const unsigned MaxVarU32DecodedBytes = 5;

// The CompileMode controls how compilation of a module is performed (notably,
// how many times we compile it).

enum class CompileMode { Once, Tier1, Tier2 };

// Typed enum for whether debugging is enabled.

enum class DebugEnabled { False, True };

}  // namespace wasm
}  // namespace js

#endif  // wasm_constants_h
