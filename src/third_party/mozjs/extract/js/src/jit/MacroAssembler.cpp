/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MacroAssembler-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/MathAlgorithms.h"

#include "jsfriendapi.h"

#include "builtin/TypedObject.h"
#include "gc/GCTrace.h"
#include "jit/AtomicOp.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/JitOptions.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "js/Conversions.h"
#include "js/Printf.h"
#include "vm/TraceLogging.h"

#include "gc/Nursery-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/TypeInference-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using JS::ToInt32;

using mozilla::CheckedUint32;

template <typename T>
static void
EmitTypeCheck(MacroAssembler& masm, Assembler::Condition cond, const T& src, TypeSet::Type type,
              Label* label)
{
    if (type.isAnyObject()) {
        masm.branchTestObject(cond, src, label);
        return;
    }
    switch (type.primitive()) {
      case JSVAL_TYPE_DOUBLE:
        // TI double type includes int32.
        masm.branchTestNumber(cond, src, label);
        break;
      case JSVAL_TYPE_INT32:
        masm.branchTestInt32(cond, src, label);
        break;
      case JSVAL_TYPE_BOOLEAN:
        masm.branchTestBoolean(cond, src, label);
        break;
      case JSVAL_TYPE_STRING:
        masm.branchTestString(cond, src, label);
        break;
      case JSVAL_TYPE_SYMBOL:
        masm.branchTestSymbol(cond, src, label);
        break;
      case JSVAL_TYPE_NULL:
        masm.branchTestNull(cond, src, label);
        break;
      case JSVAL_TYPE_UNDEFINED:
        masm.branchTestUndefined(cond, src, label);
        break;
      case JSVAL_TYPE_MAGIC:
        masm.branchTestMagic(cond, src, label);
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
}

template <typename Source> void
MacroAssembler::guardTypeSet(const Source& address, const TypeSet* types, BarrierKind kind,
                             Register unboxScratch, Register objScratch,
                             Register spectreRegToZero, Label* miss)
{
    // unboxScratch may be InvalidReg on 32-bit platforms. It should only be
    // used for extracting the Value tag or payload.
    //
    // objScratch may be InvalidReg if the TypeSet does not contain specific
    // objects to guard on. It should only be used for guardObjectType.
    //
    // spectreRegToZero is a register that will be zeroed by guardObjectType on
    // speculatively executed paths.

    MOZ_ASSERT(kind == BarrierKind::TypeTagOnly || kind == BarrierKind::TypeSet);
    MOZ_ASSERT(!types->unknown());

    Label matched;
    TypeSet::Type tests[8] = {
        TypeSet::Int32Type(),
        TypeSet::UndefinedType(),
        TypeSet::BooleanType(),
        TypeSet::StringType(),
        TypeSet::SymbolType(),
        TypeSet::NullType(),
        TypeSet::MagicArgType(),
        TypeSet::AnyObjectType()
    };

    // The double type also implies Int32.
    // So replace the int32 test with the double one.
    if (types->hasType(TypeSet::DoubleType())) {
        MOZ_ASSERT(types->hasType(TypeSet::Int32Type()));
        tests[0] = TypeSet::DoubleType();
    }

    unsigned numBranches = 0;
    for (size_t i = 0; i < mozilla::ArrayLength(tests); i++) {
        if (types->hasType(tests[i]))
            numBranches++;
    }

    if (!types->unknownObject() && types->getObjectCount() > 0)
        numBranches++;

    if (numBranches == 0) {
        MOZ_ASSERT(types->empty());
        jump(miss);
        return;
    }

    Register tag = extractTag(address, unboxScratch);

    // Emit all typed tests.
    for (size_t i = 0; i < mozilla::ArrayLength(tests); i++) {
        if (!types->hasType(tests[i]))
            continue;

        if (--numBranches > 0)
            EmitTypeCheck(*this, Equal, tag, tests[i], &matched);
        else
            EmitTypeCheck(*this, NotEqual, tag, tests[i], miss);
    }

    // If we don't have specific objects to check for, we're done.
    if (numBranches == 0) {
        MOZ_ASSERT(types->unknownObject() || types->getObjectCount() == 0);
        bind(&matched);
        return;
    }

    // Test specific objects.
    MOZ_ASSERT(objScratch != InvalidReg);
    MOZ_ASSERT(objScratch != unboxScratch);

    MOZ_ASSERT(numBranches == 1);
    branchTestObject(NotEqual, tag, miss);

    if (kind != BarrierKind::TypeTagOnly) {
        Register obj = extractObject(address, unboxScratch);
        guardObjectType(obj, types, objScratch, spectreRegToZero, miss);
    } else {
#ifdef DEBUG
        Label fail;
        Register obj = extractObject(address, unboxScratch);
        guardObjectType(obj, types, objScratch, spectreRegToZero, &fail);
        jump(&matched);

        bind(&fail);
        guardTypeSetMightBeIncomplete(types, obj, objScratch, &matched);
        assumeUnreachable("Unexpected object type");
#endif
    }

    bind(&matched);
}

#ifdef DEBUG
// guardTypeSetMightBeIncomplete is only used in DEBUG builds. If this ever
// changes, we need to make sure it's Spectre-safe.
void
MacroAssembler::guardTypeSetMightBeIncomplete(const TypeSet* types, Register obj,
                                              Register scratch, Label* label)
{
    // Type set guards might miss when an object's group changes. In this case
    // either its old group's properties will become unknown, or it will change
    // to a native object with an original unboxed group. Jump to label if this
    // might have happened for the input object.

    if (types->unknownObject()) {
        jump(label);
        return;
    }

    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    load32(Address(scratch, ObjectGroup::offsetOfFlags()), scratch);
    and32(Imm32(OBJECT_FLAG_ADDENDUM_MASK), scratch);
    branch32(Assembler::Equal,
             scratch, Imm32(ObjectGroup::addendumOriginalUnboxedGroupValue()), label);

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        if (JSObject* singleton = getSingletonAndDelayBarrier(types, i)) {
            movePtr(ImmGCPtr(singleton), scratch);
            loadPtr(Address(scratch, JSObject::offsetOfGroup()), scratch);
        } else if (ObjectGroup* group = getGroupAndDelayBarrier(types, i)) {
            movePtr(ImmGCPtr(group), scratch);
        } else {
            continue;
        }
        branchTest32(Assembler::NonZero, Address(scratch, ObjectGroup::offsetOfFlags()),
                     Imm32(OBJECT_FLAG_UNKNOWN_PROPERTIES), label);
    }
}
#endif

void
MacroAssembler::guardObjectType(Register obj, const TypeSet* types, Register scratch,
                                Register spectreRegToZero, Label* miss)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(!types->unknown());
    MOZ_ASSERT(!types->hasType(TypeSet::AnyObjectType()));
    MOZ_ASSERT_IF(types->getObjectCount() > 0, scratch != InvalidReg);

    // Note: this method elides read barriers on values read from type sets, as
    // this may be called off thread during Ion compilation. This is
    // safe to do as the final JitCode object will be allocated during the
    // incremental GC (or the compilation canceled before we start sweeping),
    // see CodeGenerator::link. Other callers should use TypeSet::readBarrier
    // to trigger the barrier on the contents of type sets passed in here.
    Label matched;

    bool hasSingletons = false;
    bool hasObjectGroups = false;
    unsigned numBranches = 0;

    unsigned count = types->getObjectCount();
    for (unsigned i = 0; i < count; i++) {
        if (types->hasGroup(i)) {
            hasObjectGroups = true;
            numBranches++;
        } else if (types->hasSingleton(i)) {
            hasSingletons = true;
            numBranches++;
        }
    }

    if (numBranches == 0) {
        jump(miss);
        return;
    }

    if (JitOptions.spectreObjectMitigationsBarriers)
        move32(Imm32(0), scratch);

    if (hasSingletons) {
        for (unsigned i = 0; i < count; i++) {
            JSObject* singleton = getSingletonAndDelayBarrier(types, i);
            if (!singleton)
                continue;

            if (JitOptions.spectreObjectMitigationsBarriers) {
                if (--numBranches > 0) {
                    Label next;
                    branchPtr(NotEqual, obj, ImmGCPtr(singleton), &next);
                    spectreMovePtr(NotEqual, scratch, spectreRegToZero);
                    jump(&matched);
                    bind(&next);
                } else {
                    branchPtr(NotEqual, obj, ImmGCPtr(singleton), miss);
                    spectreMovePtr(NotEqual, scratch, spectreRegToZero);
                }
            } else {
                if (--numBranches > 0)
                    branchPtr(Equal, obj, ImmGCPtr(singleton), &matched);
                else
                    branchPtr(NotEqual, obj, ImmGCPtr(singleton), miss);
            }
        }
    }

    if (hasObjectGroups) {
        comment("has object groups");

        // If Spectre mitigations are enabled, we use the scratch register as
        // zero register. Without mitigations we can use it to store the group.
        Address groupAddr(obj, JSObject::offsetOfGroup());
        if (!JitOptions.spectreObjectMitigationsBarriers)
            loadPtr(groupAddr, scratch);

        for (unsigned i = 0; i < count; i++) {
            ObjectGroup* group = getGroupAndDelayBarrier(types, i);
            if (!group)
                continue;

            if (!pendingObjectGroupReadBarriers_.append(group)) {
                setOOM();
                return;
            }

            if (JitOptions.spectreObjectMitigationsBarriers) {
                if (--numBranches > 0) {
                    Label next;
                    branchPtr(NotEqual, groupAddr, ImmGCPtr(group), &next);
                    spectreMovePtr(NotEqual, scratch, spectreRegToZero);
                    jump(&matched);
                    bind(&next);
                } else {
                    branchPtr(NotEqual, groupAddr, ImmGCPtr(group), miss);
                    spectreMovePtr(NotEqual, scratch, spectreRegToZero);
                }
            } else {
                if (--numBranches > 0)
                    branchPtr(Equal, scratch, ImmGCPtr(group), &matched);
                else
                    branchPtr(NotEqual, scratch, ImmGCPtr(group), miss);
            }
        }
    }

    MOZ_ASSERT(numBranches == 0);

    bind(&matched);
}

template void MacroAssembler::guardTypeSet(const Address& address, const TypeSet* types,
                                           BarrierKind kind, Register unboxScratch,
                                           Register objScratch, Register spectreRegToZero,
                                           Label* miss);
template void MacroAssembler::guardTypeSet(const ValueOperand& value, const TypeSet* types,
                                           BarrierKind kind, Register unboxScratch,
                                           Register objScratch, Register spectreRegToZero,
                                           Label* miss);
template void MacroAssembler::guardTypeSet(const TypedOrValueRegister& value, const TypeSet* types,
                                           BarrierKind kind, Register unboxScratch,
                                           Register objScratch, Register spectreRegToZero,
                                           Label* miss);

template<typename S, typename T>
static void
StoreToTypedFloatArray(MacroAssembler& masm, int arrayType, const S& value, const T& dest,
                       unsigned numElems)
{
    switch (arrayType) {
      case Scalar::Float32:
        masm.storeFloat32(value, dest);
        break;
      case Scalar::Float64:
        masm.storeDouble(value, dest);
        break;
      case Scalar::Float32x4:
        switch (numElems) {
          case 1:
            masm.storeFloat32(value, dest);
            break;
          case 2:
            masm.storeDouble(value, dest);
            break;
          case 3:
            masm.storeFloat32x3(value, dest);
            break;
          case 4:
            masm.storeUnalignedSimd128Float(value, dest);
            break;
          default: MOZ_CRASH("unexpected number of elements in simd write");
        }
        break;
      case Scalar::Int32x4:
        switch (numElems) {
          case 1:
            masm.storeInt32x1(value, dest);
            break;
          case 2:
            masm.storeInt32x2(value, dest);
            break;
          case 3:
            masm.storeInt32x3(value, dest);
            break;
          case 4:
            masm.storeUnalignedSimd128Int(value, dest);
            break;
          default: MOZ_CRASH("unexpected number of elements in simd write");
        }
        break;
      case Scalar::Int8x16:
        MOZ_ASSERT(numElems == 16, "unexpected partial store");
        masm.storeUnalignedSimd128Int(value, dest);
        break;
      case Scalar::Int16x8:
        MOZ_ASSERT(numElems == 8, "unexpected partial store");
        masm.storeUnalignedSimd128Int(value, dest);
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

void
MacroAssembler::storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value,
                                       const BaseIndex& dest, unsigned numElems)
{
    StoreToTypedFloatArray(*this, arrayType, value, dest, numElems);
}
void
MacroAssembler::storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value,
                                       const Address& dest, unsigned numElems)
{
    StoreToTypedFloatArray(*this, arrayType, value, dest, numElems);
}

template<typename T>
void
MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const T& src, AnyRegister dest, Register temp,
                                   Label* fail, bool canonicalizeDoubles, unsigned numElems)
{
    switch (arrayType) {
      case Scalar::Int8:
        load8SignExtend(src, dest.gpr());
        break;
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
        load8ZeroExtend(src, dest.gpr());
        break;
      case Scalar::Int16:
        load16SignExtend(src, dest.gpr());
        break;
      case Scalar::Uint16:
        load16ZeroExtend(src, dest.gpr());
        break;
      case Scalar::Int32:
        load32(src, dest.gpr());
        break;
      case Scalar::Uint32:
        if (dest.isFloat()) {
            load32(src, temp);
            convertUInt32ToDouble(temp, dest.fpu());
        } else {
            load32(src, dest.gpr());

            // Bail out if the value doesn't fit into a signed int32 value. This
            // is what allows MLoadUnboxedScalar to have a type() of
            // MIRType::Int32 for UInt32 array loads.
            branchTest32(Assembler::Signed, dest.gpr(), dest.gpr(), fail);
        }
        break;
      case Scalar::Float32:
        loadFloat32(src, dest.fpu());
        canonicalizeFloat(dest.fpu());
        break;
      case Scalar::Float64:
        loadDouble(src, dest.fpu());
        if (canonicalizeDoubles)
            canonicalizeDouble(dest.fpu());
        break;
      case Scalar::Int32x4:
        switch (numElems) {
          case 1:
            loadInt32x1(src, dest.fpu());
            break;
          case 2:
            loadInt32x2(src, dest.fpu());
            break;
          case 3:
            loadInt32x3(src, dest.fpu());
            break;
          case 4:
            loadUnalignedSimd128Int(src, dest.fpu());
            break;
          default: MOZ_CRASH("unexpected number of elements in SIMD load");
        }
        break;
      case Scalar::Float32x4:
        switch (numElems) {
          case 1:
            loadFloat32(src, dest.fpu());
            break;
          case 2:
            loadDouble(src, dest.fpu());
            break;
          case 3:
            loadFloat32x3(src, dest.fpu());
            break;
          case 4:
            loadUnalignedSimd128Float(src, dest.fpu());
            break;
          default: MOZ_CRASH("unexpected number of elements in SIMD load");
        }
        break;
      case Scalar::Int8x16:
        MOZ_ASSERT(numElems == 16, "unexpected partial load");
        loadUnalignedSimd128Int(src, dest.fpu());
        break;
      case Scalar::Int16x8:
        MOZ_ASSERT(numElems == 8, "unexpected partial load");
        loadUnalignedSimd128Int(src, dest.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const Address& src, AnyRegister dest,
                                                 Register temp, Label* fail, bool canonicalizeDoubles,
                                                 unsigned numElems);
template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const BaseIndex& src, AnyRegister dest,
                                                 Register temp, Label* fail, bool canonicalizeDoubles,
                                                 unsigned numElems);

template<typename T>
void
MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const T& src, const ValueOperand& dest,
                                   bool allowDouble, Register temp, Label* fail)
{
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
        loadFromTypedArray(arrayType, src, AnyRegister(dest.scratchReg()), InvalidReg, nullptr);
        tagValue(JSVAL_TYPE_INT32, dest.scratchReg(), dest);
        break;
      case Scalar::Uint32:
        // Don't clobber dest when we could fail, instead use temp.
        load32(src, temp);
        if (allowDouble) {
            // If the value fits in an int32, store an int32 type tag.
            // Else, convert the value to double and box it.
            Label done, isDouble;
            branchTest32(Assembler::Signed, temp, temp, &isDouble);
            {
                tagValue(JSVAL_TYPE_INT32, temp, dest);
                jump(&done);
            }
            bind(&isDouble);
            {
                convertUInt32ToDouble(temp, ScratchDoubleReg);
                boxDouble(ScratchDoubleReg, dest, ScratchDoubleReg);
            }
            bind(&done);
        } else {
            // Bailout if the value does not fit in an int32.
            branchTest32(Assembler::Signed, temp, temp, fail);
            tagValue(JSVAL_TYPE_INT32, temp, dest);
        }
        break;
      case Scalar::Float32:
        loadFromTypedArray(arrayType, src, AnyRegister(ScratchFloat32Reg), dest.scratchReg(),
                           nullptr);
        convertFloat32ToDouble(ScratchFloat32Reg, ScratchDoubleReg);
        boxDouble(ScratchDoubleReg, dest, ScratchDoubleReg);
        break;
      case Scalar::Float64:
        loadFromTypedArray(arrayType, src, AnyRegister(ScratchDoubleReg), dest.scratchReg(),
                           nullptr);
        boxDouble(ScratchDoubleReg, dest, ScratchDoubleReg);
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const Address& src, const ValueOperand& dest,
                                                 bool allowDouble, Register temp, Label* fail);
template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const BaseIndex& src, const ValueOperand& dest,
                                                 bool allowDouble, Register temp, Label* fail);

template <typename T>
void
MacroAssembler::loadUnboxedProperty(T address, JSValueType type, TypedOrValueRegister output)
{
    switch (type) {
      case JSVAL_TYPE_INT32: {
          // Handle loading an int32 into a double reg.
          if (output.type() == MIRType::Double) {
              convertInt32ToDouble(address, output.typedReg().fpu());
              break;
          }
          MOZ_FALLTHROUGH;
      }

      case JSVAL_TYPE_BOOLEAN:
      case JSVAL_TYPE_STRING: {
        Register outReg;
        if (output.hasValue()) {
            outReg = output.valueReg().scratchReg();
        } else {
            MOZ_ASSERT(output.type() == MIRTypeFromValueType(type));
            outReg = output.typedReg().gpr();
        }

        switch (type) {
          case JSVAL_TYPE_BOOLEAN:
            load8ZeroExtend(address, outReg);
            break;
          case JSVAL_TYPE_INT32:
            load32(address, outReg);
            break;
          case JSVAL_TYPE_STRING:
            loadPtr(address, outReg);
            break;
          default:
            MOZ_CRASH();
        }

        if (output.hasValue())
            tagValue(type, outReg, output.valueReg());
        break;
      }

      case JSVAL_TYPE_OBJECT:
        if (output.hasValue()) {
            Register scratch = output.valueReg().scratchReg();
            loadPtr(address, scratch);

            Label notNull, done;
            branchPtr(Assembler::NotEqual, scratch, ImmWord(0), &notNull);

            moveValue(NullValue(), output.valueReg());
            jump(&done);

            bind(&notNull);
            tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());

            bind(&done);
        } else {
            // Reading null can't be possible here, as otherwise the result
            // would be a value (either because null has been read before or
            // because there is a barrier).
            Register reg = output.typedReg().gpr();
            loadPtr(address, reg);
#ifdef DEBUG
            Label ok;
            branchTestPtr(Assembler::NonZero, reg, reg, &ok);
            assumeUnreachable("Null not possible");
            bind(&ok);
#endif
        }
        break;

      case JSVAL_TYPE_DOUBLE:
        // Note: doubles in unboxed objects are not accessed through other
        // views and do not need canonicalization.
        if (output.hasValue())
            loadValue(address, output.valueReg());
        else
            loadDouble(address, output.typedReg().fpu());
        break;

      default:
        MOZ_CRASH();
    }
}

template void
MacroAssembler::loadUnboxedProperty(Address address, JSValueType type,
                                    TypedOrValueRegister output);

template void
MacroAssembler::loadUnboxedProperty(BaseIndex address, JSValueType type,
                                    TypedOrValueRegister output);

static void
StoreUnboxedFailure(MacroAssembler& masm, Label* failure)
{
    // Storing a value to an unboxed property is a fallible operation and
    // the caller must provide a failure label if a particular unboxed store
    // might fail. Sometimes, however, a store that cannot succeed (such as
    // storing a string to an int32 property) will be marked as infallible.
    // This can only happen if the code involved is unreachable.
    if (failure)
        masm.jump(failure);
    else
        masm.assumeUnreachable("Incompatible write to unboxed property");
}

template <typename T>
void
MacroAssembler::storeUnboxedProperty(T address, JSValueType type,
                                     const ConstantOrRegister& value, Label* failure)
{
    switch (type) {
      case JSVAL_TYPE_BOOLEAN:
        if (value.constant()) {
            if (value.value().isBoolean())
                store8(Imm32(value.value().toBoolean()), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else if (value.reg().hasTyped()) {
            if (value.reg().type() == MIRType::Boolean)
                store8(value.reg().typedReg().gpr(), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else {
            if (failure)
                branchTestBoolean(Assembler::NotEqual, value.reg().valueReg(), failure);
            storeUnboxedPayload(value.reg().valueReg(), address, /* width = */ 1, type);
        }
        break;

      case JSVAL_TYPE_INT32:
        if (value.constant()) {
            if (value.value().isInt32())
                store32(Imm32(value.value().toInt32()), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else if (value.reg().hasTyped()) {
            if (value.reg().type() == MIRType::Int32)
                store32(value.reg().typedReg().gpr(), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else {
            if (failure)
                branchTestInt32(Assembler::NotEqual, value.reg().valueReg(), failure);
            storeUnboxedPayload(value.reg().valueReg(), address, /* width = */ 4, type);
        }
        break;

      case JSVAL_TYPE_DOUBLE:
        if (value.constant()) {
            if (value.value().isNumber()) {
                loadConstantDouble(value.value().toNumber(), ScratchDoubleReg);
                storeDouble(ScratchDoubleReg, address);
            } else {
                StoreUnboxedFailure(*this, failure);
            }
        } else if (value.reg().hasTyped()) {
            if (value.reg().type() == MIRType::Int32) {
                convertInt32ToDouble(value.reg().typedReg().gpr(), ScratchDoubleReg);
                storeDouble(ScratchDoubleReg, address);
            } else if (value.reg().type() == MIRType::Double) {
                storeDouble(value.reg().typedReg().fpu(), address);
            } else {
                StoreUnboxedFailure(*this, failure);
            }
        } else {
            ValueOperand reg = value.reg().valueReg();
            Label notInt32, end;
            branchTestInt32(Assembler::NotEqual, reg, &notInt32);
            int32ValueToDouble(reg, ScratchDoubleReg);
            storeDouble(ScratchDoubleReg, address);
            jump(&end);
            bind(&notInt32);
            if (failure)
                branchTestDouble(Assembler::NotEqual, reg, failure);
            storeValue(reg, address);
            bind(&end);
        }
        break;

      case JSVAL_TYPE_OBJECT:
        if (value.constant()) {
            if (value.value().isObjectOrNull())
                storePtr(ImmGCPtr(value.value().toObjectOrNull()), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else if (value.reg().hasTyped()) {
            MOZ_ASSERT(value.reg().type() != MIRType::Null);
            if (value.reg().type() == MIRType::Object)
                storePtr(value.reg().typedReg().gpr(), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else {
            if (failure) {
                Label ok;
                branchTestNull(Assembler::Equal, value.reg().valueReg(), &ok);
                branchTestObject(Assembler::NotEqual, value.reg().valueReg(), failure);
                bind(&ok);
            }
            storeUnboxedPayload(value.reg().valueReg(), address, /* width = */ sizeof(uintptr_t),
                                type);
        }
        break;

      case JSVAL_TYPE_STRING:
        if (value.constant()) {
            if (value.value().isString())
                storePtr(ImmGCPtr(value.value().toString()), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else if (value.reg().hasTyped()) {
            if (value.reg().type() == MIRType::String)
                storePtr(value.reg().typedReg().gpr(), address);
            else
                StoreUnboxedFailure(*this, failure);
        } else {
            if (failure)
                branchTestString(Assembler::NotEqual, value.reg().valueReg(), failure);
            storeUnboxedPayload(value.reg().valueReg(), address, /* width = */ sizeof(uintptr_t),
                                type);
        }
        break;

      default:
        MOZ_CRASH();
    }
}

template void
MacroAssembler::storeUnboxedProperty(Address address, JSValueType type,
                                     const ConstantOrRegister& value, Label* failure);

template void
MacroAssembler::storeUnboxedProperty(BaseIndex address, JSValueType type,
                                     const ConstantOrRegister& value, Label* failure);

// Inlined version of gc::CheckAllocatorState that checks the bare essentials
// and bails for anything that cannot be handled with our jit allocators.
void
MacroAssembler::checkAllocatorState(Label* fail)
{
    // Don't execute the inline path if we are tracing allocations.
    if (js::gc::TraceEnabled())
        jump(fail);

#ifdef JS_GC_ZEAL
    // Don't execute the inline path if gc zeal or tracing are active.
    branch32(Assembler::NotEqual,
             AbsoluteAddress(GetJitContext()->runtime->addressOfGCZealModeBits()), Imm32(0),
             fail);
#endif

    // Don't execute the inline path if the compartment has an object metadata callback,
    // as the metadata to use for the object may vary between executions of the op.
    if (GetJitContext()->compartment->hasAllocationMetadataBuilder())
        jump(fail);
}

bool
MacroAssembler::shouldNurseryAllocate(gc::AllocKind allocKind, gc::InitialHeap initialHeap)
{
    // Note that Ion elides barriers on writes to objects known to be in the
    // nursery, so any allocation that can be made into the nursery must be made
    // into the nursery, even if the nursery is disabled. At runtime these will
    // take the out-of-line path, which is required to insert a barrier for the
    // initializing writes.
    return IsNurseryAllocable(allocKind) && initialHeap != gc::TenuredHeap;
}

// Inline version of Nursery::allocateObject. If the object has dynamic slots,
// this fills in the slots_ pointer.
void
MacroAssembler::nurseryAllocateObject(Register result, Register temp, gc::AllocKind allocKind,
                                      size_t nDynamicSlots, Label* fail)
{
    MOZ_ASSERT(IsNurseryAllocable(allocKind));

    // We still need to allocate in the nursery, per the comment in
    // shouldNurseryAllocate; however, we need to insert into the
    // mallocedBuffers set, so bail to do the nursery allocation in the
    // interpreter.
    if (nDynamicSlots >= Nursery::MaxNurseryBufferSize / sizeof(Value)) {
        jump(fail);
        return;
    }

    // No explicit check for nursery.isEnabled() is needed, as the comparison
    // with the nursery's end will always fail in such cases.
    CompileZone* zone = GetJitContext()->compartment->zone();
    int thingSize = int(gc::Arena::thingSize(allocKind));
    int totalSize = thingSize + nDynamicSlots * sizeof(HeapSlot);
    MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);
    loadPtr(AbsoluteAddress(zone->addressOfNurseryPosition()), result);
    computeEffectiveAddress(Address(result, totalSize), temp);
    branchPtr(Assembler::Below, AbsoluteAddress(zone->addressOfNurseryCurrentEnd()), temp, fail);
    storePtr(temp, AbsoluteAddress(zone->addressOfNurseryPosition()));

    if (nDynamicSlots) {
        computeEffectiveAddress(Address(result, thingSize), temp);
        storePtr(temp, Address(result, NativeObject::offsetOfSlots()));
    }
}

// Inlined version of FreeSpan::allocate. This does not fill in slots_.
void
MacroAssembler::freeListAllocate(Register result, Register temp, gc::AllocKind allocKind, Label* fail)
{
    CompileZone* zone = GetJitContext()->compartment->zone();
    int thingSize = int(gc::Arena::thingSize(allocKind));

    Label fallback;
    Label success;

    // Load the first and last offsets of |zone|'s free list for |allocKind|.
    // If there is no room remaining in the span, fall back to get the next one.
    loadPtr(AbsoluteAddress(zone->addressOfFreeList(allocKind)), temp);
    load16ZeroExtend(Address(temp, js::gc::FreeSpan::offsetOfFirst()), result);
    load16ZeroExtend(Address(temp, js::gc::FreeSpan::offsetOfLast()), temp);
    branch32(Assembler::AboveOrEqual, result, temp, &fallback);

    // Bump the offset for the next allocation.
    add32(Imm32(thingSize), result);
    loadPtr(AbsoluteAddress(zone->addressOfFreeList(allocKind)), temp);
    store16(result, Address(temp, js::gc::FreeSpan::offsetOfFirst()));
    sub32(Imm32(thingSize), result);
    addPtr(temp, result); // Turn the offset into a pointer.
    jump(&success);

    bind(&fallback);
    // If there are no free spans left, we bail to finish the allocation. The
    // interpreter will call the GC allocator to set up a new arena to allocate
    // from, after which we can resume allocating in the jit.
    branchTest32(Assembler::Zero, result, result, fail);
    loadPtr(AbsoluteAddress(zone->addressOfFreeList(allocKind)), temp);
    addPtr(temp, result); // Turn the offset into a pointer.
    Push(result);
    // Update the free list to point to the next span (which may be empty).
    load32(Address(result, 0), result);
    store32(result, Address(temp, js::gc::FreeSpan::offsetOfFirst()));
    Pop(result);

    bind(&success);
}

void
MacroAssembler::callMallocStub(size_t nbytes, Register result, Label* fail)
{
    // These registers must match the ones in JitRuntime::generateMallocStub.
    const Register regReturn = CallTempReg0;
    const Register regZone = CallTempReg0;
    const Register regNBytes = CallTempReg1;

    MOZ_ASSERT(nbytes > 0);
    MOZ_ASSERT(nbytes <= INT32_MAX);

    if (regZone != result)
        push(regZone);
    if (regNBytes != result)
        push(regNBytes);

    move32(Imm32(nbytes), regNBytes);
    movePtr(ImmPtr(GetJitContext()->compartment->zone()), regZone);
    call(GetJitContext()->runtime->jitRuntime()->mallocStub());
    if (regReturn != result)
        movePtr(regReturn, result);

    if (regNBytes != result)
        pop(regNBytes);
    if (regZone != result)
        pop(regZone);

    branchTest32(Assembler::Zero, result, result, fail);
}

void
MacroAssembler::callFreeStub(Register slots)
{
    // This register must match the one in JitRuntime::generateFreeStub.
    const Register regSlots = CallTempReg0;

    push(regSlots);
    movePtr(slots, regSlots);
    call(GetJitContext()->runtime->jitRuntime()->freeStub());
    pop(regSlots);
}

// Inlined equivalent of gc::AllocateObject, without failure case handling.
void
MacroAssembler::allocateObject(Register result, Register temp, gc::AllocKind allocKind,
                               uint32_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail)
{
    MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

    checkAllocatorState(fail);

    if (shouldNurseryAllocate(allocKind, initialHeap)) {
        MOZ_ASSERT(initialHeap == gc::DefaultHeap);
        return nurseryAllocateObject(result, temp, allocKind, nDynamicSlots, fail);
    }

    if (!nDynamicSlots)
        return freeListAllocate(result, temp, allocKind, fail);

    // Only NativeObject can have nDynamicSlots > 0 and reach here.

    callMallocStub(nDynamicSlots * sizeof(GCPtrValue), temp, fail);

    Label failAlloc;
    Label success;

    push(temp);
    freeListAllocate(result, temp, allocKind, &failAlloc);

    pop(temp);
    storePtr(temp, Address(result, NativeObject::offsetOfSlots()));

    jump(&success);

    bind(&failAlloc);
    pop(temp);
    callFreeStub(temp);
    jump(fail);

    bind(&success);
}

void
MacroAssembler::createGCObject(Register obj, Register temp, JSObject* templateObj,
                               gc::InitialHeap initialHeap, Label* fail, bool initContents,
                               bool convertDoubleElements)
{
    gc::AllocKind allocKind = templateObj->asTenured().getAllocKind();
    MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

    uint32_t nDynamicSlots = 0;
    if (templateObj->isNative()) {
        nDynamicSlots = templateObj->as<NativeObject>().numDynamicSlots();

        // Arrays with copy on write elements do not need fixed space for an
        // elements header. The template object, which owns the original
        // elements, might have another allocation kind.
        if (templateObj->as<NativeObject>().denseElementsAreCopyOnWrite())
            allocKind = gc::AllocKind::OBJECT0_BACKGROUND;
    }

    allocateObject(obj, temp, allocKind, nDynamicSlots, initialHeap, fail);
    initGCThing(obj, temp, templateObj, initContents, convertDoubleElements);
}


// Inlined equivalent of gc::AllocateNonObject, without failure case handling.
// Non-object allocation does not need to worry about slots, so can take a
// simpler path.
void
MacroAssembler::allocateNonObject(Register result, Register temp, gc::AllocKind allocKind, Label* fail)
{
    checkAllocatorState(fail);
    freeListAllocate(result, temp, allocKind, fail);
}

// Inline version of Nursery::allocateString.
void
MacroAssembler::nurseryAllocateString(Register result, Register temp, gc::AllocKind allocKind,
                                      Label* fail)
{
    MOZ_ASSERT(IsNurseryAllocable(allocKind));

    // No explicit check for nursery.isEnabled() is needed, as the comparison
    // with the nursery's end will always fail in such cases.

    CompileZone* zone = GetJitContext()->compartment->zone();
    int thingSize = int(gc::Arena::thingSize(allocKind));
    int totalSize = js::Nursery::stringHeaderSize() + thingSize;
    MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

    // The nursery position (allocation pointer) and the nursery end are stored
    // very close to each other. In practice, the zone will probably be close
    // (within 32 bits) as well. If so, use relative offsets between them, to
    // avoid multiple 64-bit immediate loads.
    auto nurseryPosAddr = intptr_t(zone->addressOfStringNurseryPosition());
    auto nurseryEndAddr = intptr_t(zone->addressOfStringNurseryCurrentEnd());
    auto zoneAddr = intptr_t(zone);

    intptr_t maxOffset = std::max(std::abs(nurseryPosAddr - zoneAddr),
                                  std::abs(nurseryEndAddr - zoneAddr));
    if (maxOffset < (1 << 31)) {
        movePtr(ImmPtr(zone), temp); // temp holds the Zone pointer from here on.
        loadPtr(Address(temp, nurseryPosAddr - zoneAddr), result);
        addPtr(Imm32(totalSize), result); // result points past this allocation.
        branchPtr(Assembler::Below, Address(temp, nurseryEndAddr - zoneAddr), result, fail);
        storePtr(result, Address(temp, nurseryPosAddr - zoneAddr)); // Update position.
        subPtr(Imm32(thingSize), result); // Point result at Cell data.
        storePtr(temp, Address(result, -js::Nursery::stringHeaderSize())); // Store Zone*
    } else {
        // Otherwise, the zone is far from the nursery pointers. But the
        // nursery pos/end pointers are still near each other.
        movePtr(ImmPtr(zone->addressOfNurseryPosition()), temp);
        loadPtr(Address(temp, 0), result);
        addPtr(Imm32(totalSize), result);
        branchPtr(Assembler::Below, Address(temp, nurseryEndAddr - nurseryPosAddr), result, fail);
        storePtr(result, Address(temp, 0));
        subPtr(Imm32(thingSize), result);
        storePtr(ImmPtr(zone), Address(result, -js::Nursery::stringHeaderSize()));
    }
}

// Inlined equivalent of gc::AllocateString, jumping to fail if nursery
// allocation requested but unsuccessful.
void
MacroAssembler::allocateString(Register result, Register temp, gc::AllocKind allocKind,
                               gc::InitialHeap initialHeap, Label* fail)
{
    MOZ_ASSERT(allocKind == gc::AllocKind::STRING || allocKind == gc::AllocKind::FAT_INLINE_STRING);

    checkAllocatorState(fail);

    if (shouldNurseryAllocate(allocKind, initialHeap)) {
        MOZ_ASSERT(initialHeap == gc::DefaultHeap);
        return nurseryAllocateString(result, temp, allocKind, fail);
    }

    freeListAllocate(result, temp, allocKind, fail);
}

void
MacroAssembler::newGCString(Register result, Register temp, Label* fail, bool attemptNursery)
{
    allocateString(result, temp, js::gc::AllocKind::STRING,
                   attemptNursery ? gc::DefaultHeap : gc::TenuredHeap, fail);
}

void
MacroAssembler::newGCFatInlineString(Register result, Register temp, Label* fail, bool attemptNursery)
{
    allocateString(result, temp, js::gc::AllocKind::FAT_INLINE_STRING,
                   attemptNursery ? gc::DefaultHeap : gc::TenuredHeap, fail);
}

void
MacroAssembler::copySlotsFromTemplate(Register obj, const NativeObject* templateObj,
                                      uint32_t start, uint32_t end)
{
    uint32_t nfixed = Min(templateObj->numFixedSlotsForCompilation(), end);
    for (unsigned i = start; i < nfixed; i++) {
        // Template objects are not exposed to script and therefore immutable.
        // However, regexp template objects are sometimes used directly (when
        // the cloning is not observable), and therefore we can end up with a
        // non-zero lastIndex. Detect this case here and just substitute 0, to
        // avoid racing with the main thread updating this slot.
        Value v;
        if (templateObj->is<RegExpObject>() && i == RegExpObject::lastIndexSlot())
            v = Int32Value(0);
        else
            v = templateObj->getFixedSlot(i);
        storeValue(v, Address(obj, NativeObject::getFixedSlotOffset(i)));
    }
}

void
MacroAssembler::fillSlotsWithConstantValue(Address base, Register temp,
                                           uint32_t start, uint32_t end, const Value& v)
{
    MOZ_ASSERT(v.isUndefined() || IsUninitializedLexical(v));

    if (start >= end)
        return;

#ifdef JS_NUNBOX32
    // We only have a single spare register, so do the initialization as two
    // strided writes of the tag and body.
    Address addr = base;
    move32(Imm32(v.toNunboxPayload()), temp);
    for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtrValue))
        store32(temp, ToPayload(addr));

    addr = base;
    move32(Imm32(v.toNunboxTag()), temp);
    for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtrValue))
        store32(temp, ToType(addr));
#else
    moveValue(v, ValueOperand(temp));
    for (uint32_t i = start; i < end; ++i, base.offset += sizeof(GCPtrValue))
        storePtr(temp, base);
#endif
}

void
MacroAssembler::fillSlotsWithUndefined(Address base, Register temp, uint32_t start, uint32_t end)
{
    fillSlotsWithConstantValue(base, temp, start, end, UndefinedValue());
}

void
MacroAssembler::fillSlotsWithUninitialized(Address base, Register temp, uint32_t start, uint32_t end)
{
    fillSlotsWithConstantValue(base, temp, start, end, MagicValue(JS_UNINITIALIZED_LEXICAL));
}

static void
FindStartOfUninitializedAndUndefinedSlots(NativeObject* templateObj, uint32_t nslots,
                                          uint32_t* startOfUninitialized,
                                          uint32_t* startOfUndefined)
{
    MOZ_ASSERT(nslots == templateObj->lastProperty()->slotSpan(templateObj->getClass()));
    MOZ_ASSERT(nslots > 0);

    uint32_t first = nslots;
    for (; first != 0; --first) {
        if (templateObj->getSlot(first - 1) != UndefinedValue())
            break;
    }
    *startOfUndefined = first;

    if (first != 0 && IsUninitializedLexical(templateObj->getSlot(first - 1))) {
        for (; first != 0; --first) {
            if (!IsUninitializedLexical(templateObj->getSlot(first - 1)))
                break;
        }
        *startOfUninitialized = first;
    } else {
        *startOfUninitialized = *startOfUndefined;
    }
}

static void
AllocateObjectBufferWithInit(JSContext* cx, TypedArrayObject* obj, int32_t count)
{
    AutoUnsafeCallWithABI unsafe;

    obj->initPrivate(nullptr);

    // Negative numbers or zero will bail out to the slow path, which in turn will raise
    // an invalid argument exception or create a correct object with zero elements.
    if (count <= 0 || uint32_t(count) >= INT32_MAX / obj->bytesPerElement()) {
        obj->setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(0));
        return;
    }

    obj->setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(count));
    size_t nbytes;

    switch (obj->type()) {
#define CREATE_TYPED_ARRAY(T, N) \
      case Scalar::N: \
        MOZ_ALWAYS_TRUE(js::CalculateAllocSize<T>(count, &nbytes)); \
        break;
JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPED_ARRAY)
#undef CREATE_TYPED_ARRAY
      default:
        MOZ_CRASH("Unsupported TypedArray type");
    }

    MOZ_ASSERT((CheckedUint32(nbytes) + sizeof(Value)).isValid());

    nbytes = JS_ROUNDUP(nbytes, sizeof(Value));
    void* buf = cx->nursery().allocateBuffer(obj, nbytes);
    if (buf) {
        obj->initPrivate(buf);
        memset(buf, 0, nbytes);
    }
}

void
MacroAssembler::initTypedArraySlots(Register obj, Register temp, Register lengthReg,
                                    LiveRegisterSet liveRegs, Label* fail,
                                    TypedArrayObject* templateObj, TypedArrayLength lengthKind)
{
    MOZ_ASSERT(templateObj->hasPrivate());
    MOZ_ASSERT(!templateObj->hasBuffer());

    size_t dataSlotOffset = TypedArrayObject::dataOffset();
    size_t dataOffset = TypedArrayObject::dataOffset() + sizeof(HeapSlot);

    static_assert(TypedArrayObject::FIXED_DATA_START == TypedArrayObject::DATA_SLOT + 1,
                    "fixed inline element data assumed to begin after the data slot");

    // Initialise data elements to zero.
    int32_t length = templateObj->length();
    size_t nbytes = length * templateObj->bytesPerElement();

    if (lengthKind == TypedArrayLength::Fixed && dataOffset + nbytes <= JSObject::MAX_BYTE_SIZE) {
        MOZ_ASSERT(dataOffset + nbytes <= templateObj->tenuredSizeOfThis());

        // Store data elements inside the remaining JSObject slots.
        computeEffectiveAddress(Address(obj, dataOffset), temp);
        storePtr(temp, Address(obj, dataSlotOffset));

        // Write enough zero pointers into fixed data to zero every
        // element.  (This zeroes past the end of a byte count that's
        // not a multiple of pointer size.  That's okay, because fixed
        // data is a count of 8-byte HeapSlots (i.e. <= pointer size),
        // and we won't inline unless the desired memory fits in that
        // space.)
        static_assert(sizeof(HeapSlot) == 8, "Assumed 8 bytes alignment");

        size_t numZeroPointers = ((nbytes + 7) & ~0x7) / sizeof(char *);
        for (size_t i = 0; i < numZeroPointers; i++)
            storePtr(ImmWord(0), Address(obj, dataOffset + i * sizeof(char *)));
#ifdef DEBUG
        if (nbytes == 0)
            store8(Imm32(TypedArrayObject::ZeroLengthArrayData), Address(obj, dataSlotOffset));
#endif
    } else {
        if (lengthKind == TypedArrayLength::Fixed)
            move32(Imm32(length), lengthReg);

        // Allocate a buffer on the heap to store the data elements.
        liveRegs.addUnchecked(temp);
        liveRegs.addUnchecked(obj);
        liveRegs.addUnchecked(lengthReg);
        PushRegsInMask(liveRegs);
        setupUnalignedABICall(temp);
        loadJSContext(temp);
        passABIArg(temp);
        passABIArg(obj);
        passABIArg(lengthReg);
        callWithABI(JS_FUNC_TO_DATA_PTR(void*, AllocateObjectBufferWithInit));
        PopRegsInMask(liveRegs);

        // Fail when data elements is set to NULL.
        branchPtr(Assembler::Equal, Address(obj, dataSlotOffset), ImmWord(0), fail);
    }
}

void
MacroAssembler::initGCSlots(Register obj, Register temp, NativeObject* templateObj,
                            bool initContents)
{
    // Slots of non-array objects are required to be initialized.
    // Use the values currently in the template object.
    uint32_t nslots = templateObj->lastProperty()->slotSpan(templateObj->getClass());
    if (nslots == 0)
        return;

    uint32_t nfixed = templateObj->numUsedFixedSlots();
    uint32_t ndynamic = templateObj->numDynamicSlots();

    // Attempt to group slot writes such that we minimize the amount of
    // duplicated data we need to embed in code and load into registers. In
    // general, most template object slots will be undefined except for any
    // reserved slots. Since reserved slots come first, we split the object
    // logically into independent non-UndefinedValue writes to the head and
    // duplicated writes of UndefinedValue to the tail. For the majority of
    // objects, the "tail" will be the entire slot range.
    //
    // The template object may be a CallObject, in which case we need to
    // account for uninitialized lexical slots as well as undefined
    // slots. Unitialized lexical slots appears in CallObjects if the function
    // has parameter expressions, in which case closed over parameters have
    // TDZ. Uninitialized slots come before undefined slots in CallObjects.
    uint32_t startOfUninitialized = nslots;
    uint32_t startOfUndefined = nslots;
    FindStartOfUninitializedAndUndefinedSlots(templateObj, nslots,
                                              &startOfUninitialized, &startOfUndefined);
    MOZ_ASSERT(startOfUninitialized <= nfixed); // Reserved slots must be fixed.
    MOZ_ASSERT(startOfUndefined >= startOfUninitialized);
    MOZ_ASSERT_IF(!templateObj->is<CallObject>(), startOfUninitialized == startOfUndefined);

    // Copy over any preserved reserved slots.
    copySlotsFromTemplate(obj, templateObj, 0, startOfUninitialized);

    // Fill the rest of the fixed slots with undefined and uninitialized.
    if (initContents) {
        size_t offset = NativeObject::getFixedSlotOffset(startOfUninitialized);
        fillSlotsWithUninitialized(Address(obj, offset), temp,
                                   startOfUninitialized, Min(startOfUndefined, nfixed));

        offset = NativeObject::getFixedSlotOffset(startOfUndefined);
        fillSlotsWithUndefined(Address(obj, offset), temp,
                               startOfUndefined, nfixed);
    }

    if (ndynamic) {
        // We are short one register to do this elegantly. Borrow the obj
        // register briefly for our slots base address.
        push(obj);
        loadPtr(Address(obj, NativeObject::offsetOfSlots()), obj);

        // Fill uninitialized slots if necessary. Otherwise initialize all
        // slots to undefined.
        if (startOfUndefined > nfixed) {
            MOZ_ASSERT(startOfUninitialized != startOfUndefined);
            fillSlotsWithUninitialized(Address(obj, 0), temp, 0, startOfUndefined - nfixed);
            size_t offset = (startOfUndefined - nfixed) * sizeof(Value);
            fillSlotsWithUndefined(Address(obj, offset), temp, startOfUndefined - nfixed, ndynamic);
        } else {
            fillSlotsWithUndefined(Address(obj, 0), temp, 0, ndynamic);
        }

        pop(obj);
    }
}

void
MacroAssembler::initGCThing(Register obj, Register temp, JSObject* templateObj,
                            bool initContents, bool convertDoubleElements)
{
    // Fast initialization of an empty object returned by allocateObject().

    storePtr(ImmGCPtr(templateObj->group()), Address(obj, JSObject::offsetOfGroup()));

    if (templateObj->is<ShapedObject>())
        storePtr(ImmGCPtr(templateObj->maybeShape()), Address(obj, ShapedObject::offsetOfShape()));

    MOZ_ASSERT_IF(convertDoubleElements, templateObj->is<ArrayObject>());

    if (templateObj->isNative()) {
        NativeObject* ntemplate = &templateObj->as<NativeObject>();
        MOZ_ASSERT_IF(!ntemplate->denseElementsAreCopyOnWrite(), !ntemplate->hasDynamicElements());

        // If the object has dynamic slots, the slots member has already been
        // filled in.
        if (!ntemplate->hasDynamicSlots())
            storePtr(ImmPtr(nullptr), Address(obj, NativeObject::offsetOfSlots()));

        if (ntemplate->denseElementsAreCopyOnWrite()) {
            storePtr(ImmPtr((const Value*) ntemplate->getDenseElements()),
                     Address(obj, NativeObject::offsetOfElements()));
        } else if (ntemplate->is<ArrayObject>()) {
            int elementsOffset = NativeObject::offsetOfFixedElements();

            computeEffectiveAddress(Address(obj, elementsOffset), temp);
            storePtr(temp, Address(obj, NativeObject::offsetOfElements()));

            // Fill in the elements header.
            store32(Imm32(ntemplate->getDenseCapacity()),
                    Address(obj, elementsOffset + ObjectElements::offsetOfCapacity()));
            store32(Imm32(ntemplate->getDenseInitializedLength()),
                    Address(obj, elementsOffset + ObjectElements::offsetOfInitializedLength()));
            store32(Imm32(ntemplate->as<ArrayObject>().length()),
                    Address(obj, elementsOffset + ObjectElements::offsetOfLength()));
            store32(Imm32(convertDoubleElements
                          ? ObjectElements::CONVERT_DOUBLE_ELEMENTS
                          : 0),
                    Address(obj, elementsOffset + ObjectElements::offsetOfFlags()));
            MOZ_ASSERT(!ntemplate->hasPrivate());
        } else if (ntemplate->is<ArgumentsObject>()) {
            // The caller will initialize the reserved slots.
            MOZ_ASSERT(!initContents);
            MOZ_ASSERT(!ntemplate->hasPrivate());
            storePtr(ImmPtr(emptyObjectElements), Address(obj, NativeObject::offsetOfElements()));
        } else {
            // If the target type could be a TypedArray that maps shared memory
            // then this would need to store emptyObjectElementsShared in that case.
            MOZ_ASSERT(!ntemplate->isSharedMemory());

            storePtr(ImmPtr(emptyObjectElements), Address(obj, NativeObject::offsetOfElements()));

            initGCSlots(obj, temp, ntemplate, initContents);

            if (ntemplate->hasPrivate() && !ntemplate->is<TypedArrayObject>()) {
                uint32_t nfixed = ntemplate->numFixedSlotsForCompilation();
                Address privateSlot(obj, NativeObject::getPrivateDataOffset(nfixed));
                if (ntemplate->is<RegExpObject>()) {
                    // RegExpObject stores a GC thing (RegExpShared*) in its
                    // private slot, so we have to use ImmGCPtr.
                    RegExpObject* regexp = &ntemplate->as<RegExpObject>();
                    MOZ_ASSERT(regexp->hasShared());
                    MOZ_ASSERT(ntemplate->getPrivate() == regexp->sharedRef().get());
                    storePtr(ImmGCPtr(regexp->sharedRef().get()), privateSlot);
                } else {
                    storePtr(ImmPtr(ntemplate->getPrivate()), privateSlot);
                }
            }
        }
    } else if (templateObj->is<InlineTypedObject>()) {
        JS::AutoAssertNoGC nogc; // off-thread, so cannot GC
        size_t nbytes = templateObj->as<InlineTypedObject>().size();
        const uint8_t* memory = templateObj->as<InlineTypedObject>().inlineTypedMem(nogc);

        // Memcpy the contents of the template object to the new object.
        size_t offset = 0;
        while (nbytes) {
            uintptr_t value = *(uintptr_t*)(memory + offset);
            storePtr(ImmWord(value),
                     Address(obj, InlineTypedObject::offsetOfDataStart() + offset));
            nbytes = (nbytes < sizeof(uintptr_t)) ? 0 : nbytes - sizeof(uintptr_t);
            offset += sizeof(uintptr_t);
        }
    } else if (templateObj->is<UnboxedPlainObject>()) {
        MOZ_ASSERT(!templateObj->as<UnboxedPlainObject>().maybeExpando());
        storePtr(ImmPtr(nullptr), Address(obj, UnboxedPlainObject::offsetOfExpando()));
        if (initContents)
            initUnboxedObjectContents(obj, &templateObj->as<UnboxedPlainObject>());
    } else {
        MOZ_CRASH("Unknown object");
    }

#ifdef JS_GC_TRACE
    RegisterSet regs = RegisterSet::Volatile();
    PushRegsInMask(regs);
    regs.takeUnchecked(obj);
    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(obj);
    movePtr(ImmGCPtr(templateObj->type()), temp);
    passABIArg(temp);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::gc::TraceCreateObject));

    PopRegsInMask(RegisterSet::Volatile());
#endif
}

void
MacroAssembler::initUnboxedObjectContents(Register object, UnboxedPlainObject* templateObject)
{
    const UnboxedLayout& layout = templateObject->layoutDontCheckGeneration();

    // Initialize reference fields of the object, per UnboxedPlainObject::create.
    if (const int32_t* list = layout.traceList()) {
        while (*list != -1) {
            storePtr(ImmGCPtr(GetJitContext()->runtime->names().empty),
                     Address(object, UnboxedPlainObject::offsetOfData() + *list));
            list++;
        }
        list++;
        while (*list != -1) {
            storePtr(ImmWord(0),
                     Address(object, UnboxedPlainObject::offsetOfData() + *list));
            list++;
        }
        // Unboxed objects don't have Values to initialize.
        MOZ_ASSERT(*(list + 1) == -1);
    }
}

void
MacroAssembler::compareStrings(JSOp op, Register left, Register right, Register result,
                               Label* fail)
{
    MOZ_ASSERT(IsEqualityOp(op));

    Label done;
    Label notPointerEqual;
    // Fast path for identical strings.
    branchPtr(Assembler::NotEqual, left, right, &notPointerEqual);
    move32(Imm32(op == JSOP_EQ || op == JSOP_STRICTEQ), result);
    jump(&done);

    bind(&notPointerEqual);

    Label notAtom;
    // Optimize the equality operation to a pointer compare for two atoms.
    Imm32 nonAtomBit(JSString::NON_ATOM_BIT);
    branchTest32(Assembler::NonZero, Address(left, JSString::offsetOfFlags()), nonAtomBit, &notAtom);
    branchTest32(Assembler::NonZero, Address(right, JSString::offsetOfFlags()), nonAtomBit, &notAtom);

    cmpPtrSet(JSOpToCondition(MCompare::Compare_String, op), left, right, result);
    jump(&done);

    bind(&notAtom);
    // Strings of different length can never be equal.
    loadStringLength(left, result);
    branch32(Assembler::Equal, Address(right, JSString::offsetOfLength()), result, fail);
    move32(Imm32(op == JSOP_NE || op == JSOP_STRICTNE), result);

    bind(&done);
}

void
MacroAssembler::loadStringChars(Register str, Register dest, CharEncoding encoding)
{
    MOZ_ASSERT(str != dest);

    if (JitOptions.spectreStringMitigations) {
        if (encoding == CharEncoding::Latin1) {
            // If the string is a rope, zero the |str| register. The code below
            // depends on str->flags so this should block speculative execution.
            movePtr(ImmWord(0), dest);
            test32MovePtr(Assembler::Zero,
                          Address(str, JSString::offsetOfFlags()), Imm32(JSString::LINEAR_BIT),
                          dest, str);
        } else {
            // If we're loading TwoByte chars, there's an additional risk:
            // if the string has Latin1 chars, we could read out-of-bounds. To
            // prevent this, we check both the Linear and Latin1 bits. We don't
            // have a scratch register, so we use these flags also to block
            // speculative execution, similar to the use of 0 above.
            MOZ_ASSERT(encoding == CharEncoding::TwoByte);
            static constexpr uint32_t Mask = JSString::LINEAR_BIT | JSString::LATIN1_CHARS_BIT;
            static_assert(Mask < 1024,
                          "Mask should be a small, near-null value to ensure we "
                          "block speculative execution when it's used as string "
                          "pointer");
            move32(Imm32(Mask), dest);
            and32(Address(str, JSString::offsetOfFlags()), dest);
            cmp32MovePtr(Assembler::NotEqual, dest, Imm32(JSString::LINEAR_BIT),
                         dest, str);
        }
    }

    // Load the inline chars.
    computeEffectiveAddress(Address(str, JSInlineString::offsetOfInlineStorage()), dest);

    // If it's not an inline string, load the non-inline chars. Use a
    // conditional move to prevent speculative execution.
    test32LoadPtr(Assembler::Zero,
                  Address(str, JSString::offsetOfFlags()), Imm32(JSString::INLINE_CHARS_BIT),
                  Address(str, JSString::offsetOfNonInlineChars()), dest);
}

void
MacroAssembler::loadNonInlineStringChars(Register str, Register dest, CharEncoding encoding)
{
    MOZ_ASSERT(str != dest);

    if (JitOptions.spectreStringMitigations) {
        // If the string is a rope, has inline chars, or has a different
        // character encoding, set str to a near-null value to prevent
        // speculative execution below (when reading str->nonInlineChars).

        static constexpr uint32_t Mask =
            JSString::LINEAR_BIT |
            JSString::INLINE_CHARS_BIT |
            JSString::LATIN1_CHARS_BIT;
        static_assert(Mask < 1024,
                      "Mask should be a small, near-null value to ensure we "
                      "block speculative execution when it's used as string "
                      "pointer");

        uint32_t expectedBits = JSString::LINEAR_BIT;
        if (encoding == CharEncoding::Latin1)
            expectedBits |= JSString::LATIN1_CHARS_BIT;

        move32(Imm32(Mask), dest);
        and32(Address(str, JSString::offsetOfFlags()), dest);

        cmp32MovePtr(Assembler::NotEqual, dest, Imm32(expectedBits),
                     dest, str);
    }

    loadPtr(Address(str, JSString::offsetOfNonInlineChars()), dest);
}

void
MacroAssembler::storeNonInlineStringChars(Register chars, Register str)
{
    MOZ_ASSERT(chars != str);
    storePtr(chars, Address(str, JSString::offsetOfNonInlineChars()));
}

void
MacroAssembler::loadInlineStringCharsForStore(Register str, Register dest)
{
    computeEffectiveAddress(Address(str, JSInlineString::offsetOfInlineStorage()), dest);
}

void
MacroAssembler::loadInlineStringChars(Register str, Register dest, CharEncoding encoding)
{
    MOZ_ASSERT(str != dest);

    if (JitOptions.spectreStringMitigations) {
        // Making this Spectre-safe is a bit complicated: using
        // computeEffectiveAddress and then zeroing the output register if
        // non-inline is not sufficient: when the index is very large, it would
        // allow reading |nullptr + index|. Just fall back to loadStringChars
        // for now.
        loadStringChars(str, dest, encoding);
    } else {
        computeEffectiveAddress(Address(str, JSInlineString::offsetOfInlineStorage()), dest);
    }
}

void
MacroAssembler::loadRopeLeftChild(Register str, Register dest)
{
    MOZ_ASSERT(str != dest);

    if (JitOptions.spectreStringMitigations) {
        // Zero the output register if the input was not a rope.
        movePtr(ImmWord(0), dest);
        test32LoadPtr(Assembler::Zero,
                      Address(str, JSString::offsetOfFlags()), Imm32(JSString::LINEAR_BIT),
                      Address(str, JSRope::offsetOfLeft()), dest);
    } else {
        loadPtr(Address(str, JSRope::offsetOfLeft()), dest);
    }
}

void
MacroAssembler::storeRopeChildren(Register left, Register right, Register str)
{
    storePtr(left, Address(str, JSRope::offsetOfLeft()));
    storePtr(right, Address(str, JSRope::offsetOfRight()));
}

void
MacroAssembler::loadDependentStringBase(Register str, Register dest)
{
    MOZ_ASSERT(str != dest);

    if (JitOptions.spectreStringMitigations) {
        // If the string does not have a base-string, zero the |str| register.
        // The code below loads str->base so this should block speculative
        // execution.
        movePtr(ImmWord(0), dest);
        test32MovePtr(Assembler::Zero,
                      Address(str, JSString::offsetOfFlags()), Imm32(JSString::HAS_BASE_BIT),
                      dest, str);
    }

    loadPtr(Address(str, JSDependentString::offsetOfBase()), dest);
}

void
MacroAssembler::leaNewDependentStringBase(Register str, Register dest)
{
    MOZ_ASSERT(str != dest);

    // Spectre-safe because this is a newly allocated dependent string, thus we
    // are certain of its type and the type of its base field.
    computeEffectiveAddress(Address(str, JSDependentString::offsetOfBase()), dest);
}

void
MacroAssembler::storeDependentStringBase(Register base, Register str)
{
    storePtr(base, Address(str, JSDependentString::offsetOfBase()));
}

void
MacroAssembler::loadStringChar(Register str, Register index, Register output, Register scratch,
                               Label* fail)
{
    MOZ_ASSERT(str != output);
    MOZ_ASSERT(str != index);
    MOZ_ASSERT(index != output);
    MOZ_ASSERT(output != scratch);

    movePtr(str, output);

    // This follows JSString::getChar.
    Label notRope;
    branchIfNotRope(str, &notRope);

    loadRopeLeftChild(str, output);

    // Check if the index is contained in the leftChild.
    // Todo: Handle index in the rightChild.
    spectreBoundsCheck32(index, Address(output, JSString::offsetOfLength()), scratch, fail);

    // If the left side is another rope, give up.
    branchIfRope(output, fail);

    bind(&notRope);

    Label isLatin1, done;
    // We have to check the left/right side for ropes,
    // because a TwoByte rope might have a Latin1 child.
    branchLatin1String(output, &isLatin1);
    loadStringChars(output, scratch, CharEncoding::TwoByte);
    load16ZeroExtend(BaseIndex(scratch, index, TimesTwo), output);
    jump(&done);

    bind(&isLatin1);
    loadStringChars(output, scratch, CharEncoding::Latin1);
    load8ZeroExtend(BaseIndex(scratch, index, TimesOne), output);

    bind(&done);
}

void
MacroAssembler::loadStringIndexValue(Register str, Register dest, Label* fail)
{
    MOZ_ASSERT(str != dest);

    load32(Address(str, JSString::offsetOfFlags()), dest);

    // Does not have a cached index value.
    branchTest32(Assembler::Zero, dest, Imm32(JSString::INDEX_VALUE_BIT), fail);

    // Extract the index.
    rshift32(Imm32(JSString::INDEX_VALUE_SHIFT), dest);
}

void
MacroAssembler::typeOfObject(Register obj, Register scratch, Label* slow,
                             Label* isObject, Label* isCallable, Label* isUndefined)
{
    loadObjClassUnsafe(obj, scratch);

    // Proxies can emulate undefined and have complex isCallable behavior.
    branchTestClassIsProxy(true, scratch, slow);

    // JSFunctions are always callable.
    branchPtr(Assembler::Equal, scratch, ImmPtr(&JSFunction::class_), isCallable);

    // Objects that emulate undefined.
    Address flags(scratch, Class::offsetOfFlags());
    branchTest32(Assembler::NonZero, flags, Imm32(JSCLASS_EMULATES_UNDEFINED), isUndefined);

    // Handle classes with a call hook.
    branchPtr(Assembler::Equal, Address(scratch, offsetof(js::Class, cOps)), ImmPtr(nullptr),
              isObject);

    loadPtr(Address(scratch, offsetof(js::Class, cOps)), scratch);
    branchPtr(Assembler::Equal, Address(scratch, offsetof(js::ClassOps, call)), ImmPtr(nullptr),
              isObject);

    jump(isCallable);
}

void
MacroAssembler::loadJSContext(Register dest)
{
    JitContext* jcx = GetJitContext();
    CompileCompartment* compartment = jcx->compartment;
    if (compartment->zone()->isAtomsZone()) {
        // If we are in the atoms zone then we are generating a runtime wide
        // trampoline which can run in any zone. Load the context which is
        // currently running using cooperative scheduling in the runtime.
        // (This will need to be fixed when we have preemptive scheduling,
        // bug 1323066).
        loadPtr(AbsoluteAddress(jcx->runtime->addressOfActiveJSContext()), dest);
    } else {
        // If we are in a specific zone then the current context will be stored
        // in the containing zone group.
        loadPtr(AbsoluteAddress(compartment->zone()->addressOfJSContext()), dest);
    }
}

void
MacroAssembler::guardGroupHasUnanalyzedNewScript(Register group, Register scratch, Label* fail)
{
    Label noNewScript;
    load32(Address(group, ObjectGroup::offsetOfFlags()), scratch);
    and32(Imm32(OBJECT_FLAG_ADDENDUM_MASK), scratch);
    branch32(Assembler::NotEqual, scratch,
             Imm32(uint32_t(ObjectGroup::Addendum_NewScript) << OBJECT_FLAG_ADDENDUM_SHIFT),
             &noNewScript);

    // Guard group->newScript()->preliminaryObjects is non-nullptr.
    loadPtr(Address(group, ObjectGroup::offsetOfAddendum()), scratch);
    branchPtr(Assembler::Equal,
              Address(scratch, TypeNewScript::offsetOfPreliminaryObjects()),
              ImmWord(0), fail);

    bind(&noNewScript);
}

static void
BailoutReportOverRecursed(JSContext* cx)
{
    ReportOverRecursed(cx);
}

void
MacroAssembler::generateBailoutTail(Register scratch, Register bailoutInfo)
{
    loadJSContext(scratch);
    enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    Label baseline;

    // The return value from Bailout is tagged as:
    // - 0x0: done (enter baseline)
    // - 0x1: error (handle exception)
    // - 0x2: overrecursed
    JS_STATIC_ASSERT(BAILOUT_RETURN_OK == 0);
    JS_STATIC_ASSERT(BAILOUT_RETURN_FATAL_ERROR == 1);
    JS_STATIC_ASSERT(BAILOUT_RETURN_OVERRECURSED == 2);

    branch32(Equal, ReturnReg, Imm32(BAILOUT_RETURN_OK), &baseline);
    branch32(Equal, ReturnReg, Imm32(BAILOUT_RETURN_FATAL_ERROR), exceptionLabel());

    // Fall-through: overrecursed.
    {
        loadJSContext(ReturnReg);
        setupUnalignedABICall(scratch);
        passABIArg(ReturnReg);
        callWithABI(JS_FUNC_TO_DATA_PTR(void*, BailoutReportOverRecursed), MoveOp::GENERAL,
                    CheckUnsafeCallWithABI::DontCheckHasExitFrame);
        jump(exceptionLabel());
    }

    bind(&baseline);
    {
        // Prepare a register set for use in this case.
        AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
        MOZ_ASSERT_IF(!IsHiddenSP(getStackPointer()), !regs.has(AsRegister(getStackPointer())));
        regs.take(bailoutInfo);

        // Reset SP to the point where clobbering starts.
        loadStackPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, incomingStack)));

        Register copyCur = regs.takeAny();
        Register copyEnd = regs.takeAny();
        Register temp = regs.takeAny();

        // Copy data onto stack.
        loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, copyStackTop)), copyCur);
        loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, copyStackBottom)), copyEnd);
        {
            Label copyLoop;
            Label endOfCopy;
            bind(&copyLoop);
            branchPtr(Assembler::BelowOrEqual, copyCur, copyEnd, &endOfCopy);
            subPtr(Imm32(4), copyCur);
            subFromStackPtr(Imm32(4));
            load32(Address(copyCur, 0), temp);
            store32(temp, Address(getStackPointer(), 0));
            jump(&copyLoop);
            bind(&endOfCopy);
        }

        // Enter exit frame for the FinishBailoutToBaseline call.
        loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeFramePtr)), temp);
        load32(Address(temp, BaselineFrame::reverseOffsetOfFrameSize()), temp);
        makeFrameDescriptor(temp, JitFrame_BaselineJS, ExitFrameLayout::Size());
        push(temp);
        push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));
        // No GC things to mark on the stack, push a bare token.
        loadJSContext(scratch);
        enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

        // If monitorStub is non-null, handle resumeAddr appropriately.
        Label noMonitor;
        Label done;
        branchPtr(Assembler::Equal,
                  Address(bailoutInfo, offsetof(BaselineBailoutInfo, monitorStub)),
                  ImmPtr(nullptr),
                  &noMonitor);

        //
        // Resuming into a monitoring stub chain.
        //
        {
            // Save needed values onto stack temporarily.
            pushValue(Address(bailoutInfo, offsetof(BaselineBailoutInfo, valueR0)));
            push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeFramePtr)));
            push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));
            push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, monitorStub)));

            // Call a stub to free allocated memory and create arguments objects.
            setupUnalignedABICall(temp);
            passABIArg(bailoutInfo);
            callWithABI(JS_FUNC_TO_DATA_PTR(void*, FinishBailoutToBaseline),
                        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);
            branchTest32(Zero, ReturnReg, ReturnReg, exceptionLabel());

            // Restore values where they need to be and resume execution.
            AllocatableGeneralRegisterSet enterMonRegs(GeneralRegisterSet::All());
            enterMonRegs.take(R0);
            enterMonRegs.take(ICStubReg);
            enterMonRegs.take(BaselineFrameReg);
            enterMonRegs.takeUnchecked(ICTailCallReg);

            pop(ICStubReg);
            pop(ICTailCallReg);
            pop(BaselineFrameReg);
            popValue(R0);

            // Discard exit frame.
            addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
            push(ICTailCallReg);
#endif
            jump(Address(ICStubReg, ICStub::offsetOfStubCode()));
        }

        //
        // Resuming into main jitcode.
        //
        bind(&noMonitor);
        {
            // Save needed values onto stack temporarily.
            pushValue(Address(bailoutInfo, offsetof(BaselineBailoutInfo, valueR0)));
            pushValue(Address(bailoutInfo, offsetof(BaselineBailoutInfo, valueR1)));
            push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeFramePtr)));
            push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));

            // Call a stub to free allocated memory and create arguments objects.
            setupUnalignedABICall(temp);
            passABIArg(bailoutInfo);
            callWithABI(JS_FUNC_TO_DATA_PTR(void*, FinishBailoutToBaseline),
                        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);
            branchTest32(Zero, ReturnReg, ReturnReg, exceptionLabel());

            // Restore values where they need to be and resume execution.
            AllocatableGeneralRegisterSet enterRegs(GeneralRegisterSet::All());
            enterRegs.take(R0);
            enterRegs.take(R1);
            enterRegs.take(BaselineFrameReg);
            Register jitcodeReg = enterRegs.takeAny();

            pop(jitcodeReg);
            pop(BaselineFrameReg);
            popValue(R1);
            popValue(R0);

            // Discard exit frame.
            addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));

            jump(jitcodeReg);
        }
    }
}

void
MacroAssembler::assertRectifierFrameParentType(Register frameType)
{
#ifdef DEBUG
    {
        // Check the possible previous frame types here.
        Label checkOk;
        branch32(Assembler::Equal, frameType, Imm32(JitFrame_IonJS), &checkOk);
        branch32(Assembler::Equal, frameType, Imm32(JitFrame_BaselineStub), &checkOk);
        branch32(Assembler::Equal, frameType, Imm32(JitFrame_WasmToJSJit), &checkOk);
        branch32(Assembler::Equal, frameType, Imm32(JitFrame_CppToJSJit), &checkOk);
        assumeUnreachable("Unrecognized frame type preceding RectifierFrame.");
        bind(&checkOk);
    }
#endif
}

void
MacroAssembler::loadJitCodeRaw(Register func, Register dest)
{
    loadPtr(Address(func, JSFunction::offsetOfScript()), dest);
    loadPtr(Address(dest, JSScript::offsetOfJitCodeRaw()), dest);
}

void
MacroAssembler::loadJitCodeNoArgCheck(Register func, Register dest)
{
    loadPtr(Address(func, JSFunction::offsetOfScript()), dest);
    loadPtr(Address(dest, JSScript::offsetOfJitCodeSkipArgCheck()), dest);
}

void
MacroAssembler::loadBaselineFramePtr(Register framePtr, Register dest)
{
    if (framePtr != dest)
        movePtr(framePtr, dest);
    subPtr(Imm32(BaselineFrame::Size()), dest);
}

void
MacroAssembler::handleFailure()
{
    // Re-entry code is irrelevant because the exception will leave the
    // running function and never come back
    TrampolinePtr excTail = GetJitContext()->runtime->jitRuntime()->getExceptionTail();
    jump(excTail);
}

#ifdef DEBUG
static void
AssumeUnreachable_(const char* output) {
    MOZ_ReportAssertionFailure(output, __FILE__, __LINE__);
}
#endif

void
MacroAssembler::assumeUnreachable(const char* output)
{
#ifdef DEBUG
    if (!IsCompilingWasm()) {
        AllocatableRegisterSet regs(RegisterSet::Volatile());
        LiveRegisterSet save(regs.asLiveSet());
        PushRegsInMask(save);
        Register temp = regs.takeAnyGeneral();

        setupUnalignedABICall(temp);
        movePtr(ImmPtr(output), temp);
        passABIArg(temp);
        callWithABI(JS_FUNC_TO_DATA_PTR(void*, AssumeUnreachable_),
                    MoveOp::GENERAL,
                    CheckUnsafeCallWithABI::DontCheckOther);

        PopRegsInMask(save);
    }
#endif

    breakpoint();
}

template<typename T>
void
MacroAssembler::assertTestInt32(Condition cond, const T& value, const char* output)
{
#ifdef DEBUG
    Label ok;
    branchTestInt32(cond, value, &ok);
    assumeUnreachable(output);
    bind(&ok);
#endif
}

template void MacroAssembler::assertTestInt32(Condition, const Address&, const char*);

static void
Printf0_(const char* output)
{
    AutoUnsafeCallWithABI unsafe;

    // Use stderr instead of stdout because this is only used for debug
    // output. stderr is less likely to interfere with the program's normal
    // output, and it's always unbuffered.
    fprintf(stderr, "%s", output);
}

void
MacroAssembler::printf(const char* output)
{
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    movePtr(ImmPtr(output), temp);
    passABIArg(temp);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, Printf0_));

    PopRegsInMask(save);
}

static void
Printf1_(const char* output, uintptr_t value)
{
    AutoUnsafeCallWithABI unsafe;
    AutoEnterOOMUnsafeRegion oomUnsafe;
    js::UniqueChars line = JS_sprintf_append(nullptr, output, value);
    if (!line)
        oomUnsafe.crash("OOM at masm.printf");
    fprintf(stderr, "%s", line.get());
}

void
MacroAssembler::printf(const char* output, Register value)
{
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);

    regs.takeUnchecked(value);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    movePtr(ImmPtr(output), temp);
    passABIArg(temp);
    passABIArg(value);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, Printf1_));

    PopRegsInMask(save);
}

#ifdef JS_TRACE_LOGGING
void
MacroAssembler::tracelogStartId(Register logger, uint32_t textId, bool force)
{
    if (!force && !TraceLogTextIdEnabled(textId))
        return;

    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    regs.takeUnchecked(logger);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(logger);
    move32(Imm32(textId), temp);
    passABIArg(temp);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLogStartEventPrivate), MoveOp::GENERAL,
                CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
}

void
MacroAssembler::tracelogStartId(Register logger, Register textId)
{
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    regs.takeUnchecked(logger);
    regs.takeUnchecked(textId);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(logger);
    passABIArg(textId);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLogStartEventPrivate), MoveOp::GENERAL,
                CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
}

void
MacroAssembler::tracelogStartEvent(Register logger, Register event)
{
    void (&TraceLogFunc)(TraceLoggerThread*, const TraceLoggerEvent&) = TraceLogStartEvent;

    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    regs.takeUnchecked(logger);
    regs.takeUnchecked(event);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(logger);
    passABIArg(event);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLogFunc), MoveOp::GENERAL,
                CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
}

void
MacroAssembler::tracelogStopId(Register logger, uint32_t textId, bool force)
{
    if (!force && !TraceLogTextIdEnabled(textId))
        return;

    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    regs.takeUnchecked(logger);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(logger);
    move32(Imm32(textId), temp);
    passABIArg(temp);

    callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLogStopEventPrivate), MoveOp::GENERAL,
                CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
}

void
MacroAssembler::tracelogStopId(Register logger, Register textId)
{
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    regs.takeUnchecked(logger);
    regs.takeUnchecked(textId);

    Register temp = regs.takeAnyGeneral();

    setupUnalignedABICall(temp);
    passABIArg(logger);
    passABIArg(textId);
    callWithABI(JS_FUNC_TO_DATA_PTR(void*, TraceLogStopEventPrivate), MoveOp::GENERAL,
                CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
}
#endif

void
MacroAssembler::convertInt32ValueToDouble(const Address& address, Register scratch, Label* done)
{
    branchTestInt32(Assembler::NotEqual, address, done);
    unboxInt32(address, scratch);
    convertInt32ToDouble(scratch, ScratchDoubleReg);
    storeDouble(ScratchDoubleReg, address);
}

void
MacroAssembler::convertInt32ValueToDouble(ValueOperand val)
{
    Label done;
    branchTestInt32(Assembler::NotEqual, val, &done);
    unboxInt32(val, val.scratchReg());
    convertInt32ToDouble(val.scratchReg(), ScratchDoubleReg);
    boxDouble(ScratchDoubleReg, val, ScratchDoubleReg);
    bind(&done);
}

void
MacroAssembler::convertValueToFloatingPoint(ValueOperand value, FloatRegister output,
                                            Label* fail, MIRType outputType)
{
    Label isDouble, isInt32, isBool, isNull, done;

    {
        ScratchTagScope tag(*this, value);
        splitTagForTest(value, tag);

        branchTestDouble(Assembler::Equal, tag, &isDouble);
        branchTestInt32(Assembler::Equal, tag, &isInt32);
        branchTestBoolean(Assembler::Equal, tag, &isBool);
        branchTestNull(Assembler::Equal, tag, &isNull);
        branchTestUndefined(Assembler::NotEqual, tag, fail);
    }

    // fall-through: undefined
    loadConstantFloatingPoint(GenericNaN(), float(GenericNaN()), output, outputType);
    jump(&done);

    bind(&isNull);
    loadConstantFloatingPoint(0.0, 0.0f, output, outputType);
    jump(&done);

    bind(&isBool);
    boolValueToFloatingPoint(value, output, outputType);
    jump(&done);

    bind(&isInt32);
    int32ValueToFloatingPoint(value, output, outputType);
    jump(&done);

    bind(&isDouble);
    FloatRegister tmp = output.asDouble();
    if (outputType == MIRType::Float32 && hasMultiAlias())
        tmp = ScratchDoubleReg;

    unboxDouble(value, tmp);
    if (outputType == MIRType::Float32)
        convertDoubleToFloat32(tmp, output);

    bind(&done);
}

bool
MacroAssembler::convertValueToFloatingPoint(JSContext* cx, const Value& v, FloatRegister output,
                                            Label* fail, MIRType outputType)
{
    if (v.isNumber() || v.isString()) {
        double d;
        if (v.isNumber())
            d = v.toNumber();
        else if (!StringToNumber(cx, v.toString(), &d))
            return false;

        loadConstantFloatingPoint(d, (float)d, output, outputType);
        return true;
    }

    if (v.isBoolean()) {
        if (v.toBoolean())
            loadConstantFloatingPoint(1.0, 1.0f, output, outputType);
        else
            loadConstantFloatingPoint(0.0, 0.0f, output, outputType);
        return true;
    }

    if (v.isNull()) {
        loadConstantFloatingPoint(0.0, 0.0f, output, outputType);
        return true;
    }

    if (v.isUndefined()) {
        loadConstantFloatingPoint(GenericNaN(), float(GenericNaN()), output, outputType);
        return true;
    }

    MOZ_ASSERT(v.isObject() || v.isSymbol());
    jump(fail);
    return true;
}

bool
MacroAssembler::convertConstantOrRegisterToFloatingPoint(JSContext* cx,
                                                         const ConstantOrRegister& src,
                                                         FloatRegister output, Label* fail,
                                                         MIRType outputType)
{
    if (src.constant())
        return convertValueToFloatingPoint(cx, src.value(), output, fail, outputType);

    convertTypedOrValueToFloatingPoint(src.reg(), output, fail, outputType);
    return true;
}

void
MacroAssembler::convertTypedOrValueToFloatingPoint(TypedOrValueRegister src, FloatRegister output,
                                                   Label* fail, MIRType outputType)
{
    MOZ_ASSERT(IsFloatingPointType(outputType));

    if (src.hasValue()) {
        convertValueToFloatingPoint(src.valueReg(), output, fail, outputType);
        return;
    }

    bool outputIsDouble = outputType == MIRType::Double;
    switch (src.type()) {
      case MIRType::Null:
        loadConstantFloatingPoint(0.0, 0.0f, output, outputType);
        break;
      case MIRType::Boolean:
      case MIRType::Int32:
        convertInt32ToFloatingPoint(src.typedReg().gpr(), output, outputType);
        break;
      case MIRType::Float32:
        if (outputIsDouble) {
            convertFloat32ToDouble(src.typedReg().fpu(), output);
        } else {
            if (src.typedReg().fpu() != output)
                moveFloat32(src.typedReg().fpu(), output);
        }
        break;
      case MIRType::Double:
        if (outputIsDouble) {
            if (src.typedReg().fpu() != output)
                moveDouble(src.typedReg().fpu(), output);
        } else {
            convertDoubleToFloat32(src.typedReg().fpu(), output);
        }
        break;
      case MIRType::Object:
      case MIRType::String:
      case MIRType::Symbol:
        jump(fail);
        break;
      case MIRType::Undefined:
        loadConstantFloatingPoint(GenericNaN(), float(GenericNaN()), output, outputType);
        break;
      default:
        MOZ_CRASH("Bad MIRType");
    }
}

void
MacroAssembler::outOfLineTruncateSlow(FloatRegister src, Register dest, bool widenFloatToDouble,
                                      bool compilingWasm, wasm::BytecodeOffset callOffset)
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    if (widenFloatToDouble) {
        convertFloat32ToDouble(src, ScratchDoubleReg);
        src = ScratchDoubleReg;
    }
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    FloatRegister srcSingle;
    if (widenFloatToDouble) {
        MOZ_ASSERT(src.isSingle());
        srcSingle = src;
        src = src.asDouble();
        Push(srcSingle);
        convertFloat32ToDouble(srcSingle, src);
    }
#else
    // Also see below
    MOZ_CRASH("MacroAssembler platform hook: outOfLineTruncateSlow");
#endif

    MOZ_ASSERT(src.isDouble());

    if (compilingWasm) {
        setupWasmABICall();
        passABIArg(src, MoveOp::DOUBLE);
        callWithABI(callOffset, wasm::SymbolicAddress::ToInt32);
    } else {
        setupUnalignedABICall(dest);
        passABIArg(src, MoveOp::DOUBLE);
        callWithABI(mozilla::BitwiseCast<void*, int32_t(*)(double)>(JS::ToInt32),
                    MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);
    }
    storeCallInt32Result(dest);

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    // Nothing
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    if (widenFloatToDouble)
        Pop(srcSingle);
#else
    MOZ_CRASH("MacroAssembler platform hook: outOfLineTruncateSlow");
#endif
}

void
MacroAssembler::convertDoubleToInt(FloatRegister src, Register output, FloatRegister temp,
                                   Label* truncateFail, Label* fail,
                                   IntConversionBehavior behavior)
{
    switch (behavior) {
      case IntConversionBehavior::Normal:
      case IntConversionBehavior::NegativeZeroCheck:
        convertDoubleToInt32(src, output, fail, behavior == IntConversionBehavior::NegativeZeroCheck);
        break;
      case IntConversionBehavior::Truncate:
        branchTruncateDoubleMaybeModUint32(src, output, truncateFail ? truncateFail : fail);
        break;
      case IntConversionBehavior::ClampToUint8:
        // Clamping clobbers the input register, so use a temp.
        moveDouble(src, temp);
        clampDoubleToUint8(temp, output);
        break;
    }
}

void
MacroAssembler::convertValueToInt(ValueOperand value, MDefinition* maybeInput,
                                  Label* handleStringEntry, Label* handleStringRejoin,
                                  Label* truncateDoubleSlow,
                                  Register stringReg, FloatRegister temp, Register output,
                                  Label* fail, IntConversionBehavior behavior,
                                  IntConversionInputKind conversion)
{
    Label done, isInt32, isBool, isDouble, isNull, isString;

    bool handleStrings = (behavior == IntConversionBehavior::Truncate ||
                          behavior == IntConversionBehavior::ClampToUint8) &&
                         handleStringEntry &&
                         handleStringRejoin;

    MOZ_ASSERT_IF(handleStrings, conversion == IntConversionInputKind::Any);

    {
        ScratchTagScope tag(*this, value);
        splitTagForTest(value, tag);

        maybeBranchTestType(MIRType::Int32, maybeInput, tag, &isInt32);
        if (conversion == IntConversionInputKind::Any || conversion == IntConversionInputKind::NumbersOrBoolsOnly)
            maybeBranchTestType(MIRType::Boolean, maybeInput, tag, &isBool);
        maybeBranchTestType(MIRType::Double, maybeInput, tag, &isDouble);

        if (conversion == IntConversionInputKind::Any) {
            // If we are not truncating, we fail for anything that's not
            // null. Otherwise we might be able to handle strings and objects.
            switch (behavior) {
              case IntConversionBehavior::Normal:
              case IntConversionBehavior::NegativeZeroCheck:
                branchTestNull(Assembler::NotEqual, tag, fail);
                break;

              case IntConversionBehavior::Truncate:
              case IntConversionBehavior::ClampToUint8:
                maybeBranchTestType(MIRType::Null, maybeInput, tag, &isNull);
                if (handleStrings)
                    maybeBranchTestType(MIRType::String, maybeInput, tag, &isString);
                maybeBranchTestType(MIRType::Object, maybeInput, tag, fail);
                branchTestUndefined(Assembler::NotEqual, tag, fail);
                break;
            }
        } else {
            jump(fail);
        }
    }

    // The value is null or undefined in truncation contexts - just emit 0.
    if (isNull.used())
        bind(&isNull);
    mov(ImmWord(0), output);
    jump(&done);

    // Try converting a string into a double, then jump to the double case.
    if (handleStrings) {
        bind(&isString);
        unboxString(value, stringReg);
        jump(handleStringEntry);
    }

    // Try converting double into integer.
    if (isDouble.used() || handleStrings) {
        if (isDouble.used()) {
            bind(&isDouble);
            unboxDouble(value, temp);
        }

        if (handleStrings)
            bind(handleStringRejoin);

        convertDoubleToInt(temp, output, temp, truncateDoubleSlow, fail, behavior);
        jump(&done);
    }

    // Just unbox a bool, the result is 0 or 1.
    if (isBool.used()) {
        bind(&isBool);
        unboxBoolean(value, output);
        jump(&done);
    }

    // Integers can be unboxed.
    if (isInt32.used()) {
        bind(&isInt32);
        unboxInt32(value, output);
        if (behavior == IntConversionBehavior::ClampToUint8)
            clampIntToUint8(output);
    }

    bind(&done);
}

bool
MacroAssembler::convertValueToInt(JSContext* cx, const Value& v, Register output, Label* fail,
                                  IntConversionBehavior behavior)
{
    bool handleStrings = (behavior == IntConversionBehavior::Truncate ||
                          behavior == IntConversionBehavior::ClampToUint8);

    if (v.isNumber() || (handleStrings && v.isString())) {
        double d;
        if (v.isNumber())
            d = v.toNumber();
        else if (!StringToNumber(cx, v.toString(), &d))
            return false;

        switch (behavior) {
          case IntConversionBehavior::Normal:
          case IntConversionBehavior::NegativeZeroCheck: {
            // -0 is checked anyways if we have a constant value.
            int i;
            if (mozilla::NumberIsInt32(d, &i))
                move32(Imm32(i), output);
            else
                jump(fail);
            break;
          }
          case IntConversionBehavior::Truncate:
            move32(Imm32(ToInt32(d)), output);
            break;
          case IntConversionBehavior::ClampToUint8:
            move32(Imm32(ClampDoubleToUint8(d)), output);
            break;
        }

        return true;
    }

    if (v.isBoolean()) {
        move32(Imm32(v.toBoolean() ? 1 : 0), output);
        return true;
    }

    if (v.isNull() || v.isUndefined()) {
        move32(Imm32(0), output);
        return true;
    }

    MOZ_ASSERT(v.isObject() || v.isSymbol());

    jump(fail);
    return true;
}

bool
MacroAssembler::convertConstantOrRegisterToInt(JSContext* cx,
                                               const ConstantOrRegister& src,
                                               FloatRegister temp, Register output,
                                               Label* fail, IntConversionBehavior behavior)
{
    if (src.constant())
        return convertValueToInt(cx, src.value(), output, fail, behavior);

    convertTypedOrValueToInt(src.reg(), temp, output, fail, behavior);
    return true;
}

void
MacroAssembler::convertTypedOrValueToInt(TypedOrValueRegister src, FloatRegister temp,
                                         Register output, Label* fail,
                                         IntConversionBehavior behavior)
{
    if (src.hasValue()) {
        convertValueToInt(src.valueReg(), temp, output, fail, behavior);
        return;
    }

    switch (src.type()) {
      case MIRType::Undefined:
      case MIRType::Null:
        move32(Imm32(0), output);
        break;
      case MIRType::Boolean:
      case MIRType::Int32:
        if (src.typedReg().gpr() != output)
            move32(src.typedReg().gpr(), output);
        if (src.type() == MIRType::Int32 && behavior == IntConversionBehavior::ClampToUint8)
            clampIntToUint8(output);
        break;
      case MIRType::Double:
        convertDoubleToInt(src.typedReg().fpu(), output, temp, nullptr, fail, behavior);
        break;
      case MIRType::Float32:
        // Conversion to Double simplifies implementation at the expense of performance.
        convertFloat32ToDouble(src.typedReg().fpu(), temp);
        convertDoubleToInt(temp, output, temp, nullptr, fail, behavior);
        break;
      case MIRType::String:
      case MIRType::Symbol:
      case MIRType::Object:
        jump(fail);
        break;
      default:
        MOZ_CRASH("Bad MIRType");
    }
}

void
MacroAssembler::finish()
{
    if (failureLabel_.used()) {
        bind(&failureLabel_);
        handleFailure();
    }

    MacroAssemblerSpecific::finish();

    MOZ_RELEASE_ASSERT(size() <= MaxCodeBytesPerProcess,
                       "AssemblerBuffer should ensure we don't exceed MaxCodeBytesPerProcess");

    if (bytesNeeded() > MaxCodeBytesPerProcess)
        setOOM();
}

void
MacroAssembler::link(JitCode* code)
{
    MOZ_ASSERT(!oom());
    linkProfilerCallSites(code);
}

MacroAssembler::AutoProfilerCallInstrumentation::AutoProfilerCallInstrumentation(
    MacroAssembler& masm
    MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    if (!masm.emitProfilingInstrumentation_)
        return;

    Register reg = CallTempReg0;
    Register reg2 = CallTempReg1;
    masm.push(reg);
    masm.push(reg2);

    CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), reg);
    masm.loadJSContext(reg2);
    masm.loadPtr(Address(reg2, offsetof(JSContext, profilingActivation_)), reg2);
    masm.storePtr(reg, Address(reg2, JitActivation::offsetOfLastProfilingCallSite()));

    masm.appendProfilerCallSite(label);

    masm.pop(reg2);
    masm.pop(reg);
}

void
MacroAssembler::linkProfilerCallSites(JitCode* code)
{
    for (size_t i = 0; i < profilerCallSites_.length(); i++) {
        CodeOffset offset = profilerCallSites_[i];
        CodeLocationLabel location(code, offset);
        PatchDataWithValueCheck(location, ImmPtr(location.raw()), ImmPtr((void*)-1));
    }
}

void
MacroAssembler::alignJitStackBasedOnNArgs(Register nargs)
{
    if (JitStackValueAlignment == 1)
        return;

    // A JitFrameLayout is composed of the following:
    // [padding?] [argN] .. [arg1] [this] [[argc] [callee] [descr] [raddr]]
    //
    // We want to ensure that the |raddr| address is aligned.
    // Which implies that we want to ensure that |this| is aligned.
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");

    // Which implies that |argN| is aligned if |nargs| is even, and offset by
    // |sizeof(Value)| if |nargs| is odd.
    MOZ_ASSERT(JitStackValueAlignment == 2);

    // Thus the |padding| is offset by |sizeof(Value)| if |nargs| is even, and
    // aligned if |nargs| is odd.

    // if (nargs % 2 == 0) {
    //     if (sp % JitStackAlignment == 0)
    //         sp -= sizeof(Value);
    //     MOZ_ASSERT(sp % JitStackAlignment == JitStackAlignment - sizeof(Value));
    // } else {
    //     sp = sp & ~(JitStackAlignment - 1);
    // }
    Label odd, end;
    Label* maybeAssert = &end;
#ifdef DEBUG
    Label assert;
    maybeAssert = &assert;
#endif
    assertStackAlignment(sizeof(Value), 0);
    branchTestPtr(Assembler::NonZero, nargs, Imm32(1), &odd);
    branchTestStackPtr(Assembler::NonZero, Imm32(JitStackAlignment - 1), maybeAssert);
    subFromStackPtr(Imm32(sizeof(Value)));
#ifdef DEBUG
    bind(&assert);
#endif
    assertStackAlignment(JitStackAlignment, sizeof(Value));
    jump(&end);
    bind(&odd);
    andToStackPtr(Imm32(~(JitStackAlignment - 1)));
    bind(&end);
}

void
MacroAssembler::alignJitStackBasedOnNArgs(uint32_t nargs)
{
    if (JitStackValueAlignment == 1)
        return;

    // A JitFrameLayout is composed of the following:
    // [padding?] [argN] .. [arg1] [this] [[argc] [callee] [descr] [raddr]]
    //
    // We want to ensure that the |raddr| address is aligned.
    // Which implies that we want to ensure that |this| is aligned.
    static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
      "No need to consider the JitFrameLayout for aligning the stack");

    // Which implies that |argN| is aligned if |nargs| is even, and offset by
    // |sizeof(Value)| if |nargs| is odd.
    MOZ_ASSERT(JitStackValueAlignment == 2);

    // Thus the |padding| is offset by |sizeof(Value)| if |nargs| is even, and
    // aligned if |nargs| is odd.

    assertStackAlignment(sizeof(Value), 0);
    if (nargs % 2 == 0) {
        Label end;
        branchTestStackPtr(Assembler::NonZero, Imm32(JitStackAlignment - 1), &end);
        subFromStackPtr(Imm32(sizeof(Value)));
        bind(&end);
        assertStackAlignment(JitStackAlignment, sizeof(Value));
    } else {
        andToStackPtr(Imm32(~(JitStackAlignment - 1)));
    }
}

// ===============================================================

MacroAssembler::MacroAssembler(JSContext* cx, IonScript* ion,
                               JSScript* script, jsbytecode* pc)
  : framePushed_(0),
#ifdef DEBUG
    inCall_(false),
#endif
    emitProfilingInstrumentation_(false)
{
    constructRoot(cx);
    jitContext_.emplace(cx, (js::jit::TempAllocator*)nullptr);
    alloc_.emplace(cx);
    moveResolver_.setAllocator(*jitContext_->temp);
#if defined(JS_CODEGEN_ARM)
    initWithAllocator();
    m_buffer.id = GetJitContext()->getNextAssemblerId();
#elif defined(JS_CODEGEN_ARM64)
    initWithAllocator();
    armbuffer_.id = GetJitContext()->getNextAssemblerId();
#endif
    if (ion) {
        setFramePushed(ion->frameSize());
        if (pc && cx->runtime()->geckoProfiler().enabled())
            enableProfilingInstrumentation();
    }
}

bool
MacroAssembler::icBuildOOLFakeExitFrame(void* fakeReturnAddr, AutoSaveLiveRegisters& save)
{
    return buildOOLFakeExitFrame(fakeReturnAddr);
}

#ifndef JS_CODEGEN_ARM64
void
MacroAssembler::subFromStackPtr(Register reg)
{
    subPtr(reg, getStackPointer());
}
#endif // JS_CODEGEN_ARM64

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::PushRegsInMask(LiveGeneralRegisterSet set)
{
    PushRegsInMask(LiveRegisterSet(set.set(), FloatRegisterSet()));
}

void
MacroAssembler::PopRegsInMask(LiveRegisterSet set)
{
    PopRegsInMaskIgnore(set, LiveRegisterSet());
}

void
MacroAssembler::PopRegsInMask(LiveGeneralRegisterSet set)
{
    PopRegsInMask(LiveRegisterSet(set.set(), FloatRegisterSet()));
}

void
MacroAssembler::Push(jsid id, Register scratchReg)
{
    if (JSID_IS_GCTHING(id)) {
        // If we're pushing a gcthing, then we can't just push the tagged jsid
        // value since the GC won't have any idea that the push instruction
        // carries a reference to a gcthing.  Need to unpack the pointer,
        // push it using ImmGCPtr, and then rematerialize the id at runtime.

        if (JSID_IS_STRING(id)) {
            JSString* str = JSID_TO_STRING(id);
            MOZ_ASSERT(((size_t)str & JSID_TYPE_MASK) == 0);
            MOZ_ASSERT(JSID_TYPE_STRING == 0x0);
            Push(ImmGCPtr(str));
        } else {
            MOZ_ASSERT(JSID_IS_SYMBOL(id));
            JS::Symbol* sym = JSID_TO_SYMBOL(id);
            movePtr(ImmGCPtr(sym), scratchReg);
            orPtr(Imm32(JSID_TYPE_SYMBOL), scratchReg);
            Push(scratchReg);
        }
    } else {
        Push(ImmWord(JSID_BITS(id)));
    }
}

void
MacroAssembler::Push(TypedOrValueRegister v)
{
    if (v.hasValue()) {
        Push(v.valueReg());
    } else if (IsFloatingPointType(v.type())) {
        FloatRegister reg = v.typedReg().fpu();
        if (v.type() == MIRType::Float32) {
            convertFloat32ToDouble(reg, ScratchDoubleReg);
            reg = ScratchDoubleReg;
        }
        Push(reg);
    } else {
        Push(ValueTypeFromMIRType(v.type()), v.typedReg().gpr());
    }
}

void
MacroAssembler::Push(const ConstantOrRegister& v)
{
    if (v.constant())
        Push(v.value());
    else
        Push(v.reg());
}

void
MacroAssembler::Push(const ValueOperand& val)
{
    pushValue(val);
    framePushed_ += sizeof(Value);
}

void
MacroAssembler::Push(const Value& val)
{
    pushValue(val);
    framePushed_ += sizeof(Value);
}

void
MacroAssembler::Push(JSValueType type, Register reg)
{
    pushValue(type, reg);
    framePushed_ += sizeof(Value);
}

void
MacroAssembler::PushValue(const Address& addr)
{
    MOZ_ASSERT(addr.base != getStackPointer());
    pushValue(addr);
    framePushed_ += sizeof(Value);
}

void
MacroAssembler::PushEmptyRooted(VMFunction::RootType rootType)
{
    switch (rootType) {
      case VMFunction::RootNone:
        MOZ_CRASH("Handle must have root type");
      case VMFunction::RootObject:
      case VMFunction::RootString:
      case VMFunction::RootFunction:
      case VMFunction::RootCell:
        Push(ImmPtr(nullptr));
        break;
      case VMFunction::RootValue:
        Push(UndefinedValue());
        break;
      case VMFunction::RootId:
        Push(ImmWord(JSID_BITS(JSID_VOID)));
        break;
    }
}

void
MacroAssembler::popRooted(VMFunction::RootType rootType, Register cellReg,
                          const ValueOperand& valueReg)
{
    switch (rootType) {
      case VMFunction::RootNone:
        MOZ_CRASH("Handle must have root type");
      case VMFunction::RootObject:
      case VMFunction::RootString:
      case VMFunction::RootFunction:
      case VMFunction::RootCell:
      case VMFunction::RootId:
        Pop(cellReg);
        break;
      case VMFunction::RootValue:
        Pop(valueReg);
        break;
    }
}

void
MacroAssembler::adjustStack(int amount)
{
    if (amount > 0)
        freeStack(amount);
    else if (amount < 0)
        reserveStack(-amount);
}

void
MacroAssembler::freeStack(uint32_t amount)
{
    MOZ_ASSERT(amount <= framePushed_);
    if (amount)
        addToStackPtr(Imm32(amount));
    framePushed_ -= amount;
}

void
MacroAssembler::freeStack(Register amount)
{
    addToStackPtr(amount);
}

// ===============================================================
// ABI function calls.

void
MacroAssembler::setupABICall()
{
#ifdef DEBUG
    MOZ_ASSERT(!inCall_);
    inCall_ = true;
#endif

#ifdef JS_SIMULATOR
    signature_ = 0;
#endif

    // Reinitialize the ABIArg generator.
    abiArgs_ = ABIArgGenerator();

#if defined(JS_CODEGEN_ARM)
    // On ARM, we need to know what ABI we are using, either in the
    // simulator, or based on the configure flags.
#if defined(JS_SIMULATOR_ARM)
    abiArgs_.setUseHardFp(UseHardFpABI());
#elif defined(JS_CODEGEN_ARM_HARDFP)
    abiArgs_.setUseHardFp(true);
#else
    abiArgs_.setUseHardFp(false);
#endif
#endif

#if defined(JS_CODEGEN_MIPS32)
    // On MIPS, the system ABI use general registers pairs to encode double
    // arguments, after one or 2 integer-like arguments. Unfortunately, the
    // Lowering phase is not capable to express it at the moment. So we enforce
    // the system ABI here.
    abiArgs_.enforceO32ABI();
#endif
}

void
MacroAssembler::setupWasmABICall()
{
    MOZ_ASSERT(IsCompilingWasm(), "non-wasm should use setupAlignedABICall");
    setupABICall();

#if defined(JS_CODEGEN_ARM)
    // The builtin thunk does the FP -> GPR moving on soft-FP, so
    // use hard fp unconditionally.
    abiArgs_.setUseHardFp(true);
#endif
    dynamicAlignment_ = false;
}

void
MacroAssembler::setupAlignedABICall()
{
    MOZ_ASSERT(!IsCompilingWasm(), "wasm should use setupWasmABICall");
    setupABICall();
    dynamicAlignment_ = false;

#if defined(JS_CODEGEN_ARM64)
    MOZ_CRASH("Not supported on arm64");
#endif
}

void
MacroAssembler::passABIArg(const MoveOperand& from, MoveOp::Type type)
{
    MOZ_ASSERT(inCall_);
    appendSignatureType(type);

    ABIArg arg;
    switch (type) {
      case MoveOp::FLOAT32:
        arg = abiArgs_.next(MIRType::Float32);
        break;
      case MoveOp::DOUBLE:
        arg = abiArgs_.next(MIRType::Double);
        break;
      case MoveOp::GENERAL:
        arg = abiArgs_.next(MIRType::Pointer);
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    MoveOperand to(*this, arg);
    if (from == to)
        return;

    if (oom())
        return;
    propagateOOM(moveResolver_.addMove(from, to, type));
}

void
MacroAssembler::callWithABINoProfiler(void* fun, MoveOp::Type result, CheckUnsafeCallWithABI check)
{
    appendSignatureType(result);
#ifdef JS_SIMULATOR
    fun = Simulator::RedirectNativeFunction(fun, signature());
#endif

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);

#ifdef DEBUG
    if (check == CheckUnsafeCallWithABI::Check) {
        push(ReturnReg);
        loadJSContext(ReturnReg);
        Address flagAddr(ReturnReg, JSContext::offsetOfInUnsafeCallWithABI());
        store32(Imm32(1), flagAddr);
        pop(ReturnReg);
    }
#endif

    call(ImmPtr(fun));

    callWithABIPost(stackAdjust, result);

#ifdef DEBUG
    if (check == CheckUnsafeCallWithABI::Check) {
        Label ok;
        push(ReturnReg);
        loadJSContext(ReturnReg);
        Address flagAddr(ReturnReg, JSContext::offsetOfInUnsafeCallWithABI());
        branch32(Assembler::Equal, flagAddr, Imm32(0), &ok);
        assumeUnreachable("callWithABI: callee did not use AutoInUnsafeCallWithABI");
        bind(&ok);
        pop(ReturnReg);
    }
#endif
}

void
MacroAssembler::callWithABI(wasm::BytecodeOffset bytecode, wasm::SymbolicAddress imm,
                            MoveOp::Type result)
{
    MOZ_ASSERT(wasm::NeedsBuiltinThunk(imm));

    // We clobber WasmTlsReg below in the loadWasmTlsRegFromFrame(), but Ion
    // assumes it is non-volatile, so preserve it manually.
    Push(WasmTlsReg);

    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust, /* callFromWasm = */ true);

    // The TLS register is used in builtin thunks and must be set, by ABI:
    // reload it after passing arguments, which might have used it at spill
    // points when placing arguments.
    loadWasmTlsRegFromFrame();

    call(wasm::CallSiteDesc(bytecode.offset, wasm::CallSite::Symbolic), imm);
    callWithABIPost(stackAdjust, result, /* callFromWasm = */ true);

    Pop(WasmTlsReg);
}

// ===============================================================
// Exit frame footer.

void
MacroAssembler::linkExitFrame(Register cxreg, Register scratch)
{
    loadPtr(Address(cxreg, JSContext::offsetOfActivation()), scratch);
    storeStackPtr(Address(scratch, JitActivation::offsetOfPackedExitFP()));
}

// ===============================================================
// Branch functions

void
MacroAssembler::branchIfNotInterpretedConstructor(Register fun, Register scratch, Label* label)
{
    // 16-bit loads are slow and unaligned 32-bit loads may be too so
    // perform an aligned 32-bit load and adjust the bitmask accordingly.
    MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
    MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);

    // First, ensure it's a scripted function.
    load32(Address(fun, JSFunction::offsetOfNargs()), scratch);
    int32_t bits = IMM32_16ADJ(JSFunction::INTERPRETED);
    branchTest32(Assembler::Zero, scratch, Imm32(bits), label);

    // Check if the CONSTRUCTOR bit is set.
    bits = IMM32_16ADJ(JSFunction::CONSTRUCTOR);
    branchTest32(Assembler::Zero, scratch, Imm32(bits), label);
}

void
MacroAssembler::branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj,
                                                       const Address& group, Register scratch,
                                                       Label* label)
{
    // Note: obj and scratch registers may alias.
    MOZ_ASSERT(group.base != scratch);
    MOZ_ASSERT(group.base != obj);

    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    branchPtr(cond, group, scratch, label);
}

void
MacroAssembler::branchTestObjGroup(Condition cond, Register obj, const Address& group,
                                   Register scratch, Register spectreRegToZero, Label* label)
{
    // Note: obj and scratch registers may alias.
    MOZ_ASSERT(group.base != scratch);
    MOZ_ASSERT(group.base != obj);
    MOZ_ASSERT(scratch != spectreRegToZero);

    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    branchPtr(cond, group, scratch, label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreZeroRegister(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjCompartment(Condition cond, Register obj, const Address& compartment,
                                         Register scratch, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    loadPtr(Address(scratch, ObjectGroup::offsetOfCompartment()), scratch);
    branchPtr(cond, compartment, scratch, label);
}

void
MacroAssembler::branchTestObjCompartment(Condition cond, Register obj,
                                         const JSCompartment* compartment, Register scratch,
                                         Label* label)
{
    MOZ_ASSERT(obj != scratch);
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    loadPtr(Address(scratch, ObjectGroup::offsetOfCompartment()), scratch);
    branchPtr(cond, scratch, ImmPtr(compartment), label);
}

void
MacroAssembler::branchIfObjGroupHasNoAddendum(Register obj, Register scratch, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    branchPtr(Assembler::Equal,
              Address(scratch, ObjectGroup::offsetOfAddendum()),
              ImmWord(0),
              label);
}

void
MacroAssembler::branchIfPretenuredGroup(const ObjectGroup* group, Register scratch, Label* label)
{
    movePtr(ImmGCPtr(group), scratch);
    branchTest32(Assembler::NonZero, Address(scratch, ObjectGroup::offsetOfFlags()),
                 Imm32(OBJECT_FLAG_PRE_TENURE), label);
}

void
MacroAssembler::branchIfNonNativeObj(Register obj, Register scratch, Label* label)
{
    loadObjClassUnsafe(obj, scratch);
    branchTest32(Assembler::NonZero, Address(scratch, Class::offsetOfFlags()),
                 Imm32(Class::NON_NATIVE), label);
}

void
MacroAssembler::branchIfInlineTypedObject(Register obj, Register scratch, Label* label)
{
    loadObjClassUnsafe(obj, scratch);
    branchPtr(Assembler::Equal, scratch, ImmPtr(&InlineOpaqueTypedObject::class_), label);
    branchPtr(Assembler::Equal, scratch, ImmPtr(&InlineTransparentTypedObject::class_), label);
}

void
MacroAssembler::branchIfNotSimdObject(Register obj, Register scratch, SimdType simdType,
                                      Label* label)
{
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);

    // Guard that the object has the same representation as the one produced for
    // SIMD value-type.
    Address clasp(scratch, ObjectGroup::offsetOfClasp());
    static_assert(!SimdTypeDescr::Opaque, "SIMD objects are transparent");
    branchPtr(Assembler::NotEqual, clasp, ImmPtr(&InlineTransparentTypedObject::class_),
              label);

    // obj->type()->typeDescr()
    // The previous class pointer comparison implies that the addendumKind is
    // Addendum_TypeDescr.
    loadPtr(Address(scratch, ObjectGroup::offsetOfAddendum()), scratch);

    // Check for the /Kind/ reserved slot of the TypeDescr.  This is an Int32
    // Value which is equivalent to the object class check.
    static_assert(JS_DESCR_SLOT_KIND < NativeObject::MAX_FIXED_SLOTS, "Load from fixed slots");
    Address typeDescrKind(scratch, NativeObject::getFixedSlotOffset(JS_DESCR_SLOT_KIND));
    assertTestInt32(Assembler::Equal, typeDescrKind,
      "MOZ_ASSERT(obj->type()->typeDescr()->getReservedSlot(JS_DESCR_SLOT_KIND).isInt32())");
    branch32(Assembler::NotEqual, ToPayload(typeDescrKind), Imm32(js::type::Simd), label);

    // Check if the SimdTypeDescr /Type/ matches the specialization of this
    // MSimdUnbox instruction.
    static_assert(JS_DESCR_SLOT_TYPE < NativeObject::MAX_FIXED_SLOTS, "Load from fixed slots");
    Address typeDescrType(scratch, NativeObject::getFixedSlotOffset(JS_DESCR_SLOT_TYPE));
    assertTestInt32(Assembler::Equal, typeDescrType,
      "MOZ_ASSERT(obj->type()->typeDescr()->getReservedSlot(JS_DESCR_SLOT_TYPE).isInt32())");
    branch32(Assembler::NotEqual, ToPayload(typeDescrType), Imm32(int32_t(simdType)), label);
}

void
MacroAssembler::copyObjGroupNoPreBarrier(Register sourceObj, Register destObj, Register scratch)
{
    loadPtr(Address(sourceObj, JSObject::offsetOfGroup()), scratch);
    storePtr(scratch, Address(destObj, JSObject::offsetOfGroup()));
}

void
MacroAssembler::loadTypedObjectDescr(Register obj, Register dest)
{
    loadPtr(Address(obj, JSObject::offsetOfGroup()), dest);
    loadPtr(Address(dest, ObjectGroup::offsetOfAddendum()), dest);
}

void
MacroAssembler::loadTypedObjectLength(Register obj, Register dest)
{
    loadTypedObjectDescr(obj, dest);
    unboxInt32(Address(dest, ArrayTypeDescr::offsetOfLength()), dest);
}

void
MacroAssembler::maybeBranchTestType(MIRType type, MDefinition* maybeDef, Register tag, Label* label)
{
    if (!maybeDef || maybeDef->mightBeType(type)) {
        switch (type) {
          case MIRType::Null:
            branchTestNull(Equal, tag, label);
            break;
          case MIRType::Boolean:
            branchTestBoolean(Equal, tag, label);
            break;
          case MIRType::Int32:
            branchTestInt32(Equal, tag, label);
            break;
          case MIRType::Double:
            branchTestDouble(Equal, tag, label);
            break;
          case MIRType::String:
            branchTestString(Equal, tag, label);
            break;
          case MIRType::Symbol:
            branchTestSymbol(Equal, tag, label);
            break;
          case MIRType::Object:
            branchTestObject(Equal, tag, label);
            break;
          default:
            MOZ_CRASH("Unsupported type");
        }
    }
}

void
MacroAssembler::wasmTrap(wasm::Trap trap, wasm::BytecodeOffset bytecodeOffset)
{
    append(trap, wasm::TrapSite(wasmTrapInstruction().offset(), bytecodeOffset));
}

void
MacroAssembler::wasmCallImport(const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee)
{
    // Load the callee, before the caller's registers are clobbered.
    uint32_t globalDataOffset = callee.importGlobalDataOffset();
    loadWasmGlobalPtr(globalDataOffset + offsetof(wasm::FuncImportTls, code), ABINonArgReg0);

#ifndef JS_CODEGEN_NONE
    static_assert(ABINonArgReg0 != WasmTlsReg, "by constraint");
#endif

    // Switch to the callee's TLS and pinned registers and make the call.
    loadWasmGlobalPtr(globalDataOffset + offsetof(wasm::FuncImportTls, tls), WasmTlsReg);
    loadWasmPinnedRegsFromTls();

    call(desc, ABINonArgReg0);
}

void
MacroAssembler::wasmCallBuiltinInstanceMethod(const wasm::CallSiteDesc& desc,
                                              const ABIArg& instanceArg,
                                              wasm::SymbolicAddress builtin)
{
    MOZ_ASSERT(instanceArg != ABIArg());

    if (instanceArg.kind() == ABIArg::GPR) {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, instance)), instanceArg.gpr());
    } else if (instanceArg.kind() == ABIArg::Stack) {
        // Safe to use ABINonArgReg0 since it's the last thing before the call.
        Register scratch = ABINonArgReg0;
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, instance)), scratch);
        storePtr(scratch, Address(getStackPointer(), instanceArg.offsetFromArgBase()));
    } else {
        MOZ_CRASH("Unknown abi passing style for pointer");
    }

    call(desc, builtin);
}

void
MacroAssembler::wasmCallIndirect(const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
                                 bool needsBoundsCheck)
{
    Register scratch = WasmTableCallScratchReg;
    Register index = WasmTableCallIndexReg;

    if (callee.which() == wasm::CalleeDesc::AsmJSTable) {
        // asm.js tables require no signature check, have had their index masked
        // into range and thus need no bounds check and cannot be external.
        loadWasmGlobalPtr(callee.tableBaseGlobalDataOffset(), scratch);
        loadPtr(BaseIndex(scratch, index, ScalePointer), scratch);
        call(desc, scratch);
        return;
    }

    MOZ_ASSERT(callee.which() == wasm::CalleeDesc::WasmTable);

    // Write the sig-id into the ABI sig-id register.
    wasm::SigIdDesc sigId = callee.wasmTableSigId();
    switch (sigId.kind()) {
      case wasm::SigIdDesc::Kind::Global:
        loadWasmGlobalPtr(sigId.globalDataOffset(), WasmTableCallSigReg);
        break;
      case wasm::SigIdDesc::Kind::Immediate:
        move32(Imm32(sigId.immediate()), WasmTableCallSigReg);
        break;
      case wasm::SigIdDesc::Kind::None:
        break;
    }

    wasm::BytecodeOffset trapOffset(desc.lineOrBytecode());

    // WebAssembly throws if the index is out-of-bounds.
    if (needsBoundsCheck) {
        loadWasmGlobalPtr(callee.tableLengthGlobalDataOffset(), scratch);

        wasm::OldTrapDesc oobTrap(trapOffset, wasm::Trap::OutOfBounds, framePushed());
        branch32(Assembler::Condition::AboveOrEqual, index, scratch, oobTrap);
    }

    // Load the base pointer of the table.
    loadWasmGlobalPtr(callee.tableBaseGlobalDataOffset(), scratch);

    // Load the callee from the table.
    if (callee.wasmTableIsExternal()) {
        static_assert(sizeof(wasm::ExternalTableElem) == 8 || sizeof(wasm::ExternalTableElem) == 16,
                      "elements of external tables are two words");
        if (sizeof(wasm::ExternalTableElem) == 8) {
            computeEffectiveAddress(BaseIndex(scratch, index, TimesEight), scratch);
        } else {
            lshift32(Imm32(4), index);
            addPtr(index, scratch);
        }

        loadPtr(Address(scratch, offsetof(wasm::ExternalTableElem, tls)), WasmTlsReg);

        Label nonNull;
        branchTest32(Assembler::NonZero, WasmTlsReg, WasmTlsReg, &nonNull);
        wasmTrap(wasm::Trap::IndirectCallToNull, trapOffset);
        bind(&nonNull);

        loadWasmPinnedRegsFromTls();

        loadPtr(Address(scratch, offsetof(wasm::ExternalTableElem, code)), scratch);
    } else {
        loadPtr(BaseIndex(scratch, index, ScalePointer), scratch);

        Label nonNull;
        branchTest32(Assembler::NonZero, scratch, scratch, &nonNull);
        wasmTrap(wasm::Trap::IndirectCallToNull, trapOffset);
        bind(&nonNull);
    }

    call(desc, scratch);
}

void
MacroAssembler::wasmEmitOldTrapOutOfLineCode()
{
    for (const wasm::OldTrapSite& site : oldTrapSites()) {
        // Trap out-of-line codes are created for two kinds of trap sites:
        //  - jumps, which are bound directly to the trap out-of-line path
        //  - memory accesses, which can fault and then have control transferred
        //    to the out-of-line path directly via signal handler setting pc
        switch (site.kind) {
          case wasm::OldTrapSite::Jump: {
            RepatchLabel jump;
            jump.use(site.codeOffset);
            bind(&jump);
            break;
          }
          case wasm::OldTrapSite::MemoryAccess: {
            append(wasm::MemoryAccess(site.codeOffset, currentOffset()));
            break;
          }
        }

        MOZ_ASSERT(site.trap != wasm::Trap::IndirectCallBadSig);

        // Inherit the frame depth of the trap site. This value is captured
        // by the wasm::CallSite to allow unwinding this frame.
        setFramePushed(site.framePushed);

        // Align the stack for a nullary call.
        size_t alreadyPushed = sizeof(wasm::Frame) + framePushed();
        size_t toPush = ABIArgGenerator().stackBytesConsumedSoFar();
        if (size_t dec = StackDecrementForCall(ABIStackAlignment, alreadyPushed, toPush))
            reserveStack(dec);

        // To call the trap handler function, we must have the WasmTlsReg
        // filled since this is the normal calling ABI. To avoid requiring
        // every trapping operation to have the TLS register filled for the
        // rare case that it takes a trap, we restore it from the frame on
        // the out-of-line path. However, there are millions of out-of-line
        // paths (viz. for loads/stores), so the load is factored out into
        // the shared FarJumpIsland generated by patchCallSites.

        // Call the trap's exit, using the bytecode offset of the trap site.
        // Note that this code is inside the same CodeRange::Function as the
        // trap site so it's as if the trapping instruction called the
        // trap-handling function. The frame iterator knows to skip the trap
        // exit's frame so that unwinding begins at the frame and offset of
        // the trapping instruction.
        wasm::CallSiteDesc desc(site.offset, wasm::CallSiteDesc::OldTrapExit);
        call(desc, site.trap);

#ifdef DEBUG
        // Traps do not return, so no need to freeStack().
        breakpoint();
#endif
    }

    // Ensure that the return address of the last emitted call above is always
    // within this function's CodeRange which is necessary for the stack
    // iterator to find the right CodeRange while walking the stack.
    breakpoint();

    oldTrapSites().clear();
}

void
MacroAssembler::emitPreBarrierFastPath(JSRuntime* rt, MIRType type, Register temp1, Register temp2,
                                       Register temp3, Label* noBarrier)
{
    MOZ_ASSERT(temp1 != PreBarrierReg);
    MOZ_ASSERT(temp2 != PreBarrierReg);
    MOZ_ASSERT(temp3 != PreBarrierReg);

    // Load the GC thing in temp1.
    if (type == MIRType::Value) {
        unboxGCThingForPreBarrierTrampoline(Address(PreBarrierReg, 0), temp1);
    } else {
        MOZ_ASSERT(type == MIRType::Object ||
                   type == MIRType::String ||
                   type == MIRType::Shape ||
                   type == MIRType::ObjectGroup);
        loadPtr(Address(PreBarrierReg, 0), temp1);
    }

#ifdef DEBUG
    // The caller should have checked for null pointers.
    Label nonZero;
    branchTestPtr(Assembler::NonZero, temp1, temp1, &nonZero);
    assumeUnreachable("JIT pre-barrier: unexpected nullptr");
    bind(&nonZero);
#endif

    // Load the chunk address in temp2.
    movePtr(ImmWord(~gc::ChunkMask), temp2);
    andPtr(temp1, temp2);

    // If the GC thing is in the nursery, we don't need to barrier it.
    if (type == MIRType::Value || type == MIRType::Object || type == MIRType::String) {
        branch32(Assembler::Equal, Address(temp2, gc::ChunkLocationOffset),
                 Imm32(int32_t(gc::ChunkLocation::Nursery)), noBarrier);
    } else {
#ifdef DEBUG
        Label isTenured;
        branch32(Assembler::NotEqual, Address(temp2, gc::ChunkLocationOffset),
                 Imm32(int32_t(gc::ChunkLocation::Nursery)), &isTenured);
        assumeUnreachable("JIT pre-barrier: unexpected nursery pointer");
        bind(&isTenured);
#endif
    }

    // If it's a permanent atom or symbol from a parent runtime we don't
    // need to barrier it.
    if (type == MIRType::Value || type == MIRType::String) {
        branchPtr(Assembler::NotEqual, Address(temp2, gc::ChunkRuntimeOffset),
                  ImmPtr(rt), noBarrier);
    } else {
#ifdef DEBUG
        Label thisRuntime;
        branchPtr(Assembler::Equal, Address(temp2, gc::ChunkRuntimeOffset),
                  ImmPtr(rt), &thisRuntime);
        assumeUnreachable("JIT pre-barrier: unexpected runtime");
        bind(&thisRuntime);
#endif
    }

    // Determine the bit index and store in temp1.
    //
    // bit = (addr & js::gc::ChunkMask) / js::gc::CellBytesPerMarkBit +
    //        static_cast<uint32_t>(colorBit);
    static_assert(gc::CellBytesPerMarkBit == 8,
                  "Calculation below relies on this");
    static_assert(size_t(gc::ColorBit::BlackBit) == 0,
                  "Calculation below relies on this");
    andPtr(Imm32(gc::ChunkMask), temp1);
    rshiftPtr(Imm32(3), temp1);

    static const size_t nbits = sizeof(uintptr_t) * CHAR_BIT;
    static_assert(nbits == JS_BITS_PER_WORD,
                  "Calculation below relies on this");

    // Load the bitmap word in temp2.
    //
    // word = chunk.bitmap[bit / nbits];
    movePtr(temp1, temp3);
#if JS_BITS_PER_WORD == 64
    rshiftPtr(Imm32(6), temp1);
    loadPtr(BaseIndex(temp2, temp1, TimesEight, gc::ChunkMarkBitmapOffset), temp2);
#else
    rshiftPtr(Imm32(5), temp1);
    loadPtr(BaseIndex(temp2, temp1, TimesFour, gc::ChunkMarkBitmapOffset), temp2);
#endif

    // Load the mask in temp1.
    //
    // mask = uintptr_t(1) << (bit % nbits);
    andPtr(Imm32(nbits - 1), temp3);
    move32(Imm32(1), temp1);
#ifdef JS_CODEGEN_X64
    MOZ_ASSERT(temp3 == rcx);
    shlq_cl(temp1);
#elif JS_CODEGEN_X86
    MOZ_ASSERT(temp3 == ecx);
    shll_cl(temp1);
#elif JS_CODEGEN_ARM
    ma_lsl(temp3, temp1, temp1);
#elif JS_CODEGEN_ARM64
    Lsl(ARMRegister(temp1, 64), ARMRegister(temp1, 64), ARMRegister(temp3, 64));
#elif JS_CODEGEN_MIPS32
    ma_sll(temp1, temp1, temp3);
#elif JS_CODEGEN_MIPS64
    ma_dsll(temp1, temp1, temp3);
#elif JS_CODEGEN_NONE
    MOZ_CRASH();
#else
# error "Unknown architecture"
#endif

    // No barrier is needed if the bit is set, |word & mask != 0|.
    branchTestPtr(Assembler::NonZero, temp2, temp1, noBarrier);
}

// ========================================================================
// Spectre Mitigations.

void
MacroAssembler::spectreMaskIndex(Register index, Register length, Register output)
{
    MOZ_ASSERT(JitOptions.spectreIndexMasking);
    MOZ_ASSERT(length != output);
    MOZ_ASSERT(index != output);

    move32(Imm32(0), output);
    cmp32Move32(Assembler::Below, index, length, index, output);
}

void
MacroAssembler::spectreMaskIndex(Register index, const Address& length, Register output)
{
    MOZ_ASSERT(JitOptions.spectreIndexMasking);
    MOZ_ASSERT(index != length.base);
    MOZ_ASSERT(length.base != output);
    MOZ_ASSERT(index != output);

    move32(Imm32(0), output);
    cmp32Move32(Assembler::Below, index, length, index, output);
}

void
MacroAssembler::boundsCheck32PowerOfTwo(Register index, uint32_t length, Label* failure)
{
    MOZ_ASSERT(mozilla::IsPowerOfTwo(length));
    branch32(Assembler::AboveOrEqual, index, Imm32(length), failure);

    // Note: it's fine to clobber the input register, as this is a no-op: it
    // only affects speculative execution.
    if (JitOptions.spectreIndexMasking)
        and32(Imm32(length - 1), index);
}

//}}} check_macroassembler_style

void
MacroAssembler::memoryBarrierBefore(const Synchronization& sync) {
    memoryBarrier(sync.barrierBefore);
}

void
MacroAssembler::memoryBarrierAfter(const Synchronization& sync) {
    memoryBarrier(sync.barrierAfter);
}

void
MacroAssembler::loadWasmTlsRegFromFrame(Register dest)
{
    loadPtr(Address(getStackPointer(), framePushed() + offsetof(wasm::Frame, tls)), dest);
}

void
MacroAssembler::BranchGCPtr::emit(MacroAssembler& masm)
{
    MOZ_ASSERT(isInitialized());
    masm.branchPtr(cond(), reg(), ptr_, jump());
}

void
MacroAssembler::debugAssertIsObject(const ValueOperand& val)
{
#ifdef DEBUG
    Label ok;
    branchTestObject(Assembler::Equal, val, &ok);
    assumeUnreachable("Expected an object!");
    bind(&ok);
#endif
}

void
MacroAssembler::debugAssertObjHasFixedSlots(Register obj, Register scratch)
{
#ifdef DEBUG
    Label hasFixedSlots;
    loadPtr(Address(obj, ShapedObject::offsetOfShape()), scratch);
    branchTest32(Assembler::NonZero,
                 Address(scratch, Shape::offsetOfSlotInfo()),
                 Imm32(Shape::fixedSlotsMask()),
                 &hasFixedSlots);
    assumeUnreachable("Expected a fixed slot");
    bind(&hasFixedSlots);
#endif
}

template <typename T, size_t N, typename P>
static bool
AddPendingReadBarrier(Vector<T*, N, P>& list, T* value)
{
    // Check if value is already present in tail of list.
    // TODO: Consider using a hash table here.
    const size_t TailWindow = 4;

    size_t len = list.length();
    for (size_t i = 0; i < Min(len, TailWindow); i++) {
        if (list[len - i - 1] == value)
            return true;
    }

    return list.append(value);
}

JSObject*
MacroAssembler::getSingletonAndDelayBarrier(const TypeSet* types, size_t i)
{
    JSObject* object = types->getSingletonNoBarrier(i);
    if (!object)
        return nullptr;

    if (!AddPendingReadBarrier(pendingObjectReadBarriers_, object))
        setOOM();

    return object;
}

ObjectGroup*
MacroAssembler::getGroupAndDelayBarrier(const TypeSet* types, size_t i)
{
    ObjectGroup* group = types->getGroupNoBarrier(i);
    if (!group)
        return nullptr;

    if (!AddPendingReadBarrier(pendingObjectGroupReadBarriers_, group))
        setOOM();

    return group;
}

void
MacroAssembler::performPendingReadBarriers()
{
    for (JSObject* object : pendingObjectReadBarriers_)
        JSObject::readBarrier(object);
    for (ObjectGroup* group : pendingObjectGroupReadBarriers_)
        ObjectGroup::readBarrier(group);
}

namespace js {
namespace jit {

#ifdef DEBUG
template <class RegisterType>
AutoGenericRegisterScope<RegisterType>::AutoGenericRegisterScope(MacroAssembler& masm, RegisterType reg)
  : RegisterType(reg), masm_(masm), released_(false)
{
    masm.debugTrackedRegisters_.add(reg);
}

template AutoGenericRegisterScope<Register>::AutoGenericRegisterScope(MacroAssembler& masm, Register reg);
template AutoGenericRegisterScope<FloatRegister>::AutoGenericRegisterScope(MacroAssembler& masm, FloatRegister reg);
#endif // DEBUG

#ifdef DEBUG
template <class RegisterType>
AutoGenericRegisterScope<RegisterType>::~AutoGenericRegisterScope()
{
    if (!released_)
        release();
}

template AutoGenericRegisterScope<Register>::~AutoGenericRegisterScope();
template AutoGenericRegisterScope<FloatRegister>::~AutoGenericRegisterScope();

template <class RegisterType>
void
AutoGenericRegisterScope<RegisterType>::release()
{
    MOZ_ASSERT(!released_);
    released_ = true;
    const RegisterType& reg = *dynamic_cast<RegisterType*>(this);
    masm_.debugTrackedRegisters_.take(reg);
}

template void AutoGenericRegisterScope<Register>::release();
template void AutoGenericRegisterScope<FloatRegister>::release();

template <class RegisterType>
void
AutoGenericRegisterScope<RegisterType>::reacquire()
{
    MOZ_ASSERT(released_);
    released_ = false;
    const RegisterType& reg = *dynamic_cast<RegisterType*>(this);
    masm_.debugTrackedRegisters_.add(reg);
}

template void AutoGenericRegisterScope<Register>::reacquire();
template void AutoGenericRegisterScope<FloatRegister>::reacquire();

#endif // DEBUG

} // namespace jit
} // namespace js
