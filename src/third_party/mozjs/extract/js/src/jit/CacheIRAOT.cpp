/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ENABLE_JS_AOT_ICS

#  include "jit/CacheIRAOT.h"

#  include "jsmath.h"
#  include "jstypes.h"

#  include "gc/AllocKind.h"
#  include "jit/CacheIR.h"
#  include "jit/CacheIRAOTGenerated.h"
#  include "jit/JitZone.h"
#  include "js/ScalarType.h"
#  include "js/Value.h"
#  include "vm/CompletionKind.h"
#  include "vm/Opcodes.h"
#  include "wasm/WasmValType.h"

// Include null pointers for "native" pointers: an AOT-loaded stub
// should not bake in an arbitrary pointer observed from a previous
// execution. In any case, the AOT corpus should not include any ICs
// that bake in pointers (baseline does not generate any).

#  if JS_BITS_PER_WORD == 32
#    define NATIVE_NULLPTR 0, 0, 0, 0
#  elif JS_BITS_PER_WORD == 64
#    define NATIVE_NULLPTR 0, 0, 0, 0, 0, 0, 0, 0
#  else
#    error Please add a case for a non-32/64-bit system here.
#  endif

// These correspond to the CacheIRWriter definitions of the serialized
// CacheIR format.

#  define OP(op) \
    uint8_t(uint16_t(CacheOp::op) & 0xff), uint8_t(uint16_t(CacheOp::op) >> 8),
#  define ID(id) id,
#  define OFFSET(off) off,
#  define BOOL(x) x,
#  define BYTE(x) x,
#  define JSOP(op) uint8_t(JSOp::op),
#  define TYPEOFEQOPERAND(value) value,
#  define STATICSTRING(p) NATIVE_NULLPTR,
#  define INT32(i)                                                             \
    uint32_t(i) & 0xff, (uint32_t(i) >> 8) & 0xff, (uint32_t(i) >> 16) & 0xff, \
        (uint32_t(i) >> 24) & 0xff,
#  define UINT32(i) \
    (i) & 0xff, ((i) >> 8) & 0xff, ((i) >> 16) & 0xff, ((i) >> 24) & 0xff,
#  define CALLFLAGS(f) f,
#  define WHYMAGIC(m) m,
#  define SCALARTYPE(name) uint8_t(Scalar::Type::name),
#  define UNARYMATHFUNC(name) uint8_t(UnaryMathFunction::name),
#  define VALUETYPE(name) uint8_t(name),
#  define NATIVEIMM(p) NATIVE_NULLPTR,
#  define GUARDCLASSKIND(name) uint8_t(GuardClassKind::name),
#  define ARRAYBUFFERVIEWKIND(name) uint8_t(ArrayBufferViewKind::name),
#  define WASMVALTYPE(name) uint8_t(wasm::ValType::Kind::name),
#  define ALLOCKIND(name) uint8_t(gc::AllocKind::name),
#  define COMPLETIONKIND(name) uint8_t(CompletionKind::name),
#  define REALMFUSE(i) i,

// Other macros used to serialize parts of the CacheIRWriter.
#  define STUBFIELD(ty) AOTStubFieldData{StubField::Type::ty, 0},
#  define LASTUSED(n) n,

// First, generate individual IC bodies.

#  define IC_BODY(idx, _kind, _num_input_operands, _num_operand_ids,        \
                  _num_instructions, _typedata, _stubdatasize, _stubfields, \
                  _lastused, ops)                                           \
    static const uint8_t IC##idx[] = {ops};

JS_AOT_IC_DATA(IC_BODY)

// Generate the stubfield lists.

#  define IC_STUBFIELD(idx, _kind, _num_input_operands, _num_operand_ids, \
                       _num_instructions, _typedata, _stubdatasize,       \
                       stubfields, _lastused, _ops)                       \
    static const AOTStubFieldData IC##idx##StubFields[] = {stubfields};

JS_AOT_IC_DATA(IC_STUBFIELD)

// Generate the operand-last-used lists.

#  define IC_LASTUSED(idx, _kind, _num_input_operands, _num_operand_ids, \
                      _num_instructions, _typedata, _stubdatasize,       \
                      _stubfields, lastused, _ops)                       \
    static const uint32_t IC##idx##LastUsed[] = {lastused};

JS_AOT_IC_DATA(IC_LASTUSED)

// Now, generate the toplevel list of AOT structs from which we can
// reconstitute a CacheIRWriter.

#  define IC_TOP(idx, kind, num_input_operands, num_operand_ids,        \
                 num_instructions, typedata, stubdatasize, _stubfields, \
                 _lastused, _ops)                                       \
    CacheIRAOTStub{                                                     \
        CacheKind::kind,                                                \
        num_operand_ids,                                                \
        num_input_operands,                                             \
        num_instructions,                                               \
        TypeData(typedata),                                             \
        stubdatasize,                                                   \
        IC##idx##StubFields,                                            \
        sizeof(IC##idx##StubFields) / sizeof(IC##idx##StubFields[0]),   \
        IC##idx##LastUsed,                                              \
        IC##idx,                                                        \
        sizeof(IC##idx)},

static const CacheIRAOTStub stubs[] = {JS_AOT_IC_DATA(IC_TOP)};

mozilla::Span<const CacheIRAOTStub> js::jit::GetAOTStubs() {
  return mozilla::Span(stubs, sizeof(stubs) / sizeof(stubs[0]));
}

CacheIRWriter::CacheIRWriter(JSContext* cx, const CacheIRAOTStub& stub)
    : CustomAutoRooter(cx),
#  ifdef DEBUG
      cx_(cx),
#  endif
      tooLarge_(false),
      lastOffset_(0),
      lastIndex_(0) {
  nextOperandId_ = stub.numOperandIds;
  nextInstructionId_ = stub.numInstructions;
  numInputOperands_ = stub.numInputOperands;
  typeData_ = stub.typeData;
  stubDataSize_ = stub.stubDataSize;
  for (size_t i = 0; i < stub.stubfieldCount; i++) {
    buffer_.propagateOOM(stubFields_.append(
        StubField(stub.stubfields[i].data, stub.stubfields[i].type)));
  }
  for (uint32_t i = 0; i < stub.numOperandIds; i++) {
    buffer_.propagateOOM(operandLastUsed_.append(stub.operandLastUsed[i]));
  }
  buffer_.writeBytes(stub.data, stub.dataLength);
}

#endif /* ENABLE_JS_AOT_ICS */
