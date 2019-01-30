/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmValidate.h"

#include "mozilla/CheckedInt.h"

#include "jit/JitOptions.h"
#include "js/Printf.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "wasm/WasmBinaryIterator.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt;

// Decoder implementation.

bool
Decoder::failf(const char* msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    UniqueChars str(JS_vsmprintf(msg, ap));
    va_end(ap);
    if (!str)
        return false;

    return fail(str.get());
}

bool
Decoder::fail(size_t errorOffset, const char* msg)
{
    MOZ_ASSERT(error_);
    UniqueChars strWithOffset(JS_smprintf("at offset %zu: %s", errorOffset, msg));
    if (!strWithOffset)
        return false;

    *error_ = Move(strWithOffset);
    return false;
}

bool
Decoder::readSectionHeader(uint8_t* id, SectionRange* range)
{
    if (!readFixedU8(id))
        return false;

    uint32_t size;
    if (!readVarU32(&size))
        return false;

    range->start = currentOffset();
    range->size = size;
    return true;
}

bool
Decoder::startSection(SectionId id, ModuleEnvironment* env, MaybeSectionRange* range,
                      const char* sectionName)
{
    MOZ_ASSERT(!*range);

    // Record state at beginning of section to allow rewinding to this point
    // if, after skipping through several custom sections, we don't find the
    // section 'id'.
    const uint8_t* const initialCur = cur_;
    const size_t initialCustomSectionsLength = env->customSections.length();

    // Maintain a pointer to the current section that gets updated as custom
    // sections are skipped.
    const uint8_t* currentSectionStart = cur_;

    // Only start a section with 'id', skipping any custom sections before it.

    uint8_t idValue;
    if (!readFixedU8(&idValue))
        goto rewind;

    while (idValue != uint8_t(id)) {
        if (idValue != uint8_t(SectionId::Custom))
            goto rewind;

        // Rewind to the beginning of the current section since this is what
        // skipCustomSection() assumes.
        cur_ = currentSectionStart;
        if (!skipCustomSection(env))
            return false;

        // Having successfully skipped a custom section, consider the next
        // section.
        currentSectionStart = cur_;
        if (!readFixedU8(&idValue))
            goto rewind;
    }

    // Don't check the size since the range of bytes being decoded might not
    // contain the section body. (This is currently the case when streaming: the
    // code section header is decoded with the module environment bytes, the
    // body of the code section is streamed in separately.)

    uint32_t size;
    if (!readVarU32(&size))
        goto fail;

    range->emplace();
    (*range)->start = currentOffset();
    (*range)->size = size;
    return true;

  rewind:
    cur_ = initialCur;
    env->customSections.shrinkTo(initialCustomSectionsLength);
    return true;

  fail:
    return failf("failed to start %s section", sectionName);
}

bool
Decoder::finishSection(const SectionRange& range, const char* sectionName)
{
    if (resilientMode_)
        return true;
    if (range.size != currentOffset() - range.start)
        return failf("byte size mismatch in %s section", sectionName);
    return true;
}

bool
Decoder::startCustomSection(const char* expected, size_t expectedLength, ModuleEnvironment* env,
                            MaybeSectionRange* range)
{
    // Record state at beginning of section to allow rewinding to this point
    // if, after skipping through several custom sections, we don't find the
    // section 'id'.
    const uint8_t* const initialCur = cur_;
    const size_t initialCustomSectionsLength = env->customSections.length();

    while (true) {
        // Try to start a custom section. If we can't, rewind to the beginning
        // since we may have skipped several custom sections already looking for
        // 'expected'.
        if (!startSection(SectionId::Custom, env, range, "custom"))
            return false;
        if (!*range)
            goto rewind;

        if (bytesRemain() < (*range)->size)
            goto fail;

        NameInBytecode name;
        if (!readVarU32(&name.length) || name.length > bytesRemain())
            goto fail;

        name.offset = currentOffset();
        uint32_t payloadOffset = name.offset + name.length;
        uint32_t payloadEnd = (*range)->start + (*range)->size;
        if (payloadOffset > payloadEnd)
            goto fail;

        // Now that we have a valid custom section, record its offsets in the
        // metadata which can be queried by the user via Module.customSections.
        // Note: after an entry is appended, it may be popped if this loop or
        // the loop in startSection needs to rewind.
        if (!env->customSections.emplaceBack(name, payloadOffset, payloadEnd - payloadOffset))
            return false;

        // If this is the expected custom section, we're done.
        if (!expected || (expectedLength == name.length && !memcmp(cur_, expected, name.length))) {
            cur_ += name.length;
            return true;
        }

        // Otherwise, blindly skip the custom section and keep looking.
        finishCustomSection(**range);
        range->reset();
    }
    MOZ_CRASH("unreachable");

  rewind:
    cur_ = initialCur;
    env->customSections.shrinkTo(initialCustomSectionsLength);
    return true;

  fail:
    return fail("failed to start custom section");
}

void
Decoder::finishCustomSection(const SectionRange& range)
{
    MOZ_ASSERT(cur_ >= beg_);
    MOZ_ASSERT(cur_ <= end_);
    cur_ = (beg_ + (range.start - offsetInModule_)) + range.size;
    MOZ_ASSERT(cur_ <= end_);
    clearError();
}

bool
Decoder::skipCustomSection(ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!startCustomSection(nullptr, 0, env, &range))
        return false;
    if (!range)
        return fail("expected custom section");

    finishCustomSection(*range);
    return true;
}

bool
Decoder::startNameSubsection(NameType nameType, Maybe<uint32_t>* endOffset)
{
    MOZ_ASSERT(!*endOffset);

    const uint8_t* initialPosition = cur_;

    uint8_t nameTypeValue;
    if (!readFixedU8(&nameTypeValue))
        return false;

    if (nameTypeValue != uint8_t(nameType)) {
        cur_ = initialPosition;
        return true;
    }

    uint32_t payloadLength;
    if (!readVarU32(&payloadLength) || payloadLength > bytesRemain())
        return false;

    *endOffset = Some(currentOffset() + payloadLength);
    return true;
}

bool
Decoder::finishNameSubsection(uint32_t endOffset)
{
    return endOffset == uint32_t(currentOffset());
}

// Misc helpers.

bool
wasm::EncodeLocalEntries(Encoder& e, const ValTypeVector& locals)
{
    if (locals.length() > MaxLocals)
        return false;

    uint32_t numLocalEntries = 0;
    ValType prev = ValType(TypeCode::Limit);
    for (ValType t : locals) {
        if (t != prev) {
            numLocalEntries++;
            prev = t;
        }
    }

    if (!e.writeVarU32(numLocalEntries))
        return false;

    if (numLocalEntries) {
        prev = locals[0];
        uint32_t count = 1;
        for (uint32_t i = 1; i < locals.length(); i++, count++) {
            if (prev != locals[i]) {
                if (!e.writeVarU32(count))
                    return false;
                if (!e.writeValType(prev))
                    return false;
                prev = locals[i];
                count = 0;
            }
        }
        if (!e.writeVarU32(count))
            return false;
        if (!e.writeValType(prev))
            return false;
    }

    return true;
}

static bool
DecodeValType(Decoder& d, ModuleKind kind, ValType* type)
{
    uint8_t unchecked;
    if (!d.readValType(&unchecked))
        return false;

    switch (unchecked) {
      case uint8_t(ValType::I32):
      case uint8_t(ValType::F32):
      case uint8_t(ValType::F64):
      case uint8_t(ValType::I64):
        *type = ValType(unchecked);
        return true;
      case uint8_t(ValType::I8x16):
      case uint8_t(ValType::I16x8):
      case uint8_t(ValType::I32x4):
      case uint8_t(ValType::F32x4):
      case uint8_t(ValType::B8x16):
      case uint8_t(ValType::B16x8):
      case uint8_t(ValType::B32x4):
        if (kind != ModuleKind::AsmJS)
            return d.fail("bad type");
        *type = ValType(unchecked);
        return true;
      default:
        break;
    }
    return d.fail("bad type");
}

bool
wasm::DecodeLocalEntries(Decoder& d, ModuleKind kind, ValTypeVector* locals)
{
    uint32_t numLocalEntries;
    if (!d.readVarU32(&numLocalEntries))
        return d.fail("failed to read number of local entries");

    for (uint32_t i = 0; i < numLocalEntries; i++) {
        uint32_t count;
        if (!d.readVarU32(&count))
            return d.fail("failed to read local entry count");

        if (MaxLocals - locals->length() < count)
            return d.fail("too many locals");

        ValType type;
        if (!DecodeValType(d, kind, &type))
            return false;

        if (!locals->appendN(type, count))
            return false;
    }

    return true;
}

// Function body validation.

struct ValidatingPolicy
{
    typedef Nothing Value;
    typedef Nothing ControlItem;
};

typedef OpIter<ValidatingPolicy> ValidatingOpIter;

static bool
DecodeFunctionBodyExprs(const ModuleEnvironment& env, const Sig& sig, const ValTypeVector& locals,
                        const uint8_t* bodyEnd, Decoder* d)
{
    ValidatingOpIter iter(env, *d);

    if (!iter.readFunctionStart(sig.ret()))
        return false;

#define CHECK(c) if (!(c)) return false; break

    while (true) {
        OpBytes op;
        if (!iter.readOp(&op))
            return false;

        Nothing nothing;

        switch (op.b0) {
          case uint16_t(Op::End): {
            LabelKind unusedKind;
            ExprType unusedType;
            if (!iter.readEnd(&unusedKind, &unusedType, &nothing))
                return false;
            iter.popEnd();
            if (iter.controlStackEmpty())
                return iter.readFunctionEnd(bodyEnd);
            break;
          }
          case uint16_t(Op::Nop):
            CHECK(iter.readNop());
          case uint16_t(Op::Drop):
            CHECK(iter.readDrop());
          case uint16_t(Op::Call): {
            uint32_t unusedIndex;
            ValidatingOpIter::ValueVector unusedArgs;
            CHECK(iter.readCall(&unusedIndex, &unusedArgs));
          }
          case uint16_t(Op::CallIndirect): {
            uint32_t unusedIndex;
            ValidatingOpIter::ValueVector unusedArgs;
            CHECK(iter.readCallIndirect(&unusedIndex, &nothing, &unusedArgs));
          }
          case uint16_t(Op::I32Const): {
            int32_t unused;
            CHECK(iter.readI32Const(&unused));
          }
          case uint16_t(Op::I64Const): {
            int64_t unused;
            CHECK(iter.readI64Const(&unused));
          }
          case uint16_t(Op::F32Const): {
            float unused;
            CHECK(iter.readF32Const(&unused));
          }
          case uint16_t(Op::F64Const): {
            double unused;
            CHECK(iter.readF64Const(&unused));
          }
          case uint16_t(Op::GetLocal): {
            uint32_t unused;
            CHECK(iter.readGetLocal(locals, &unused));
          }
          case uint16_t(Op::SetLocal): {
            uint32_t unused;
            CHECK(iter.readSetLocal(locals, &unused, &nothing));
          }
          case uint16_t(Op::TeeLocal): {
            uint32_t unused;
            CHECK(iter.readTeeLocal(locals, &unused, &nothing));
          }
          case uint16_t(Op::GetGlobal): {
            uint32_t unused;
            CHECK(iter.readGetGlobal(&unused));
          }
          case uint16_t(Op::SetGlobal): {
            uint32_t unused;
            CHECK(iter.readSetGlobal(&unused, &nothing));
          }
          case uint16_t(Op::Select): {
            StackType unused;
            CHECK(iter.readSelect(&unused, &nothing, &nothing, &nothing));
          }
          case uint16_t(Op::Block):
            CHECK(iter.readBlock());
          case uint16_t(Op::Loop):
            CHECK(iter.readLoop());
          case uint16_t(Op::If):
            CHECK(iter.readIf(&nothing));
          case uint16_t(Op::Else): {
            ExprType type;
            CHECK(iter.readElse(&type, &nothing));
          }
          case uint16_t(Op::I32Clz):
          case uint16_t(Op::I32Ctz):
          case uint16_t(Op::I32Popcnt):
            CHECK(iter.readUnary(ValType::I32, &nothing));
          case uint16_t(Op::I64Clz):
          case uint16_t(Op::I64Ctz):
          case uint16_t(Op::I64Popcnt):
            CHECK(iter.readUnary(ValType::I64, &nothing));
          case uint16_t(Op::F32Abs):
          case uint16_t(Op::F32Neg):
          case uint16_t(Op::F32Ceil):
          case uint16_t(Op::F32Floor):
          case uint16_t(Op::F32Sqrt):
          case uint16_t(Op::F32Trunc):
          case uint16_t(Op::F32Nearest):
            CHECK(iter.readUnary(ValType::F32, &nothing));
          case uint16_t(Op::F64Abs):
          case uint16_t(Op::F64Neg):
          case uint16_t(Op::F64Ceil):
          case uint16_t(Op::F64Floor):
          case uint16_t(Op::F64Sqrt):
          case uint16_t(Op::F64Trunc):
          case uint16_t(Op::F64Nearest):
            CHECK(iter.readUnary(ValType::F64, &nothing));
          case uint16_t(Op::I32Add):
          case uint16_t(Op::I32Sub):
          case uint16_t(Op::I32Mul):
          case uint16_t(Op::I32DivS):
          case uint16_t(Op::I32DivU):
          case uint16_t(Op::I32RemS):
          case uint16_t(Op::I32RemU):
          case uint16_t(Op::I32And):
          case uint16_t(Op::I32Or):
          case uint16_t(Op::I32Xor):
          case uint16_t(Op::I32Shl):
          case uint16_t(Op::I32ShrS):
          case uint16_t(Op::I32ShrU):
          case uint16_t(Op::I32Rotl):
          case uint16_t(Op::I32Rotr):
            CHECK(iter.readBinary(ValType::I32, &nothing, &nothing));
          case uint16_t(Op::I64Add):
          case uint16_t(Op::I64Sub):
          case uint16_t(Op::I64Mul):
          case uint16_t(Op::I64DivS):
          case uint16_t(Op::I64DivU):
          case uint16_t(Op::I64RemS):
          case uint16_t(Op::I64RemU):
          case uint16_t(Op::I64And):
          case uint16_t(Op::I64Or):
          case uint16_t(Op::I64Xor):
          case uint16_t(Op::I64Shl):
          case uint16_t(Op::I64ShrS):
          case uint16_t(Op::I64ShrU):
          case uint16_t(Op::I64Rotl):
          case uint16_t(Op::I64Rotr):
            CHECK(iter.readBinary(ValType::I64, &nothing, &nothing));
          case uint16_t(Op::F32Add):
          case uint16_t(Op::F32Sub):
          case uint16_t(Op::F32Mul):
          case uint16_t(Op::F32Div):
          case uint16_t(Op::F32Min):
          case uint16_t(Op::F32Max):
          case uint16_t(Op::F32CopySign):
            CHECK(iter.readBinary(ValType::F32, &nothing, &nothing));
          case uint16_t(Op::F64Add):
          case uint16_t(Op::F64Sub):
          case uint16_t(Op::F64Mul):
          case uint16_t(Op::F64Div):
          case uint16_t(Op::F64Min):
          case uint16_t(Op::F64Max):
          case uint16_t(Op::F64CopySign):
            CHECK(iter.readBinary(ValType::F64, &nothing, &nothing));
          case uint16_t(Op::I32Eq):
          case uint16_t(Op::I32Ne):
          case uint16_t(Op::I32LtS):
          case uint16_t(Op::I32LtU):
          case uint16_t(Op::I32LeS):
          case uint16_t(Op::I32LeU):
          case uint16_t(Op::I32GtS):
          case uint16_t(Op::I32GtU):
          case uint16_t(Op::I32GeS):
          case uint16_t(Op::I32GeU):
            CHECK(iter.readComparison(ValType::I32, &nothing, &nothing));
          case uint16_t(Op::I64Eq):
          case uint16_t(Op::I64Ne):
          case uint16_t(Op::I64LtS):
          case uint16_t(Op::I64LtU):
          case uint16_t(Op::I64LeS):
          case uint16_t(Op::I64LeU):
          case uint16_t(Op::I64GtS):
          case uint16_t(Op::I64GtU):
          case uint16_t(Op::I64GeS):
          case uint16_t(Op::I64GeU):
            CHECK(iter.readComparison(ValType::I64, &nothing, &nothing));
          case uint16_t(Op::F32Eq):
          case uint16_t(Op::F32Ne):
          case uint16_t(Op::F32Lt):
          case uint16_t(Op::F32Le):
          case uint16_t(Op::F32Gt):
          case uint16_t(Op::F32Ge):
            CHECK(iter.readComparison(ValType::F32, &nothing, &nothing));
          case uint16_t(Op::F64Eq):
          case uint16_t(Op::F64Ne):
          case uint16_t(Op::F64Lt):
          case uint16_t(Op::F64Le):
          case uint16_t(Op::F64Gt):
          case uint16_t(Op::F64Ge):
            CHECK(iter.readComparison(ValType::F64, &nothing, &nothing));
          case uint16_t(Op::I32Eqz):
            CHECK(iter.readConversion(ValType::I32, ValType::I32, &nothing));
          case uint16_t(Op::I64Eqz):
          case uint16_t(Op::I32WrapI64):
            CHECK(iter.readConversion(ValType::I64, ValType::I32, &nothing));
          case uint16_t(Op::I32TruncSF32):
          case uint16_t(Op::I32TruncUF32):
          case uint16_t(Op::I32ReinterpretF32):
            CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
          case uint16_t(Op::I32TruncSF64):
          case uint16_t(Op::I32TruncUF64):
            CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
          case uint16_t(Op::I64ExtendSI32):
          case uint16_t(Op::I64ExtendUI32):
            CHECK(iter.readConversion(ValType::I32, ValType::I64, &nothing));
          case uint16_t(Op::I64TruncSF32):
          case uint16_t(Op::I64TruncUF32):
            CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
          case uint16_t(Op::I64TruncSF64):
          case uint16_t(Op::I64TruncUF64):
          case uint16_t(Op::I64ReinterpretF64):
            CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
          case uint16_t(Op::F32ConvertSI32):
          case uint16_t(Op::F32ConvertUI32):
          case uint16_t(Op::F32ReinterpretI32):
            CHECK(iter.readConversion(ValType::I32, ValType::F32, &nothing));
          case uint16_t(Op::F32ConvertSI64):
          case uint16_t(Op::F32ConvertUI64):
            CHECK(iter.readConversion(ValType::I64, ValType::F32, &nothing));
          case uint16_t(Op::F32DemoteF64):
            CHECK(iter.readConversion(ValType::F64, ValType::F32, &nothing));
          case uint16_t(Op::F64ConvertSI32):
          case uint16_t(Op::F64ConvertUI32):
            CHECK(iter.readConversion(ValType::I32, ValType::F64, &nothing));
          case uint16_t(Op::F64ConvertSI64):
          case uint16_t(Op::F64ConvertUI64):
          case uint16_t(Op::F64ReinterpretI64):
            CHECK(iter.readConversion(ValType::I64, ValType::F64, &nothing));
          case uint16_t(Op::F64PromoteF32):
            CHECK(iter.readConversion(ValType::F32, ValType::F64, &nothing));
#ifdef ENABLE_WASM_SIGNEXTEND_OPS
          case uint16_t(Op::I32Extend8S):
          case uint16_t(Op::I32Extend16S):
            CHECK(iter.readConversion(ValType::I32, ValType::I32, &nothing));
          case uint16_t(Op::I64Extend8S):
          case uint16_t(Op::I64Extend16S):
          case uint16_t(Op::I64Extend32S):
            CHECK(iter.readConversion(ValType::I64, ValType::I64, &nothing));
#endif
          case uint16_t(Op::I32Load8S):
          case uint16_t(Op::I32Load8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I32, 1, &addr));
          }
          case uint16_t(Op::I32Load16S):
          case uint16_t(Op::I32Load16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I32, 2, &addr));
          }
          case uint16_t(Op::I32Load): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I32, 4, &addr));
          }
          case uint16_t(Op::I64Load8S):
          case uint16_t(Op::I64Load8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I64, 1, &addr));
          }
          case uint16_t(Op::I64Load16S):
          case uint16_t(Op::I64Load16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I64, 2, &addr));
          }
          case uint16_t(Op::I64Load32S):
          case uint16_t(Op::I64Load32U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I64, 4, &addr));
          }
          case uint16_t(Op::I64Load): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::I64, 8, &addr));
          }
          case uint16_t(Op::F32Load): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::F32, 4, &addr));
          }
          case uint16_t(Op::F64Load): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::F64, 8, &addr));
          }
          case uint16_t(Op::I32Store8): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I32, 1, &addr, &nothing));
          }
          case uint16_t(Op::I32Store16): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I32, 2, &addr, &nothing));
          }
          case uint16_t(Op::I32Store): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I32, 4, &addr, &nothing));
          }
          case uint16_t(Op::I64Store8): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I64, 1, &addr, &nothing));
          }
          case uint16_t(Op::I64Store16): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I64, 2, &addr, &nothing));
          }
          case uint16_t(Op::I64Store32): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I64, 4, &addr, &nothing));
          }
          case uint16_t(Op::I64Store): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::I64, 8, &addr, &nothing));
          }
          case uint16_t(Op::F32Store): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::F32, 4, &addr, &nothing));
          }
          case uint16_t(Op::F64Store): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::F64, 8, &addr, &nothing));
          }
          case uint16_t(Op::GrowMemory):
            CHECK(iter.readGrowMemory(&nothing));
          case uint16_t(Op::CurrentMemory):
            CHECK(iter.readCurrentMemory());
          case uint16_t(Op::Br): {
            uint32_t unusedDepth;
            ExprType unusedType;
            CHECK(iter.readBr(&unusedDepth, &unusedType, &nothing));
          }
          case uint16_t(Op::BrIf): {
            uint32_t unusedDepth;
            ExprType unusedType;
            CHECK(iter.readBrIf(&unusedDepth, &unusedType, &nothing, &nothing));
          }
          case uint16_t(Op::BrTable): {
            Uint32Vector unusedDepths;
            uint32_t unusedDefault;
            ExprType unusedType;
            CHECK(iter.readBrTable(&unusedDepths, &unusedDefault, &unusedType, &nothing, &nothing));
          }
          case uint16_t(Op::Return):
            CHECK(iter.readReturn(&nothing));
          case uint16_t(Op::Unreachable):
            CHECK(iter.readUnreachable());
          case uint16_t(Op::NumericPrefix): {
#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
            switch (op.b1) {
              case uint16_t(NumericOp::I32TruncSSatF32):
              case uint16_t(NumericOp::I32TruncUSatF32):
                CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
              case uint16_t(NumericOp::I32TruncSSatF64):
              case uint16_t(NumericOp::I32TruncUSatF64):
                CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
              case uint16_t(NumericOp::I64TruncSSatF32):
              case uint16_t(NumericOp::I64TruncUSatF32):
                CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
              case uint16_t(NumericOp::I64TruncSSatF64):
              case uint16_t(NumericOp::I64TruncUSatF64):
                CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
              default:
                return iter.unrecognizedOpcode(&op);
            }
            break;
#else
            return iter.unrecognizedOpcode(&op);
#endif
          }
          case uint16_t(Op::ThreadPrefix): {
#ifdef ENABLE_WASM_THREAD_OPS
            switch (op.b1) {
              case uint16_t(ThreadOp::Wake): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readWake(&addr, &nothing));
              }
              case uint16_t(ThreadOp::I32Wait): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readWait(&addr, ValType::I32, 4, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I64Wait): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readWait(&addr, ValType::I64, 8, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicLoad): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I32, 4));
              }
              case uint16_t(ThreadOp::I64AtomicLoad): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I64, 8));
              }
              case uint16_t(ThreadOp::I32AtomicLoad8U): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I32, 1));
              }
              case uint16_t(ThreadOp::I32AtomicLoad16U): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I32, 2));
              }
              case uint16_t(ThreadOp::I64AtomicLoad8U): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I64, 1));
              }
              case uint16_t(ThreadOp::I64AtomicLoad16U): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I64, 2));
              }
              case uint16_t(ThreadOp::I64AtomicLoad32U): {
                LinearMemoryAddress<Nothing> addr;
                CHECK(iter.readAtomicLoad(&addr, ValType::I64, 4));
              }
              case uint16_t(ThreadOp::I32AtomicStore): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I32, 4, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicStore): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I64, 8, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicStore8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I32, 1, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicStore16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I32, 2, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicStore8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I64, 1, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicStore16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I64, 2, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicStore32U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicStore(&addr, ValType::I64, 4, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicAdd):
              case uint16_t(ThreadOp::I32AtomicSub):
              case uint16_t(ThreadOp::I32AtomicAnd):
              case uint16_t(ThreadOp::I32AtomicOr):
              case uint16_t(ThreadOp::I32AtomicXor):
              case uint16_t(ThreadOp::I32AtomicXchg): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I32, 4, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicAdd):
              case uint16_t(ThreadOp::I64AtomicSub):
              case uint16_t(ThreadOp::I64AtomicAnd):
              case uint16_t(ThreadOp::I64AtomicOr):
              case uint16_t(ThreadOp::I64AtomicXor):
              case uint16_t(ThreadOp::I64AtomicXchg): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I64, 8, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicAdd8U):
              case uint16_t(ThreadOp::I32AtomicSub8U):
              case uint16_t(ThreadOp::I32AtomicAnd8U):
              case uint16_t(ThreadOp::I32AtomicOr8U):
              case uint16_t(ThreadOp::I32AtomicXor8U):
              case uint16_t(ThreadOp::I32AtomicXchg8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I32, 1, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicAdd16U):
              case uint16_t(ThreadOp::I32AtomicSub16U):
              case uint16_t(ThreadOp::I32AtomicAnd16U):
              case uint16_t(ThreadOp::I32AtomicOr16U):
              case uint16_t(ThreadOp::I32AtomicXor16U):
              case uint16_t(ThreadOp::I32AtomicXchg16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I32, 2, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicAdd8U):
              case uint16_t(ThreadOp::I64AtomicSub8U):
              case uint16_t(ThreadOp::I64AtomicAnd8U):
              case uint16_t(ThreadOp::I64AtomicOr8U):
              case uint16_t(ThreadOp::I64AtomicXor8U):
              case uint16_t(ThreadOp::I64AtomicXchg8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I64, 1, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicAdd16U):
              case uint16_t(ThreadOp::I64AtomicSub16U):
              case uint16_t(ThreadOp::I64AtomicAnd16U):
              case uint16_t(ThreadOp::I64AtomicOr16U):
              case uint16_t(ThreadOp::I64AtomicXor16U):
              case uint16_t(ThreadOp::I64AtomicXchg16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I64, 2, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicAdd32U):
              case uint16_t(ThreadOp::I64AtomicSub32U):
              case uint16_t(ThreadOp::I64AtomicAnd32U):
              case uint16_t(ThreadOp::I64AtomicOr32U):
              case uint16_t(ThreadOp::I64AtomicXor32U):
              case uint16_t(ThreadOp::I64AtomicXchg32U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicRMW(&addr, ValType::I64, 4, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicCmpXchg): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 4, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicCmpXchg): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 8, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicCmpXchg8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 1, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I32AtomicCmpXchg16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 2, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicCmpXchg8U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 1, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicCmpXchg16U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 2, &nothing, &nothing));
              }
              case uint16_t(ThreadOp::I64AtomicCmpXchg32U): {
                  LinearMemoryAddress<Nothing> addr;
                  CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 4, &nothing, &nothing));
              }
              default:
                return iter.unrecognizedOpcode(&op);
            }
            break;
#else
            return iter.unrecognizedOpcode(&op);
#endif  // ENABLE_WASM_THREAD_OPS
          }
          case uint16_t(Op::MozPrefix):
            return iter.unrecognizedOpcode(&op);
          default:
            return iter.unrecognizedOpcode(&op);
        }
    }

    MOZ_CRASH("unreachable");

#undef CHECK
}

bool
wasm::ValidateFunctionBody(const ModuleEnvironment& env, uint32_t funcIndex, uint32_t bodySize,
                           Decoder& d)
{
    const Sig& sig = *env.funcSigs[funcIndex];

    ValTypeVector locals;
    if (!locals.appendAll(sig.args()))
        return false;

    const uint8_t* bodyBegin = d.currentPosition();

    if (!DecodeLocalEntries(d, ModuleKind::Wasm, &locals))
        return false;

    if (!DecodeFunctionBodyExprs(env, sig, locals, bodyBegin + bodySize, &d))
        return false;

    return true;
}

// Section macros.

static bool
DecodePreamble(Decoder& d)
{
    if (d.bytesRemain() > MaxModuleBytes)
        return d.fail("module too big");

    uint32_t u32;
    if (!d.readFixedU32(&u32) || u32 != MagicNumber)
        return d.fail("failed to match magic number");

    if (!d.readFixedU32(&u32) || u32 != EncodingVersion) {
        return d.failf("binary version 0x%" PRIx32 " does not match expected version 0x%" PRIx32,
                       u32, EncodingVersion);
    }

    return true;
}

static bool
DecodeTypeSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Type, env, &range, "type"))
        return false;
    if (!range)
        return true;

    uint32_t numSigs;
    if (!d.readVarU32(&numSigs))
        return d.fail("expected number of signatures");

    if (numSigs > MaxTypes)
        return d.fail("too many signatures");

    if (!env->sigs.resize(numSigs))
        return false;

    for (uint32_t sigIndex = 0; sigIndex < numSigs; sigIndex++) {
        uint8_t form;
        if (!d.readFixedU8(&form) || form != uint8_t(TypeCode::Func))
            return d.fail("expected function form");

        uint32_t numArgs;
        if (!d.readVarU32(&numArgs))
            return d.fail("bad number of function args");

        if (numArgs > MaxParams)
            return d.fail("too many arguments in signature");

        ValTypeVector args;
        if (!args.resize(numArgs))
            return false;

        for (uint32_t i = 0; i < numArgs; i++) {
            if (!DecodeValType(d, ModuleKind::Wasm, &args[i]))
                return false;
        }

        uint32_t numRets;
        if (!d.readVarU32(&numRets))
            return d.fail("bad number of function returns");

        if (numRets > 1)
            return d.fail("too many returns in signature");

        ExprType result = ExprType::Void;

        if (numRets == 1) {
            ValType type;
            if (!DecodeValType(d, ModuleKind::Wasm, &type))
                return false;

            result = ToExprType(type);
        }

        env->sigs[sigIndex] = Sig(Move(args), result);
    }

    return d.finishSection(*range, "type");
}

static UniqueChars
DecodeName(Decoder& d)
{
    uint32_t numBytes;
    if (!d.readVarU32(&numBytes))
        return nullptr;

    if (numBytes > MaxStringBytes)
        return nullptr;

    const uint8_t* bytes;
    if (!d.readBytes(numBytes, &bytes))
        return nullptr;

    if (!JS::StringIsUTF8(bytes, numBytes))
        return nullptr;

    UniqueChars name(js_pod_malloc<char>(numBytes + 1));
    if (!name)
        return nullptr;

    memcpy(name.get(), bytes, numBytes);
    name[numBytes] = '\0';

    return name;
}

static bool
DecodeSignatureIndex(Decoder& d, const SigWithIdVector& sigs, uint32_t* sigIndex)
{
    if (!d.readVarU32(sigIndex))
        return d.fail("expected signature index");

    if (*sigIndex >= sigs.length())
        return d.fail("signature index out of range");

    return true;
}

static bool
DecodeLimits(Decoder& d, Limits* limits, Shareable allowShared = Shareable::False)
{
    uint8_t flags;
    if (!d.readFixedU8(&flags))
        return d.fail("expected flags");

    uint8_t mask = allowShared == Shareable::True
                   ? uint8_t(MemoryMasks::AllowShared)
                   : uint8_t(MemoryMasks::AllowUnshared);

    if (flags & ~uint8_t(mask))
        return d.failf("unexpected bits set in flags: %" PRIu32, (flags & ~uint8_t(mask)));

    if (!d.readVarU32(&limits->initial))
        return d.fail("expected initial length");

    if (flags & uint8_t(MemoryTableFlags::HasMaximum)) {
        uint32_t maximum;
        if (!d.readVarU32(&maximum))
            return d.fail("expected maximum length");

        if (limits->initial > maximum) {
            return d.failf("memory size minimum must not be greater than maximum; "
                           "maximum length %" PRIu32 " is less than initial length %" PRIu32,
                           maximum, limits->initial);
        }

        limits->maximum.emplace(maximum);
    }

    limits->shared = Shareable::False;

#ifdef ENABLE_WASM_THREAD_OPS
    if (allowShared == Shareable::True) {
        if ((flags & uint8_t(MemoryTableFlags::IsShared)) && !(flags & uint8_t(MemoryTableFlags::HasMaximum)))
            return d.fail("maximum length required for shared memory");

        limits->shared = (flags & uint8_t(MemoryTableFlags::IsShared))
                       ? Shareable::True
                       : Shareable::False;
    }
#endif

    return true;
}

static bool
DecodeTableLimits(Decoder& d, TableDescVector* tables)
{
    uint8_t elementType;
    if (!d.readFixedU8(&elementType))
        return d.fail("expected table element type");

    if (elementType != uint8_t(TypeCode::AnyFunc))
        return d.fail("expected 'anyfunc' element type");

    Limits limits;
    if (!DecodeLimits(d, &limits))
        return false;

    if (limits.initial > MaxTableInitialLength)
        return d.fail("too many table elements");

    if (tables->length())
        return d.fail("already have default table");

    return tables->emplaceBack(TableKind::AnyFunction, limits);
}

static bool
GlobalIsJSCompatible(Decoder& d, ValType type, bool isMutable)
{
    switch (type) {
      case ValType::I32:
      case ValType::F32:
      case ValType::F64:
      case ValType::I64:
        break;
      default:
        return d.fail("unexpected variable type in global import/export");
    }

    if (isMutable)
        return d.fail("can't import/export mutable globals in the MVP");

    return true;
}

static bool
DecodeGlobalType(Decoder& d, ValType* type, bool* isMutable)
{
    if (!DecodeValType(d, ModuleKind::Wasm, type))
        return false;

    uint8_t flags;
    if (!d.readFixedU8(&flags))
        return d.fail("expected global flags");

    if (flags & ~uint8_t(GlobalTypeImmediate::AllowedMask))
        return d.fail("unexpected bits set in global flags");

    *isMutable = flags & uint8_t(GlobalTypeImmediate::IsMutable);
    return true;
}

static bool
DecodeMemoryLimits(Decoder& d, ModuleEnvironment* env)
{
    if (env->usesMemory())
        return d.fail("already have default memory");

    Limits memory;
    if (!DecodeLimits(d, &memory, Shareable::True))
        return false;

    if (memory.initial > MaxMemoryInitialPages)
        return d.fail("initial memory size too big");

    CheckedInt<uint32_t> initialBytes = memory.initial;
    initialBytes *= PageSize;
    MOZ_ASSERT(initialBytes.isValid());
    memory.initial = initialBytes.value();

    if (memory.maximum) {
        if (*memory.maximum > MaxMemoryMaximumPages)
            return d.fail("maximum memory size too big");

        CheckedInt<uint32_t> maximumBytes = *memory.maximum;
        maximumBytes *= PageSize;

        // Clamp the maximum memory value to UINT32_MAX; it's not semantically
        // visible since growing will fail for values greater than INT32_MAX.
        memory.maximum = Some(maximumBytes.isValid() ? maximumBytes.value() : UINT32_MAX);
    }

    if (memory.shared == Shareable::True && env->sharedMemoryEnabled == Shareable::False)
        return d.fail("shared memory is disabled");

    env->memoryUsage = memory.shared == Shareable::True
                     ? MemoryUsage::Shared
                     : MemoryUsage::Unshared;
    env->minMemoryLength = memory.initial;
    env->maxMemoryLength = memory.maximum;
    return true;
}

static bool
DecodeImport(Decoder& d, ModuleEnvironment* env)
{
    UniqueChars moduleName = DecodeName(d);
    if (!moduleName)
        return d.fail("expected valid import module name");

    UniqueChars funcName = DecodeName(d);
    if (!funcName)
        return d.fail("expected valid import func name");

    uint8_t rawImportKind;
    if (!d.readFixedU8(&rawImportKind))
        return d.fail("failed to read import kind");

    DefinitionKind importKind = DefinitionKind(rawImportKind);

    switch (importKind) {
      case DefinitionKind::Function: {
        uint32_t sigIndex;
        if (!DecodeSignatureIndex(d, env->sigs, &sigIndex))
            return false;
        if (!env->funcSigs.append(&env->sigs[sigIndex]))
            return false;
        if (env->funcSigs.length() > MaxFuncs)
            return d.fail("too many functions");
        break;
      }
      case DefinitionKind::Table: {
        if (!DecodeTableLimits(d, &env->tables))
            return false;
        env->tables.back().external = true;
        break;
      }
      case DefinitionKind::Memory: {
        if (!DecodeMemoryLimits(d, env))
            return false;
        break;
      }
      case DefinitionKind::Global: {
        ValType type;
        bool isMutable;
        if (!DecodeGlobalType(d, &type, &isMutable))
            return false;
        if (!GlobalIsJSCompatible(d, type, isMutable))
            return false;
        if (!env->globals.append(GlobalDesc(type, isMutable, env->globals.length())))
            return false;
        if (env->globals.length() > MaxGlobals)
            return d.fail("too many globals");
        break;
      }
      default:
        return d.fail("unsupported import kind");
    }

    return env->imports.emplaceBack(Move(moduleName), Move(funcName), importKind);
}

static bool
DecodeImportSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Import, env, &range, "import"))
        return false;
    if (!range)
        return true;

    uint32_t numImports;
    if (!d.readVarU32(&numImports))
        return d.fail("failed to read number of imports");

    if (numImports > MaxImports)
        return d.fail("too many imports");

    for (uint32_t i = 0; i < numImports; i++) {
        if (!DecodeImport(d, env))
            return false;
    }

    if (!d.finishSection(*range, "import"))
        return false;

    // The global data offsets will be filled in by ModuleGenerator::init.
    if (!env->funcImportGlobalDataOffsets.resize(env->funcSigs.length()))
        return false;

    return true;
}

static bool
DecodeFunctionSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Function, env, &range, "function"))
        return false;
    if (!range)
        return true;

    uint32_t numDefs;
    if (!d.readVarU32(&numDefs))
        return d.fail("expected number of function definitions");

    CheckedInt<uint32_t> numFuncs = env->funcSigs.length();
    numFuncs += numDefs;
    if (!numFuncs.isValid() || numFuncs.value() > MaxFuncs)
        return d.fail("too many functions");

    if (!env->funcSigs.reserve(numFuncs.value()))
        return false;

    for (uint32_t i = 0; i < numDefs; i++) {
        uint32_t sigIndex;
        if (!DecodeSignatureIndex(d, env->sigs, &sigIndex))
            return false;
        env->funcSigs.infallibleAppend(&env->sigs[sigIndex]);
    }

    return d.finishSection(*range, "function");
}

static bool
DecodeTableSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Table, env, &range, "table"))
        return false;
    if (!range)
        return true;

    uint32_t numTables;
    if (!d.readVarU32(&numTables))
        return d.fail("failed to read number of tables");

    if (numTables > 1)
        return d.fail("the number of tables must be at most one");

    for (uint32_t i = 0; i < numTables; ++i) {
        if (!DecodeTableLimits(d, &env->tables))
            return false;
    }

    return d.finishSection(*range, "table");
}

static bool
DecodeMemorySection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Memory, env, &range, "memory"))
        return false;
    if (!range)
        return true;

    uint32_t numMemories;
    if (!d.readVarU32(&numMemories))
        return d.fail("failed to read number of memories");

    if (numMemories > 1)
        return d.fail("the number of memories must be at most one");

    for (uint32_t i = 0; i < numMemories; ++i) {
        if (!DecodeMemoryLimits(d, env))
            return false;
    }

    return d.finishSection(*range, "memory");
}

static bool
DecodeInitializerExpression(Decoder& d, const GlobalDescVector& globals, ValType expected,
                            InitExpr* init)
{
    OpBytes op;
    if (!d.readOp(&op))
        return d.fail("failed to read initializer type");

    switch (op.b0) {
      case uint16_t(Op::I32Const): {
        int32_t i32;
        if (!d.readVarS32(&i32))
            return d.fail("failed to read initializer i32 expression");
        *init = InitExpr(Val(uint32_t(i32)));
        break;
      }
      case uint16_t(Op::I64Const): {
        int64_t i64;
        if (!d.readVarS64(&i64))
            return d.fail("failed to read initializer i64 expression");
        *init = InitExpr(Val(uint64_t(i64)));
        break;
      }
      case uint16_t(Op::F32Const): {
        float f32;
        if (!d.readFixedF32(&f32))
            return d.fail("failed to read initializer f32 expression");
        *init = InitExpr(Val(f32));
        break;
      }
      case uint16_t(Op::F64Const): {
        double f64;
        if (!d.readFixedF64(&f64))
            return d.fail("failed to read initializer f64 expression");
        *init = InitExpr(Val(f64));
        break;
      }
      case uint16_t(Op::GetGlobal): {
        uint32_t i;
        if (!d.readVarU32(&i))
            return d.fail("failed to read get_global index in initializer expression");
        if (i >= globals.length())
            return d.fail("global index out of range in initializer expression");
        if (!globals[i].isImport() || globals[i].isMutable())
            return d.fail("initializer expression must reference a global immutable import");
        *init = InitExpr(i, globals[i].type());
        break;
      }
      default: {
        return d.fail("unexpected initializer expression");
      }
    }

    if (expected != init->type())
        return d.fail("type mismatch: initializer type and expected type don't match");

    OpBytes end;
    if (!d.readOp(&end) || end.b0 != uint16_t(Op::End))
        return d.fail("failed to read end of initializer expression");

    return true;
}

static bool
DecodeGlobalSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Global, env, &range, "global"))
        return false;
    if (!range)
        return true;

    uint32_t numDefs;
    if (!d.readVarU32(&numDefs))
        return d.fail("expected number of globals");

    CheckedInt<uint32_t> numGlobals = env->globals.length();
    numGlobals += numDefs;
    if (!numGlobals.isValid() || numGlobals.value() > MaxGlobals)
        return d.fail("too many globals");

    if (!env->globals.reserve(numGlobals.value()))
        return false;

    for (uint32_t i = 0; i < numDefs; i++) {
        ValType type;
        bool isMutable;
        if (!DecodeGlobalType(d, &type, &isMutable))
            return false;

        InitExpr initializer;
        if (!DecodeInitializerExpression(d, env->globals, type, &initializer))
            return false;

        env->globals.infallibleAppend(GlobalDesc(initializer, isMutable));
    }

    return d.finishSection(*range, "global");
}

typedef HashSet<const char*, CStringHasher, SystemAllocPolicy> CStringSet;

static UniqueChars
DecodeExportName(Decoder& d, CStringSet* dupSet)
{
    UniqueChars exportName = DecodeName(d);
    if (!exportName) {
        d.fail("expected valid export name");
        return nullptr;
    }

    CStringSet::AddPtr p = dupSet->lookupForAdd(exportName.get());
    if (p) {
        d.fail("duplicate export");
        return nullptr;
    }

    if (!dupSet->add(p, exportName.get()))
        return nullptr;

    return Move(exportName);
}

static bool
DecodeExport(Decoder& d, ModuleEnvironment* env, CStringSet* dupSet)
{
    UniqueChars fieldName = DecodeExportName(d, dupSet);
    if (!fieldName)
        return false;

    uint8_t exportKind;
    if (!d.readFixedU8(&exportKind))
        return d.fail("failed to read export kind");

    switch (DefinitionKind(exportKind)) {
      case DefinitionKind::Function: {
        uint32_t funcIndex;
        if (!d.readVarU32(&funcIndex))
            return d.fail("expected function index");

        if (funcIndex >= env->numFuncs())
            return d.fail("exported function index out of bounds");

        return env->exports.emplaceBack(Move(fieldName), funcIndex, DefinitionKind::Function);
      }
      case DefinitionKind::Table: {
        uint32_t tableIndex;
        if (!d.readVarU32(&tableIndex))
            return d.fail("expected table index");

        if (tableIndex >= env->tables.length())
            return d.fail("exported table index out of bounds");

        MOZ_ASSERT(env->tables.length() == 1);
        env->tables[0].external = true;

        return env->exports.emplaceBack(Move(fieldName), DefinitionKind::Table);
      }
      case DefinitionKind::Memory: {
        uint32_t memoryIndex;
        if (!d.readVarU32(&memoryIndex))
            return d.fail("expected memory index");

        if (memoryIndex > 0 || !env->usesMemory())
            return d.fail("exported memory index out of bounds");

        return env->exports.emplaceBack(Move(fieldName), DefinitionKind::Memory);
      }
      case DefinitionKind::Global: {
        uint32_t globalIndex;
        if (!d.readVarU32(&globalIndex))
            return d.fail("expected global index");

        if (globalIndex >= env->globals.length())
            return d.fail("exported global index out of bounds");

        const GlobalDesc& global = env->globals[globalIndex];
        if (!GlobalIsJSCompatible(d, global.type(), global.isMutable()))
            return false;

        return env->exports.emplaceBack(Move(fieldName), globalIndex, DefinitionKind::Global);
      }
      default:
        return d.fail("unexpected export kind");
    }

    MOZ_CRASH("unreachable");
}

static bool
DecodeExportSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Export, env, &range, "export"))
        return false;
    if (!range)
        return true;

    CStringSet dupSet;
    if (!dupSet.init())
        return false;

    uint32_t numExports;
    if (!d.readVarU32(&numExports))
        return d.fail("failed to read number of exports");

    if (numExports > MaxExports)
        return d.fail("too many exports");

    for (uint32_t i = 0; i < numExports; i++) {
        if (!DecodeExport(d, env, &dupSet))
            return false;
    }

    return d.finishSection(*range, "export");
}

static bool
DecodeStartSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Start, env, &range, "start"))
        return false;
    if (!range)
        return true;

    uint32_t funcIndex;
    if (!d.readVarU32(&funcIndex))
        return d.fail("failed to read start func index");

    if (funcIndex >= env->numFuncs())
        return d.fail("unknown start function");

    const Sig& sig = *env->funcSigs[funcIndex];
    if (!IsVoid(sig.ret()))
        return d.fail("start function must not return anything");

    if (sig.args().length())
        return d.fail("start function must be nullary");

    env->startFuncIndex = Some(funcIndex);

    return d.finishSection(*range, "start");
}

static bool
DecodeElemSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Elem, env, &range, "elem"))
        return false;
    if (!range)
        return true;

    uint32_t numSegments;
    if (!d.readVarU32(&numSegments))
        return d.fail("failed to read number of elem segments");

    if (numSegments > MaxElemSegments)
        return d.fail("too many elem segments");

    for (uint32_t i = 0; i < numSegments; i++) {
        uint32_t tableIndex;
        if (!d.readVarU32(&tableIndex))
            return d.fail("expected table index");

        MOZ_ASSERT(env->tables.length() <= 1);
        if (tableIndex >= env->tables.length())
            return d.fail("table index out of range");

        InitExpr offset;
        if (!DecodeInitializerExpression(d, env->globals, ValType::I32, &offset))
            return false;

        uint32_t numElems;
        if (!d.readVarU32(&numElems))
            return d.fail("expected segment size");

        if (numElems > MaxTableInitialLength)
            return d.fail("too many table elements");

        Uint32Vector elemFuncIndices;
        if (!elemFuncIndices.resize(numElems))
            return false;

        for (uint32_t i = 0; i < numElems; i++) {
            if (!d.readVarU32(&elemFuncIndices[i]))
                return d.fail("failed to read element function index");
            if (elemFuncIndices[i] >= env->numFuncs())
                return d.fail("table element out of range");
        }

        if (!env->elemSegments.emplaceBack(0, offset, Move(elemFuncIndices)))
            return false;

        env->tables[env->elemSegments.back().tableIndex].external = true;
    }

    return d.finishSection(*range, "elem");
}

bool
wasm::StartsCodeSection(const uint8_t* begin, const uint8_t* end, SectionRange* codeSection)
{
    UniqueChars unused;
    Decoder d(begin, end, 0, &unused);

    if (!DecodePreamble(d))
        return false;

    while (!d.done()) {
        uint8_t id;
        SectionRange range;
        if (!d.readSectionHeader(&id, &range))
            return false;

        if (id == uint8_t(SectionId::Code)) {
            *codeSection = range;
            return true;
        }

        if (!d.readBytes(range.size))
            return false;
    }

    return false;
}

bool
wasm::DecodeModuleEnvironment(Decoder& d, ModuleEnvironment* env)
{
    if (!DecodePreamble(d))
        return false;

    if (!DecodeTypeSection(d, env))
        return false;

    if (!DecodeImportSection(d, env))
        return false;

    if (!DecodeFunctionSection(d, env))
        return false;

    if (!DecodeTableSection(d, env))
        return false;

    if (!DecodeMemorySection(d, env))
        return false;

    if (!DecodeGlobalSection(d, env))
        return false;

    if (!DecodeExportSection(d, env))
        return false;

    if (!DecodeStartSection(d, env))
        return false;

    if (!DecodeElemSection(d, env))
        return false;

    if (!d.startSection(SectionId::Code, env, &env->codeSection, "code"))
        return false;

    if (env->codeSection && env->codeSection->size > MaxCodeSectionBytes)
        return d.fail("code section too big");

    return true;
}

static bool
DecodeFunctionBody(Decoder& d, const ModuleEnvironment& env, uint32_t funcIndex)
{
    uint32_t bodySize;
    if (!d.readVarU32(&bodySize))
        return d.fail("expected number of function body bytes");

    if (bodySize > MaxFunctionBytes)
        return d.fail("function body too big");

    if (d.bytesRemain() < bodySize)
        return d.fail("function body length too big");

    if (!ValidateFunctionBody(env, funcIndex, bodySize, d))
        return false;

    return true;
}

static bool
DecodeCodeSection(Decoder& d, ModuleEnvironment* env)
{
    if (!env->codeSection) {
        if (env->numFuncDefs() != 0)
            return d.fail("expected function bodies");
        return true;
    }

    uint32_t numFuncDefs;
    if (!d.readVarU32(&numFuncDefs))
        return d.fail("expected function body count");

    if (numFuncDefs != env->numFuncDefs())
        return d.fail("function body count does not match function signature count");

    for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
        if (!DecodeFunctionBody(d, *env, env->numFuncImports() + funcDefIndex))
            return false;
    }

    return d.finishSection(*env->codeSection, "code");
}

static bool
DecodeDataSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startSection(SectionId::Data, env, &range, "data"))
        return false;
    if (!range)
        return true;

    uint32_t numSegments;
    if (!d.readVarU32(&numSegments))
        return d.fail("failed to read number of data segments");

    if (numSegments > MaxDataSegments)
        return d.fail("too many data segments");

    for (uint32_t i = 0; i < numSegments; i++) {
        uint32_t linearMemoryIndex;
        if (!d.readVarU32(&linearMemoryIndex))
            return d.fail("expected linear memory index");

        if (linearMemoryIndex != 0)
            return d.fail("linear memory index must currently be 0");

        if (!env->usesMemory())
            return d.fail("data segment requires a memory section");

        DataSegment seg;
        if (!DecodeInitializerExpression(d, env->globals, ValType::I32, &seg.offset))
            return false;

        if (!d.readVarU32(&seg.length))
            return d.fail("expected segment size");

        if (seg.length > MaxMemoryInitialPages * PageSize)
            return d.fail("segment size too big");

        seg.bytecodeOffset = d.currentOffset();

        if (!d.readBytes(seg.length))
            return d.fail("data segment shorter than declared");

        if (!env->dataSegments.append(seg))
            return false;
    }

    return d.finishSection(*range, "data");
}

static bool
DecodeModuleNameSubsection(Decoder& d)
{
    Maybe<uint32_t> endOffset;
    if (!d.startNameSubsection(NameType::Module, &endOffset))
        return false;
    if (!endOffset)
        return true;

    // Don't use NameInBytecode for module name; instead store a copy of the
    // string. This way supplying a module name doesn't need to save the whole
    // bytecode. While function names are likely to be stripped in practice,
    // module names aren't necessarily.

    uint32_t nameLength;
    if (!d.readVarU32(&nameLength))
        return false;

    const uint8_t* bytes;
    if (!d.readBytes(nameLength, &bytes))
        return false;

    // Do nothing with module name for now; a future patch will incorporate the
    // module name into the callstack format.

    return d.finishNameSubsection(*endOffset);
}

static bool
DecodeFunctionNameSubsection(Decoder& d, ModuleEnvironment* env)
{
    Maybe<uint32_t> endOffset;
    if (!d.startNameSubsection(NameType::Function, &endOffset))
        return false;
    if (!endOffset)
        return true;

    uint32_t nameCount = 0;
    if (!d.readVarU32(&nameCount) || nameCount > MaxFuncs)
        return false;

    NameInBytecodeVector funcNames;

    for (uint32_t i = 0; i < nameCount; ++i) {
        uint32_t funcIndex = 0;
        if (!d.readVarU32(&funcIndex))
            return false;

        // Names must refer to real functions and be given in ascending order.
        if (funcIndex >= env->numFuncs() || funcIndex < funcNames.length())
            return false;

        uint32_t nameLength = 0;
        if (!d.readVarU32(&nameLength) || nameLength > MaxStringLength)
            return false;

        if (!nameLength)
            continue;

        if (!funcNames.resize(funcIndex + 1))
            return false;

        funcNames[funcIndex] = NameInBytecode(d.currentOffset(), nameLength);

        if (!d.readBytes(nameLength))
            return false;
    }

    if (!d.finishNameSubsection(*endOffset))
        return false;

    // To encourage fully valid function names subsections; only save names if
    // the entire subsection decoded correctly.
    env->funcNames = Move(funcNames);
    return true;
}

static bool
DecodeNameSection(Decoder& d, ModuleEnvironment* env)
{
    MaybeSectionRange range;
    if (!d.startCustomSection(NameSectionName, env, &range))
        return false;
    if (!range)
        return true;

    // Once started, custom sections do not report validation errors.

    if (!DecodeModuleNameSubsection(d))
        goto finish;

    if (!DecodeFunctionNameSubsection(d, env))
        goto finish;

    // The names we care about have already been extracted into 'env' so don't
    // bother decoding the rest of the name section. finishCustomSection() will
    // skip to the end of the name section (as it would for any other error).

  finish:
    d.finishCustomSection(*range);
    return true;
}

bool
wasm::DecodeModuleTail(Decoder& d, ModuleEnvironment* env)
{
    if (!DecodeDataSection(d, env))
        return false;

    if (!DecodeNameSection(d, env))
        return false;

    while (!d.done()) {
        if (!d.skipCustomSection(env)) {
            if (d.resilientMode()) {
                d.clearError();
                return true;
            }
            return false;
        }
    }

    return true;
}

// Validate algorithm.

bool
wasm::Validate(JSContext* cx, const ShareableBytes& bytecode, UniqueChars* error)
{
    Decoder d(bytecode.bytes, 0, error);

    ModuleEnvironment env(CompileMode::Once, Tier::Ion, DebugEnabled::False,
                          cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled()
                          ? Shareable::True
                          : Shareable::False);
    if (!DecodeModuleEnvironment(d, &env))
        return false;

    if (!DecodeCodeSection(d, &env))
        return false;

    if (!DecodeModuleTail(d, &env))
        return false;

    MOZ_ASSERT(!*error, "unreported error in decoding");
    return true;
}
