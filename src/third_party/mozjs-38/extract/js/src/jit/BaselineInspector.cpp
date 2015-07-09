/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineInspector.h"

#include "mozilla/DebugOnly.h"

#include "jit/BaselineIC.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

bool
SetElemICInspector::sawOOBDenseWrite() const
{
    if (!icEntry_)
        return false;

    // Check for a SetElem_DenseAdd stub.
    for (ICStub* stub = icEntry_->firstStub(); stub; stub = stub->next()) {
        if (stub->isSetElem_DenseAdd())
            return true;
    }

    // Check for a write hole bit on the SetElem_Fallback stub.
    ICStub* stub = icEntry_->fallbackStub();
    if (stub->isSetElem_Fallback())
        return stub->toSetElem_Fallback()->hasArrayWriteHole();

    return false;
}

bool
SetElemICInspector::sawOOBTypedArrayWrite() const
{
    if (!icEntry_)
        return false;

    // Check for SetElem_TypedArray stubs with expectOutOfBounds set.
    for (ICStub* stub = icEntry_->firstStub(); stub; stub = stub->next()) {
        if (!stub->isSetElem_TypedArray())
            continue;
        if (stub->toSetElem_TypedArray()->expectOutOfBounds())
            return true;
    }
    return false;
}

bool
SetElemICInspector::sawDenseWrite() const
{
    if (!icEntry_)
        return false;

    // Check for a SetElem_DenseAdd or SetElem_Dense stub.
    for (ICStub* stub = icEntry_->firstStub(); stub; stub = stub->next()) {
        if (stub->isSetElem_DenseAdd() || stub->isSetElem_Dense())
            return true;
    }
    return false;
}

bool
SetElemICInspector::sawTypedArrayWrite() const
{
    if (!icEntry_)
        return false;

    // Check for a SetElem_TypedArray stub.
    for (ICStub* stub = icEntry_->firstStub(); stub; stub = stub->next()) {
        if (stub->isSetElem_TypedArray())
            return true;
    }
    return false;
}

template <typename S, typename T>
static bool
VectorAppendNoDuplicate(S& list, T value)
{
    for (size_t i = 0; i < list.length(); i++) {
        if (list[i] == value)
            return true;
    }
    return list.append(value);
}

bool
BaselineInspector::maybeInfoForPropertyOp(jsbytecode* pc,
                                          ShapeVector& nativeShapes,
                                          ObjectGroupVector& unboxedGroups,
                                          ObjectGroupVector& convertUnboxedGroups)
{
    // Return lists of native shapes and unboxed objects seen by the baseline
    // IC for the current op. Empty lists indicate no shapes/types are known,
    // or there was an uncacheable access. convertUnboxedGroups is used for
    // unboxed object groups which have been seen, but have had instances
    // converted to native objects and should be eagerly converted by Ion.
    MOZ_ASSERT(nativeShapes.empty());
    MOZ_ASSERT(unboxedGroups.empty());
    MOZ_ASSERT(convertUnboxedGroups.empty());

    if (!hasBaselineScript())
        return true;

    MOZ_ASSERT(isValidPC(pc));
    const ICEntry& entry = icEntryFromPC(pc);

    ICStub* stub = entry.firstStub();
    while (stub->next()) {
        Shape* shape = nullptr;
        ObjectGroup* group = nullptr;
        if (stub->isGetProp_Native()) {
            shape = stub->toGetProp_Native()->shape();
        } else if (stub->isSetProp_Native()) {
            shape = stub->toSetProp_Native()->shape();
        } else if (stub->isGetProp_Unboxed()) {
            group = stub->toGetProp_Unboxed()->group();
        } else if (stub->isSetProp_Unboxed()) {
            group = stub->toSetProp_Unboxed()->group();
        } else {
            nativeShapes.clear();
            unboxedGroups.clear();
            return true;
        }

        if (group && group->unboxedLayout().nativeGroup()) {
            if (!VectorAppendNoDuplicate(convertUnboxedGroups, group))
                return false;
            shape = group->unboxedLayout().nativeShape();
            group = nullptr;
        }

        if (shape) {
            if (!VectorAppendNoDuplicate(nativeShapes, shape))
                return false;
        } else {
            if (!VectorAppendNoDuplicate(unboxedGroups, group))
                return false;
        }

        stub = stub->next();
    }

    if (stub->isGetProp_Fallback()) {
        if (stub->toGetProp_Fallback()->hadUnoptimizableAccess()) {
            nativeShapes.clear();
            unboxedGroups.clear();
        }
    } else {
        if (stub->toSetProp_Fallback()->hadUnoptimizableAccess()) {
            nativeShapes.clear();
            unboxedGroups.clear();
        }
    }

    // Don't inline if there are more than 5 shapes/groups.
    if (nativeShapes.length() + unboxedGroups.length() > 5) {
        nativeShapes.clear();
        unboxedGroups.clear();
    }

    return true;
}

ICStub*
BaselineInspector::monomorphicStub(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return nullptr;

    const ICEntry& entry = icEntryFromPC(pc);

    ICStub* stub = entry.firstStub();
    ICStub* next = stub->next();

    if (!next || !next->isFallback())
        return nullptr;

    return stub;
}

bool
BaselineInspector::dimorphicStub(jsbytecode* pc, ICStub** pfirst, ICStub** psecond)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);

    ICStub* stub = entry.firstStub();
    ICStub* next = stub->next();
    ICStub* after = next ? next->next() : nullptr;

    if (!after || !after->isFallback())
        return false;

    *pfirst = stub;
    *psecond = next;
    return true;
}

MIRType
BaselineInspector::expectedResultType(jsbytecode* pc)
{
    // Look at the IC entries for this op to guess what type it will produce,
    // returning MIRType_None otherwise.

    ICStub* stub = monomorphicStub(pc);
    if (!stub)
        return MIRType_None;

    switch (stub->kind()) {
      case ICStub::BinaryArith_Int32:
        if (stub->toBinaryArith_Int32()->allowDouble())
            return MIRType_Double;
        return MIRType_Int32;
      case ICStub::BinaryArith_BooleanWithInt32:
      case ICStub::UnaryArith_Int32:
      case ICStub::BinaryArith_DoubleWithInt32:
        return MIRType_Int32;
      case ICStub::BinaryArith_Double:
      case ICStub::UnaryArith_Double:
        return MIRType_Double;
      case ICStub::BinaryArith_StringConcat:
      case ICStub::BinaryArith_StringObjectConcat:
        return MIRType_String;
      default:
        return MIRType_None;
    }
}

// Whether a baseline stub kind is suitable for a double comparison that
// converts its operands to doubles.
static bool
CanUseDoubleCompare(ICStub::Kind kind)
{
    return kind == ICStub::Compare_Double || kind == ICStub::Compare_NumberWithUndefined;
}

// Whether a baseline stub kind is suitable for an int32 comparison that
// converts its operands to int32.
static bool
CanUseInt32Compare(ICStub::Kind kind)
{
    return kind == ICStub::Compare_Int32 || kind == ICStub::Compare_Int32WithBoolean;
}

MCompare::CompareType
BaselineInspector::expectedCompareType(jsbytecode* pc)
{
    ICStub* first = monomorphicStub(pc), *second = nullptr;
    if (!first && !dimorphicStub(pc, &first, &second))
        return MCompare::Compare_Unknown;

    if (ICStub* fallback = second ? second->next() : first->next()) {
        MOZ_ASSERT(fallback->isFallback());
        if (fallback->toCompare_Fallback()->hadUnoptimizableAccess())
            return MCompare::Compare_Unknown;
    }

    if (CanUseInt32Compare(first->kind()) && (!second || CanUseInt32Compare(second->kind()))) {
        ICCompare_Int32WithBoolean* coerce =
            first->isCompare_Int32WithBoolean()
            ? first->toCompare_Int32WithBoolean()
            : ((second && second->isCompare_Int32WithBoolean())
               ? second->toCompare_Int32WithBoolean()
               : nullptr);
        if (coerce) {
            return coerce->lhsIsInt32()
                   ? MCompare::Compare_Int32MaybeCoerceRHS
                   : MCompare::Compare_Int32MaybeCoerceLHS;
        }
        return MCompare::Compare_Int32;
    }

    if (CanUseDoubleCompare(first->kind()) && (!second || CanUseDoubleCompare(second->kind()))) {
        ICCompare_NumberWithUndefined* coerce =
            first->isCompare_NumberWithUndefined()
            ? first->toCompare_NumberWithUndefined()
            : (second && second->isCompare_NumberWithUndefined())
              ? second->toCompare_NumberWithUndefined()
              : nullptr;
        if (coerce) {
            return coerce->lhsIsUndefined()
                   ? MCompare::Compare_DoubleMaybeCoerceLHS
                   : MCompare::Compare_DoubleMaybeCoerceRHS;
        }
        return MCompare::Compare_Double;
    }

    return MCompare::Compare_Unknown;
}

static bool
TryToSpecializeBinaryArithOp(ICStub** stubs,
                             uint32_t nstubs,
                             MIRType* result)
{
    DebugOnly<bool> sawInt32 = false;
    bool sawDouble = false;
    bool sawOther = false;

    for (uint32_t i = 0; i < nstubs; i++) {
        switch (stubs[i]->kind()) {
          case ICStub::BinaryArith_Int32:
            sawInt32 = true;
            break;
          case ICStub::BinaryArith_BooleanWithInt32:
            sawInt32 = true;
            break;
          case ICStub::BinaryArith_Double:
            sawDouble = true;
            break;
          case ICStub::BinaryArith_DoubleWithInt32:
            sawDouble = true;
            break;
          default:
            sawOther = true;
            break;
        }
    }

    if (sawOther)
        return false;

    if (sawDouble) {
        *result = MIRType_Double;
        return true;
    }

    MOZ_ASSERT(sawInt32);
    *result = MIRType_Int32;
    return true;
}

MIRType
BaselineInspector::expectedBinaryArithSpecialization(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return MIRType_None;

    MIRType result;
    ICStub* stubs[2];

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();
    if (stub->isBinaryArith_Fallback() &&
        stub->toBinaryArith_Fallback()->hadUnoptimizableOperands())
    {
        return MIRType_None;
    }

    stubs[0] = monomorphicStub(pc);
    if (stubs[0]) {
        if (TryToSpecializeBinaryArithOp(stubs, 1, &result))
            return result;
    }

    if (dimorphicStub(pc, &stubs[0], &stubs[1])) {
        if (TryToSpecializeBinaryArithOp(stubs, 2, &result))
            return result;
    }

    return MIRType_None;
}

bool
BaselineInspector::hasSeenNonNativeGetElement(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();

    if (stub->isGetElem_Fallback())
        return stub->toGetElem_Fallback()->hasNonNativeAccess();
    return false;
}

bool
BaselineInspector::hasSeenNegativeIndexGetElement(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();

    if (stub->isGetElem_Fallback())
        return stub->toGetElem_Fallback()->hasNegativeIndex();
    return false;
}

bool
BaselineInspector::hasSeenAccessedGetter(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();

    if (stub->isGetProp_Fallback())
        return stub->toGetProp_Fallback()->hasAccessedGetter();
    return false;
}

bool
BaselineInspector::hasSeenNonStringIterMore(jsbytecode* pc)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_MOREITER);

    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();

    return stub->toIteratorMore_Fallback()->hasNonStringResult();
}

bool
BaselineInspector::hasSeenDoubleResult(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.fallbackStub();

    MOZ_ASSERT(stub->isUnaryArith_Fallback() || stub->isBinaryArith_Fallback());

    if (stub->isUnaryArith_Fallback())
        return stub->toUnaryArith_Fallback()->sawDoubleResult();
    else
        return stub->toBinaryArith_Fallback()->sawDoubleResult();

    return false;
}

JSObject*
BaselineInspector::getTemplateObject(jsbytecode* pc)
{
    if (!hasBaselineScript())
        return nullptr;

    const ICEntry& entry = icEntryFromPC(pc);
    for (ICStub* stub = entry.firstStub(); stub; stub = stub->next()) {
        switch (stub->kind()) {
          case ICStub::NewArray_Fallback:
            return stub->toNewArray_Fallback()->templateObject();
          case ICStub::NewObject_Fallback:
            return stub->toNewObject_Fallback()->templateObject();
          case ICStub::Rest_Fallback:
            return stub->toRest_Fallback()->templateObject();
          case ICStub::Call_Scripted:
            if (JSObject* obj = stub->toCall_Scripted()->templateObject())
                return obj;
            break;
          default:
            break;
        }
    }

    return nullptr;
}

JSFunction*
BaselineInspector::getSingleCallee(jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_NEW);

    if (!hasBaselineScript())
        return nullptr;

    const ICEntry& entry = icEntryFromPC(pc);
    ICStub* stub = entry.firstStub();

    if (entry.fallbackStub()->toCall_Fallback()->hadUnoptimizableCall())
        return nullptr;

    if (!stub->isCall_Scripted() || stub->next() != entry.fallbackStub())
        return nullptr;

    return stub->toCall_Scripted()->callee();
}

JSObject*
BaselineInspector::getTemplateObjectForNative(jsbytecode* pc, Native native)
{
    if (!hasBaselineScript())
        return nullptr;

    const ICEntry& entry = icEntryFromPC(pc);
    for (ICStub* stub = entry.firstStub(); stub; stub = stub->next()) {
        if (stub->isCall_Native() && stub->toCall_Native()->callee()->native() == native)
            return stub->toCall_Native()->templateObject();
    }

    return nullptr;
}

bool
BaselineInspector::isOptimizableCallStringSplit(jsbytecode* pc, JSString** stringOut, JSString** stringArg,
                                                NativeObject** objOut)
{
    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);

    // If StringSplit stub is attached, must have only one stub attached.
    if (entry.fallbackStub()->numOptimizedStubs() != 1)
        return false;

    ICStub* stub = entry.firstStub();
    if (stub->kind() != ICStub::Call_StringSplit)
        return false;

    *stringOut = stub->toCall_StringSplit()->expectedThis();
    *stringArg = stub->toCall_StringSplit()->expectedArg();
    *objOut = stub->toCall_StringSplit()->templateObject();
    return true;
}

JSObject*
BaselineInspector::getTemplateObjectForClassHook(jsbytecode* pc, const Class* clasp)
{
    if (!hasBaselineScript())
        return nullptr;

    const ICEntry& entry = icEntryFromPC(pc);
    for (ICStub* stub = entry.firstStub(); stub; stub = stub->next()) {
        if (stub->isCall_ClassHook() && stub->toCall_ClassHook()->clasp() == clasp)
            return stub->toCall_ClassHook()->templateObject();
    }

    return nullptr;
}

DeclEnvObject*
BaselineInspector::templateDeclEnvObject()
{
    if (!hasBaselineScript())
        return nullptr;

    JSObject* res = &templateCallObject()->as<ScopeObject>().enclosingScope();
    MOZ_ASSERT(res);

    return &res->as<DeclEnvObject>();
}

CallObject*
BaselineInspector::templateCallObject()
{
    if (!hasBaselineScript())
        return nullptr;

    JSObject* res = baselineScript()->templateScope();
    MOZ_ASSERT(res);

    return &res->as<CallObject>();
}

static Shape* GlobalShapeForGetPropFunction(ICStub* stub)
{
    if (stub->isGetProp_CallNativePrototype()) {
        ICGetProp_CallNativePrototype* nstub =
            stub->toGetProp_CallNativePrototype();
        if (nstub->receiverShape()->getObjectClass()->flags & JSCLASS_IS_GLOBAL)
            return nstub->receiverShape();
    }
    return nullptr;
}

static bool
AddReceiverShape(BaselineInspector::ShapeVector& shapes, Shape* shape)
{
    MOZ_ASSERT(shape);

    for (size_t i = 0; i < shapes.length(); i++) {
        if (shapes[i] == shape)
            return true;
    }

    return shapes.append(shape);
}

static bool
AddReceiverShapeForGetPropFunction(BaselineInspector::ShapeVector& shapes, ICStub* stub)
{
    if (stub->isGetProp_CallNative())
        return true;

    Shape* shape = nullptr;
    if (stub->isGetProp_CallScripted())
        shape = stub->toGetProp_CallScripted()->receiverShape();
    else
        shape = stub->toGetProp_CallNativePrototype()->receiverShape();

    return AddReceiverShape(shapes, shape);
}

bool
BaselineInspector::commonGetPropFunction(jsbytecode* pc, JSObject** holder, Shape** holderShape,
                                         JSFunction** commonGetter, Shape** globalShape,
                                         bool* isOwnProperty, ShapeVector& receiverShapes)
{
    if (!hasBaselineScript())
        return false;

    MOZ_ASSERT(receiverShapes.empty());

    *holder = nullptr;
    const ICEntry& entry = icEntryFromPC(pc);

    for (ICStub* stub = entry.firstStub(); stub; stub = stub->next()) {
        if (stub->isGetProp_CallScripted()  ||
            stub->isGetProp_CallNative()    ||
            stub->isGetProp_CallNativePrototype())
        {
            ICGetPropCallGetter* nstub = static_cast<ICGetPropCallGetter*>(stub);
            bool isOwn = stub->isGetProp_CallNative();
            if (!AddReceiverShapeForGetPropFunction(receiverShapes, nstub))
                return false;

            if (!*holder) {
                *holder = nstub->holder();
                *holderShape = nstub->holderShape();
                *commonGetter = nstub->getter();
                *globalShape = GlobalShapeForGetPropFunction(nstub);
                *isOwnProperty = isOwn;
            } else if (nstub->holderShape() != *holderShape ||
                       GlobalShapeForGetPropFunction(nstub) != *globalShape ||
                       isOwn != *isOwnProperty)
            {
                return false;
            } else {
                MOZ_ASSERT(*commonGetter == nstub->getter());
            }
        } else if (stub->isGetProp_Fallback()) {
            // If we have an unoptimizable access, don't try to optimize.
            if (stub->toGetProp_Fallback()->hadUnoptimizableAccess())
                return false;
        } else if (stub->isGetName_Fallback()) {
            if (stub->toGetName_Fallback()->hadUnoptimizableAccess())
                return false;
        } else {
            return false;
        }
    }

    if (!*holder)
        return false;

    MOZ_ASSERT(*isOwnProperty == receiverShapes.empty());
    return true;
}

bool
BaselineInspector::commonSetPropFunction(jsbytecode* pc, JSObject** holder, Shape** holderShape,
                                         JSFunction** commonSetter, bool* isOwnProperty,
                                         ShapeVector& receiverShapes)
{
    if (!hasBaselineScript())
        return false;

    MOZ_ASSERT(receiverShapes.empty());

    *holder = nullptr;
    const ICEntry& entry = icEntryFromPC(pc);

    for (ICStub* stub = entry.firstStub(); stub; stub = stub->next()) {
        if (stub->isSetProp_CallScripted() || stub->isSetProp_CallNative()) {
            ICSetPropCallSetter* nstub = static_cast<ICSetPropCallSetter*>(stub);
            if (!AddReceiverShape(receiverShapes, nstub->shape()))
                return false;

            if (!*holder) {
                *holder = nstub->holder();
                *holderShape = nstub->holderShape();
                *commonSetter = nstub->setter();
                *isOwnProperty = false;
            } else if (nstub->holderShape() != *holderShape) {
                return false;
            } else {
                MOZ_ASSERT(*commonSetter == nstub->setter());
            }
        } else if (!stub->isSetProp_Fallback() ||
                   stub->toSetProp_Fallback()->hadUnoptimizableAccess())
        {
            // We have an unoptimizable access, so don't try to optimize.
            return false;
        }
    }

    if (!*holder)
        return false;

    return true;
}

bool
BaselineInspector::instanceOfData(jsbytecode* pc, Shape** shape, uint32_t* slot,
                                  JSObject** prototypeObject)
{
    MOZ_ASSERT(*pc == JSOP_INSTANCEOF);

    if (!hasBaselineScript())
        return false;

    const ICEntry& entry = icEntryFromPC(pc);

    ICStub* stub = entry.firstStub();
    if (!stub->isInstanceOf_Function() ||
        !stub->next()->isInstanceOf_Fallback() ||
        stub->next()->toInstanceOf_Fallback()->hadUnoptimizableAccess())
    {
        return false;
    }

    ICInstanceOf_Function* optStub = stub->toInstanceOf_Function();
    *shape = optStub->shape();
    *prototypeObject = optStub->prototypeObject();
    *slot = optStub->slot();

    if (IsInsideNursery(*prototypeObject))
        return false;

    return true;
}
