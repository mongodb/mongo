/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIR.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <ctype.h>

#include "jslibmath.h"
#include "jsstr.h"

#include "jit/BaselineInspector.h"
#include "jit/IonBuilder.h"
#include "jit/JitSpewer.h"
#include "jit/MIRGraph.h"
#include "jit/RangeAnalysis.h"
#include "js/Conversions.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"

using namespace js;
using namespace js::jit;

using JS::ToInt32;

using mozilla::NumbersAreIdentical;
using mozilla::IsFloat32Representable;
using mozilla::IsNaN;
using mozilla::Maybe;
using mozilla::DebugOnly;

#ifdef DEBUG
size_t MUse::index() const
{
    return consumer()->indexOf(this);
}
#endif

template<size_t Op> static void
ConvertDefinitionToDouble(TempAllocator& alloc, MDefinition* def, MInstruction* consumer)
{
    MInstruction* replace = MToDouble::New(alloc, def);
    consumer->replaceOperand(Op, replace);
    consumer->block()->insertBefore(consumer, replace);
}

static bool
CheckUsesAreFloat32Consumers(MInstruction* ins)
{
    bool allConsumerUses = true;
    for (MUseDefIterator use(ins); allConsumerUses && use; use++)
        allConsumerUses &= use.def()->canConsumeFloat32(use.use());
    return allConsumerUses;
}

void
MDefinition::PrintOpcodeName(FILE* fp, MDefinition::Opcode op)
{
    static const char * const names[] =
    {
#define NAME(x) #x,
        MIR_OPCODE_LIST(NAME)
#undef NAME
    };
    const char* name = names[op];
    size_t len = strlen(name);
    for (size_t i = 0; i < len; i++)
        fprintf(fp, "%c", tolower(name[i]));
}

const Value&
MDefinition::constantValue()
{
    MOZ_ASSERT(isConstantValue());

    if (isBox())
        return getOperand(0)->constantValue();
    return toConstant()->value();
}

const Value*
MDefinition::constantVp()
{
    MOZ_ASSERT(isConstantValue());
    if (isBox())
        return getOperand(0)->constantVp();
    return toConstant()->vp();
}

bool
MDefinition::constantToBoolean()
{
    MOZ_ASSERT(isConstantValue());
    if (isBox())
        return getOperand(0)->constantToBoolean();
    return toConstant()->valueToBoolean();
}

static MConstant*
EvaluateConstantOperands(TempAllocator& alloc, MBinaryInstruction* ins, bool* ptypeChange = nullptr)
{
    MDefinition* left = ins->getOperand(0);
    MDefinition* right = ins->getOperand(1);

    if (!left->isConstantValue() || !right->isConstantValue())
        return nullptr;

    Value lhs = left->constantValue();
    Value rhs = right->constantValue();
    Value ret = UndefinedValue();

    switch (ins->op()) {
      case MDefinition::Op_BitAnd:
        ret = Int32Value(lhs.toInt32() & rhs.toInt32());
        break;
      case MDefinition::Op_BitOr:
        ret = Int32Value(lhs.toInt32() | rhs.toInt32());
        break;
      case MDefinition::Op_BitXor:
        ret = Int32Value(lhs.toInt32() ^ rhs.toInt32());
        break;
      case MDefinition::Op_Lsh:
        ret = Int32Value(uint32_t(lhs.toInt32()) << (rhs.toInt32() & 0x1F));
        break;
      case MDefinition::Op_Rsh:
        ret = Int32Value(lhs.toInt32() >> (rhs.toInt32() & 0x1F));
        break;
      case MDefinition::Op_Ursh:
        ret.setNumber(uint32_t(lhs.toInt32()) >> (rhs.toInt32() & 0x1F));
        break;
      case MDefinition::Op_Add:
        ret.setNumber(lhs.toNumber() + rhs.toNumber());
        break;
      case MDefinition::Op_Sub:
        ret.setNumber(lhs.toNumber() - rhs.toNumber());
        break;
      case MDefinition::Op_Mul:
        ret.setNumber(lhs.toNumber() * rhs.toNumber());
        break;
      case MDefinition::Op_Div:
        if (ins->toDiv()->isUnsigned())
            ret.setInt32(rhs.isInt32(0) ? 0 : uint32_t(lhs.toInt32()) / uint32_t(rhs.toInt32()));
        else
            ret.setNumber(NumberDiv(lhs.toNumber(), rhs.toNumber()));
        break;
      case MDefinition::Op_Mod:
        if (ins->toMod()->isUnsigned())
            ret.setInt32(rhs.isInt32(0) ? 0 : uint32_t(lhs.toInt32()) % uint32_t(rhs.toInt32()));
        else
            ret.setNumber(NumberMod(lhs.toNumber(), rhs.toNumber()));
        break;
      default:
        MOZ_CRASH("NYI");
    }

    // setNumber eagerly transforms a number to int32.
    // Transform back to double, if the output type is double.
    if (ins->type() == MIRType_Double && ret.isInt32())
        ret.setDouble(ret.toNumber());

    if (ins->type() != MIRTypeFromValue(ret)) {
        if (ptypeChange)
            *ptypeChange = true;
        return nullptr;
    }

    return MConstant::New(alloc, ret);
}

static MMul*
EvaluateExactReciprocal(TempAllocator& alloc, MDiv* ins)
{
    // we should fold only when it is a floating point operation
    if (!IsFloatingPointType(ins->type()))
        return nullptr;

    MDefinition* left = ins->getOperand(0);
    MDefinition* right = ins->getOperand(1);

    if (!right->isConstantValue())
        return nullptr;

    Value rhs = right->constantValue();

    int32_t num;
    if (!mozilla::NumberIsInt32(rhs.toNumber(), &num))
        return nullptr;

    // check if rhs is a power of two
    if (mozilla::Abs(num) & (mozilla::Abs(num) - 1))
        return nullptr;

    Value ret;
    ret.setDouble(1.0 / (double) num);
    MConstant* foldedRhs = MConstant::New(alloc, ret);
    foldedRhs->setResultType(ins->type());
    ins->block()->insertBefore(ins, foldedRhs);

    MMul* mul = MMul::New(alloc, left, foldedRhs, ins->type());
    mul->setCommutative();
    return mul;
}

void
MDefinition::printName(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    fprintf(fp, "%u", id());
}

HashNumber
MDefinition::addU32ToHash(HashNumber hash, uint32_t data)
{
    return data + (hash << 6) + (hash << 16) - hash;
}

HashNumber
MDefinition::valueHash() const
{
    HashNumber out = op();
    for (size_t i = 0, e = numOperands(); i < e; i++)
        out = addU32ToHash(out, getOperand(i)->id());
    if (MInstruction* dep = dependency())
        out = addU32ToHash(out, dep->id());
    return out;
}

bool
MDefinition::congruentIfOperandsEqual(const MDefinition* ins) const
{
    if (op() != ins->op())
        return false;

    if (type() != ins->type())
        return false;

    if (isEffectful() || ins->isEffectful())
        return false;

    if (numOperands() != ins->numOperands())
        return false;

    for (size_t i = 0, e = numOperands(); i < e; i++) {
        if (getOperand(i) != ins->getOperand(i))
            return false;
    }

    return true;
}

MDefinition*
MDefinition::foldsTo(TempAllocator& alloc)
{
    // In the default case, there are no constants to fold.
    return this;
}

bool
MDefinition::mightBeMagicType() const
{
    if (IsMagicType(type()))
        return true;

    if (MIRType_Value != type())
        return false;

    return !resultTypeSet() || resultTypeSet()->hasType(TypeSet::MagicArgType());
}

MDefinition*
MInstruction::foldsToStoredValue(TempAllocator& alloc, MDefinition* loaded)
{
    // If the type are matching then we return the value which is used as
    // argument of the store.
    if (loaded->type() != type()) {
        // If we expect to read a type which is more generic than the type seen
        // by the store, then we box the value used by the store.
        if (type() != MIRType_Value)
            return this;

        MOZ_ASSERT(loaded->type() < MIRType_Value);
        MBox* box = MBox::New(alloc, loaded);
        block()->insertBefore(this, box);
        loaded = box;
    }

    return loaded;
}

void
MDefinition::analyzeEdgeCasesForward()
{
}

void
MDefinition::analyzeEdgeCasesBackward()
{
}

void
MInstruction::setResumePoint(MResumePoint* resumePoint)
{
    MOZ_ASSERT(!resumePoint_);
    resumePoint_ = resumePoint;
    resumePoint_->setInstruction(this);
}

void
MInstruction::stealResumePoint(MInstruction* ins)
{
    MOZ_ASSERT(ins->resumePoint_->instruction() == ins);
    resumePoint_ = ins->resumePoint_;
    ins->resumePoint_ = nullptr;
    resumePoint_->replaceInstruction(this);
}

void
MInstruction::moveResumePointAsEntry()
{
    MOZ_ASSERT(isNop());
    block()->clearEntryResumePoint();
    block()->setEntryResumePoint(resumePoint_);
    resumePoint_->resetInstruction();
    resumePoint_ = nullptr;
}

void
MInstruction::clearResumePoint()
{
    resumePoint_->resetInstruction();
    block()->discardPreAllocatedResumePoint(resumePoint_);
    resumePoint_ = nullptr;
}

static bool
MaybeEmulatesUndefined(CompilerConstraintList* constraints, MDefinition* op)
{
    if (!op->mightBeType(MIRType_Object))
        return false;

    TemporaryTypeSet* types = op->resultTypeSet();
    if (!types)
        return true;

    return types->maybeEmulatesUndefined(constraints);
}

static bool
MaybeCallable(CompilerConstraintList* constraints, MDefinition* op)
{
    if (!op->mightBeType(MIRType_Object))
        return false;

    TemporaryTypeSet* types = op->resultTypeSet();
    if (!types)
        return true;

    return types->maybeCallable(constraints);
}

MTest*
MTest::New(TempAllocator& alloc, MDefinition* ins, MBasicBlock* ifTrue, MBasicBlock* ifFalse)
{
    return new(alloc) MTest(ins, ifTrue, ifFalse);
}

void
MTest::cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(operandMightEmulateUndefined());

    if (!MaybeEmulatesUndefined(constraints, getOperand(0)))
        markOperandCantEmulateUndefined();
}

MDefinition*
MTest::foldsTo(TempAllocator& alloc)
{
    MDefinition* op = getOperand(0);

    if (op->isNot()) {
        // If the operand of the Not is itself a Not, they cancel out.
        MDefinition* opop = op->getOperand(0);
        if (opop->isNot())
            return MTest::New(alloc, opop->toNot()->input(), ifTrue(), ifFalse());
        return MTest::New(alloc, op->toNot()->input(), ifFalse(), ifTrue());
    }

    if (op->isConstantValue() && !op->constantValue().isMagic())
        return MGoto::New(alloc, op->constantToBoolean() ? ifTrue() : ifFalse());

    switch (op->type()) {
      case MIRType_Undefined:
      case MIRType_Null:
        return MGoto::New(alloc, ifFalse());
      case MIRType_Symbol:
        return MGoto::New(alloc, ifTrue());
      case MIRType_Object:
        if (!operandMightEmulateUndefined())
            return MGoto::New(alloc, ifTrue());
        break;
      default:
        break;
    }

    return this;
}

void
MTest::filtersUndefinedOrNull(bool trueBranch, MDefinition** subject, bool* filtersUndefined,
                              bool* filtersNull)
{
    MDefinition* ins = getOperand(0);
    if (ins->isCompare()) {
        ins->toCompare()->filtersUndefinedOrNull(trueBranch, subject, filtersUndefined, filtersNull);
        return;
    }

    if (!trueBranch && ins->isNot()) {
        *subject = ins->getOperand(0);
        *filtersUndefined = *filtersNull = true;
        return;
    }

    if (trueBranch) {
        *subject = ins;
        *filtersUndefined = *filtersNull = true;
        return;
    }

    *filtersUndefined = *filtersNull = false;
    *subject = nullptr;
}

void
MDefinition::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    for (size_t j = 0, e = numOperands(); j < e; j++) {
        fprintf(fp, " ");
        if (getUseFor(j)->hasProducer())
            getOperand(j)->printName(fp);
        else
            fprintf(fp, "(null)");
    }
}

void
MDefinition::dump(FILE* fp) const
{
    printName(fp);
    fprintf(fp, " = ");
    printOpcode(fp);
    fprintf(fp, "\n");

    if (isInstruction()) {
        if (MResumePoint* resume = toInstruction()->resumePoint())
            resume->dump(fp);
    }
}

void
MDefinition::dump() const
{
    dump(stderr);
}

void
MDefinition::dumpLocation(FILE* fp) const
{
    MResumePoint* rp = nullptr;
    const char* linkWord = nullptr;
    if (isInstruction() && toInstruction()->resumePoint()) {
        rp = toInstruction()->resumePoint();
        linkWord = "at";
    } else {
        rp = block()->entryResumePoint();
        linkWord = "after";
    }

    while (rp) {
        JSScript* script = rp->block()->info().script();
        uint32_t lineno = PCToLineNumber(rp->block()->info().script(), rp->pc());
        fprintf(fp, "  %s %s:%d\n", linkWord, script->filename(), lineno);
        rp = rp->caller();
        linkWord = "in";
    }
}

void
MDefinition::dumpLocation() const
{
    dumpLocation(stderr);
}

#ifdef DEBUG
size_t
MDefinition::useCount() const
{
    size_t count = 0;
    for (MUseIterator i(uses_.begin()); i != uses_.end(); i++)
        count++;
    return count;
}

size_t
MDefinition::defUseCount() const
{
    size_t count = 0;
    for (MUseIterator i(uses_.begin()); i != uses_.end(); i++)
        if ((*i)->consumer()->isDefinition())
            count++;
    return count;
}
#endif

bool
MDefinition::hasOneUse() const
{
    MUseIterator i(uses_.begin());
    if (i == uses_.end())
        return false;
    i++;
    return i == uses_.end();
}

bool
MDefinition::hasOneDefUse() const
{
    bool hasOneDefUse = false;
    for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
        if (!(*i)->consumer()->isDefinition())
            continue;

        // We already have a definition use. So 1+
        if (hasOneDefUse)
            return false;

        // We saw one definition. Loop to test if there is another.
        hasOneDefUse = true;
    }

    return hasOneDefUse;
}

bool
MDefinition::hasDefUses() const
{
    for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
        if ((*i)->consumer()->isDefinition())
            return true;
    }

    return false;
}

bool
MDefinition::hasLiveDefUses() const
{
    for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
        MNode* ins = (*i)->consumer();
        if (ins->isDefinition()) {
            if (!ins->toDefinition()->isRecoveredOnBailout())
                return true;
        } else {
            MOZ_ASSERT(ins->isResumePoint());
            if (!ins->toResumePoint()->isRecoverableOperand(*i))
                return true;
        }
    }

    return false;
}

void
MDefinition::replaceAllUsesWith(MDefinition* dom)
{
    for (size_t i = 0, e = numOperands(); i < e; ++i)
        getOperand(i)->setUseRemovedUnchecked();

    justReplaceAllUsesWith(dom);
}

void
MDefinition::justReplaceAllUsesWith(MDefinition* dom)
{
    MOZ_ASSERT(dom != nullptr);
    MOZ_ASSERT(dom != this);

    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ++i)
        i->setProducerUnchecked(dom);
    dom->uses_.takeElements(uses_);
}

void
MDefinition::optimizeOutAllUses(TempAllocator& alloc)
{
    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
        MUse* use = *i++;
        MConstant* constant = use->consumer()->block()->optimizedOutConstant(alloc);

        // Update the resume point operand to use the optimized-out constant.
        use->setProducerUnchecked(constant);
        constant->addUseUnchecked(use);
    }

    // Remove dangling pointers.
    this->uses_.clear();
}

bool
MDefinition::emptyResultTypeSet() const
{
    return resultTypeSet() && resultTypeSet()->empty();
}

MConstant*
MConstant::New(TempAllocator& alloc, const Value& v, CompilerConstraintList* constraints)
{
    return new(alloc) MConstant(v, constraints);
}

MConstant*
MConstant::NewTypedValue(TempAllocator& alloc, const Value& v, MIRType type, CompilerConstraintList* constraints)
{
    MOZ_ASSERT(!IsSimdType(type));
    MOZ_ASSERT_IF(type == MIRType_Float32, IsNaN(v.toDouble()) || v.toDouble() == double(float(v.toDouble())));
    MConstant* constant = new(alloc) MConstant(v, constraints);
    constant->setResultType(type);
    return constant;
}

MConstant*
MConstant::NewAsmJS(TempAllocator& alloc, const Value& v, MIRType type)
{
    if (type == MIRType_Float32)
        return NewTypedValue(alloc, Float32Value(v.toNumber()), type);
    return NewTypedValue(alloc, v, type);
}

MConstant*
MConstant::NewConstraintlessObject(TempAllocator& alloc, JSObject* v)
{
    return new(alloc) MConstant(v);
}

TemporaryTypeSet*
jit::MakeSingletonTypeSet(CompilerConstraintList* constraints, JSObject* obj)
{
    // Invalidate when this object's ObjectGroup gets unknown properties. This
    // happens for instance when we mutate an object's __proto__, in this case
    // we want to invalidate and mark this TypeSet as containing AnyObject
    // (because mutating __proto__ will change an object's ObjectGroup).
    MOZ_ASSERT(constraints);
    TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(obj);
    key->hasStableClassAndProto(constraints);

    LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
    return alloc->new_<TemporaryTypeSet>(alloc, TypeSet::ObjectType(obj));
}

static TemporaryTypeSet*
MakeUnknownTypeSet()
{
    LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
    return alloc->new_<TemporaryTypeSet>(alloc, TypeSet::UnknownType());
}

MConstant::MConstant(const js::Value& vp, CompilerConstraintList* constraints)
  : value_(vp)
{
    setResultType(MIRTypeFromValue(vp));
    if (vp.isObject()) {
        // Create a singleton type set for the object. This isn't necessary for
        // other types as the result type encodes all needed information.
        MOZ_ASSERT(!IsInsideNursery(&vp.toObject()));
        setResultTypeSet(MakeSingletonTypeSet(constraints, &vp.toObject()));
    }
    if (vp.isMagic() && vp.whyMagic() == JS_UNINITIALIZED_LEXICAL) {
        // JS_UNINITIALIZED_LEXICAL does not escape to script and is not
        // observed in type sets. However, it may flow around freely during
        // Ion compilation. Give it an unknown typeset to poison any type sets
        // it merges with.
        //
        // TODO We could track uninitialized lexicals more precisely by tracking
        // them in type sets.
        setResultTypeSet(MakeUnknownTypeSet());
    }

    setMovable();
}

MConstant::MConstant(JSObject* obj)
  : value_(ObjectValue(*obj))
{
    MOZ_ASSERT(!IsInsideNursery(obj));
    setResultType(MIRType_Object);
    setMovable();
}

HashNumber
MConstant::valueHash() const
{
    // Fold all 64 bits into the 32-bit result. It's tempting to just discard
    // half of the bits, as this is just a hash, however there are many common
    // patterns of values where only the low or the high bits vary, so
    // discarding either side would lead to excessive hash collisions.
    uint64_t bits = JSVAL_TO_IMPL(value_).asBits;
    return (HashNumber)bits ^ (HashNumber)(bits >> 32);
}

bool
MConstant::congruentTo(const MDefinition* ins) const
{
    if (!ins->isConstant())
        return false;
    return ins->toConstant()->value() == value();
}

void
MConstant::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    fprintf(fp, " ");
    switch (type()) {
      case MIRType_Undefined:
        fprintf(fp, "undefined");
        break;
      case MIRType_Null:
        fprintf(fp, "null");
        break;
      case MIRType_Boolean:
        fprintf(fp, value().toBoolean() ? "true" : "false");
        break;
      case MIRType_Int32:
        fprintf(fp, "0x%x", value().toInt32());
        break;
      case MIRType_Double:
        fprintf(fp, "%f", value().toDouble());
        break;
      case MIRType_Float32:
      {
        float val = value().toDouble();
        fprintf(fp, "%f", val);
        break;
      }
      case MIRType_Object:
        if (value().toObject().is<JSFunction>()) {
            JSFunction* fun = &value().toObject().as<JSFunction>();
            if (fun->displayAtom()) {
                fputs("function ", fp);
                FileEscapedString(fp, fun->displayAtom(), 0);
            } else {
                fputs("unnamed function", fp);
            }
            if (fun->hasScript()) {
                JSScript* script = fun->nonLazyScript();
                fprintf(fp, " (%s:%d)",
                        script->filename() ? script->filename() : "", (int) script->lineno());
            }
            fprintf(fp, " at %p", (void*) fun);
            break;
        }
        fprintf(fp, "object %p (%s)", (void*)&value().toObject(),
                value().toObject().getClass()->name);
        break;
      case MIRType_Symbol:
        fprintf(fp, "symbol at %p", (void*)value().toSymbol());
        break;
      case MIRType_String:
        fprintf(fp, "string %p", (void*)value().toString());
        break;
      case MIRType_MagicOptimizedArguments:
        fprintf(fp, "magic lazyargs");
        break;
      case MIRType_MagicHole:
        fprintf(fp, "magic hole");
        break;
      case MIRType_MagicIsConstructing:
        fprintf(fp, "magic is-constructing");
        break;
      case MIRType_MagicOptimizedOut:
        fprintf(fp, "magic optimized-out");
        break;
      case MIRType_MagicUninitializedLexical:
        fprintf(fp, "magic uninitialized-lexical");
        break;
      default:
        MOZ_CRASH("unexpected type");
    }
}

bool
MConstant::canProduceFloat32() const
{
    if (!IsNumberType(type()))
        return false;

    if (type() == MIRType_Int32)
        return IsFloat32Representable(static_cast<double>(value_.toInt32()));
    if (type() == MIRType_Double)
        return IsFloat32Representable(value_.toDouble());
    return true;
}

MNurseryObject::MNurseryObject(JSObject* obj, uint32_t index, CompilerConstraintList* constraints)
  : index_(index)
{
    setResultType(MIRType_Object);

    MOZ_ASSERT(IsInsideNursery(obj));
    MOZ_ASSERT(!obj->isSingleton());
    setResultTypeSet(MakeSingletonTypeSet(constraints, obj));

    setMovable();
}

MNurseryObject*
MNurseryObject::New(TempAllocator& alloc, JSObject* obj, uint32_t index,
                    CompilerConstraintList* constraints)
{
    return new(alloc) MNurseryObject(obj, index, constraints);
}

HashNumber
MNurseryObject::valueHash() const
{
    return HashNumber(index_);
}

bool
MNurseryObject::congruentTo(const MDefinition* ins) const
{
    if (!ins->isNurseryObject())
        return false;
    return ins->toNurseryObject()->index_ == index_;
}

MDefinition*
MSimdValueX4::foldsTo(TempAllocator& alloc)
{
    DebugOnly<MIRType> scalarType = SimdTypeToScalarType(type());
    bool allConstants = true;
    bool allSame = true;

    for (size_t i = 0; i < 4; ++i) {
        MDefinition* op = getOperand(i);
        MOZ_ASSERT(op->type() == scalarType);
        if (!op->isConstantValue())
            allConstants = false;
        if (i > 0 && op != getOperand(i - 1))
            allSame = false;
    }

    if (!allConstants && !allSame)
        return this;

    if (allConstants) {
        SimdConstant cst;
        switch (type()) {
          case MIRType_Int32x4: {
            int32_t a[4];
            for (size_t i = 0; i < 4; ++i)
                a[i] = getOperand(i)->constantValue().toInt32();
            cst = SimdConstant::CreateX4(a);
            break;
          }
          case MIRType_Float32x4: {
            float a[4];
            for (size_t i = 0; i < 4; ++i)
                a[i] = getOperand(i)->constantValue().toNumber();
            cst = SimdConstant::CreateX4(a);
            break;
          }
          default: MOZ_CRASH("unexpected type in MSimdValueX4::foldsTo");
        }

        return MSimdConstant::New(alloc, cst, type());
    }

    MOZ_ASSERT(allSame);
    return MSimdSplatX4::New(alloc, type(), getOperand(0));
}

MDefinition*
MSimdSplatX4::foldsTo(TempAllocator& alloc)
{
    DebugOnly<MIRType> scalarType = SimdTypeToScalarType(type());
    MDefinition* op = getOperand(0);
    if (!op->isConstantValue())
        return this;
    MOZ_ASSERT(op->type() == scalarType);

    SimdConstant cst;
    switch (type()) {
      case MIRType_Int32x4: {
        int32_t a[4];
        int32_t v = getOperand(0)->constantValue().toInt32();
        for (size_t i = 0; i < 4; ++i)
            a[i] = v;
        cst = SimdConstant::CreateX4(a);
        break;
      }
      case MIRType_Float32x4: {
        float a[4];
        float v = getOperand(0)->constantValue().toNumber();
        for (size_t i = 0; i < 4; ++i)
            a[i] = v;
        cst = SimdConstant::CreateX4(a);
        break;
      }
      default: MOZ_CRASH("unexpected type in MSimdSplatX4::foldsTo");
    }

    return MSimdConstant::New(alloc, cst, type());
}

MDefinition*
MSimdSwizzle::foldsTo(TempAllocator& alloc)
{
    if (lanesMatch(0, 1, 2, 3))
        return input();
    return this;
}

MCloneLiteral*
MCloneLiteral::New(TempAllocator& alloc, MDefinition* obj)
{
    return new(alloc) MCloneLiteral(obj);
}

void
MControlInstruction::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);
    for (size_t j = 0; j < numSuccessors(); j++)
        fprintf(fp, " block%u", getSuccessor(j)->id());
}

void
MCompare::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);
    fprintf(fp, " %s", js_CodeName[jsop()]);
}

void
MConstantElements::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    fprintf(fp, " %p", value());
}

void
MLoadTypedArrayElement::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);
    fprintf(fp, " %s", ScalarTypeDescr::typeName(arrayType()));
}

void
MAssertRange::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);
    Sprinter sp(GetJitContext()->cx);
    sp.init();
    assertedRange()->print(sp);
    fprintf(fp, " %s", sp.string());
}

const char*
MMathFunction::FunctionName(Function function)
{
    switch (function) {
      case Log:    return "Log";
      case Sin:    return "Sin";
      case Cos:    return "Cos";
      case Exp:    return "Exp";
      case Tan:    return "Tan";
      case ACos:   return "ACos";
      case ASin:   return "ASin";
      case ATan:   return "ATan";
      case Log10:  return "Log10";
      case Log2:   return "Log2";
      case Log1P:  return "Log1P";
      case ExpM1:  return "ExpM1";
      case CosH:   return "CosH";
      case SinH:   return "SinH";
      case TanH:   return "TanH";
      case ACosH:  return "ACosH";
      case ASinH:  return "ASinH";
      case ATanH:  return "ATanH";
      case Sign:   return "Sign";
      case Trunc:  return "Trunc";
      case Cbrt:   return "Cbrt";
      case Floor:  return "Floor";
      case Ceil:   return "Ceil";
      case Round:  return "Round";
      default:
        MOZ_CRASH("Unknown math function");
    }
}

void
MMathFunction::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);
    fprintf(fp, " %s", FunctionName(function()));
}

MDefinition*
MMathFunction::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (!input->isConstant())
        return this;

    Value val = input->toConstant()->value();
    if (!val.isNumber())
        return this;

    double in = val.toNumber();
    double out;
    switch (function_) {
      case Log:
        out = js::math_log_uncached(in);
        break;
      case Sin:
        out = js::math_sin_uncached(in);
        break;
      case Cos:
        out = js::math_cos_uncached(in);
        break;
      case Exp:
        out = js::math_exp_uncached(in);
        break;
      case Tan:
        out = js::math_tan_uncached(in);
        break;
      case ACos:
        out = js::math_acos_uncached(in);
        break;
      case ASin:
        out = js::math_asin_uncached(in);
        break;
      case ATan:
        out = js::math_atan_uncached(in);
        break;
      case Log10:
        out = js::math_log10_uncached(in);
        break;
      case Log2:
        out = js::math_log2_uncached(in);
        break;
      case Log1P:
        out = js::math_log1p_uncached(in);
        break;
      case ExpM1:
        out = js::math_expm1_uncached(in);
        break;
      case CosH:
        out = js::math_cosh_uncached(in);
        break;
      case SinH:
        out = js::math_sinh_uncached(in);
        break;
      case TanH:
        out = js::math_tanh_uncached(in);
        break;
      case ACosH:
        out = js::math_acosh_uncached(in);
        break;
      case ASinH:
        out = js::math_asinh_uncached(in);
        break;
      case ATanH:
        out = js::math_atanh_uncached(in);
        break;
      case Sign:
        out = js::math_sign_uncached(in);
        break;
      case Trunc:
        out = js::math_trunc_uncached(in);
        break;
      case Cbrt:
        out = js::math_cbrt_uncached(in);
        break;
      case Floor:
        out = js::math_floor_impl(in);
        break;
      case Ceil:
        out = js::math_ceil_impl(in);
        break;
      case Round:
        out = js::math_round_impl(in);
        break;
      default:
        return this;
    }

    if (input->type() == MIRType_Float32)
        return MConstant::NewTypedValue(alloc, DoubleValue(out), MIRType_Float32);
    return MConstant::New(alloc, DoubleValue(out));
}

MParameter*
MParameter::New(TempAllocator& alloc, int32_t index, TemporaryTypeSet* types)
{
    return new(alloc) MParameter(index, types);
}

void
MParameter::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    if (index() == THIS_SLOT)
        fprintf(fp, " THIS_SLOT");
    else
        fprintf(fp, " %d", index());
}

HashNumber
MParameter::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, index_);
    return hash;
}

bool
MParameter::congruentTo(const MDefinition* ins) const
{
    if (!ins->isParameter())
        return false;

    return ins->toParameter()->index() == index_;
}

MCall*
MCall::New(TempAllocator& alloc, JSFunction* target, size_t maxArgc, size_t numActualArgs,
           bool construct, bool isDOMCall)
{
    MOZ_ASSERT(maxArgc >= numActualArgs);
    MCall* ins;
    if (isDOMCall) {
        MOZ_ASSERT(!construct);
        ins = new(alloc) MCallDOMNative(target, numActualArgs);
    } else {
        ins = new(alloc) MCall(target, numActualArgs, construct);
    }
    if (!ins->init(alloc, maxArgc + NumNonArgumentOperands))
        return nullptr;
    return ins;
}

AliasSet
MCallDOMNative::getAliasSet() const
{
    const JSJitInfo* jitInfo = getJitInfo();

    // If we don't know anything about the types of our arguments, we have to
    // assume that type-coercions can have side-effects, so we need to alias
    // everything.
    if (jitInfo->aliasSet() == JSJitInfo::AliasEverything || !jitInfo->isTypedMethodJitInfo())
        return AliasSet::Store(AliasSet::Any);

    uint32_t argIndex = 0;
    const JSTypedMethodJitInfo* methodInfo =
        reinterpret_cast<const JSTypedMethodJitInfo*>(jitInfo);
    for (const JSJitInfo::ArgType* argType = methodInfo->argTypes;
         *argType != JSJitInfo::ArgTypeListEnd;
         ++argType, ++argIndex)
    {
        if (argIndex >= numActualArgs()) {
            // Passing through undefined can't have side-effects
            continue;
        }
        // getArg(0) is "this", so skip it
        MDefinition* arg = getArg(argIndex+1);
        MIRType actualType = arg->type();
        // The only way to reliably avoid side-effects given the informtion we
        // have here is if we're passing in a known primitive value to an
        // argument that expects a primitive value.  XXXbz maybe we need to
        // communicate better information.  For example, a sequence argument
        // will sort of unavoidably have side effects, while a typed array
        // argument won't have any, but both are claimed to be
        // JSJitInfo::Object.
        if ((actualType == MIRType_Value || actualType == MIRType_Object) ||
            (*argType & JSJitInfo::Object))
         {
             return AliasSet::Store(AliasSet::Any);
         }
    }

    // We checked all the args, and they check out.  So we only alias DOM
    // mutations or alias nothing, depending on the alias set in the jitinfo.
    if (jitInfo->aliasSet() == JSJitInfo::AliasNone)
        return AliasSet::None();

    MOZ_ASSERT(jitInfo->aliasSet() == JSJitInfo::AliasDOMSets);
    return AliasSet::Load(AliasSet::DOMProperty);
}

void
MCallDOMNative::computeMovable()
{
    // We are movable if the jitinfo says we can be and if we're also not
    // effectful.  The jitinfo can't check for the latter, since it depends on
    // the types of our arguments.
    const JSJitInfo* jitInfo = getJitInfo();

    MOZ_ASSERT_IF(jitInfo->isMovable,
                  jitInfo->aliasSet() != JSJitInfo::AliasEverything);

    if (jitInfo->isMovable && !isEffectful())
        setMovable();
}

bool
MCallDOMNative::congruentTo(const MDefinition* ins) const
{
    if (!isMovable())
        return false;

    if (!ins->isCall())
        return false;

    const MCall* call = ins->toCall();

    if (!call->isCallDOMNative())
        return false;

    if (getSingleTarget() != call->getSingleTarget())
        return false;

    if (isConstructing() != call->isConstructing())
        return false;

    if (numActualArgs() != call->numActualArgs())
        return false;

    if (needsArgCheck() != call->needsArgCheck())
        return false;

    if (!congruentIfOperandsEqual(call))
        return false;

    // The other call had better be movable at this point!
    MOZ_ASSERT(call->isMovable());

    return true;
}

const JSJitInfo*
MCallDOMNative::getJitInfo() const
{
    MOZ_ASSERT(getSingleTarget() && getSingleTarget()->isNative());

    const JSJitInfo* jitInfo = getSingleTarget()->jitInfo();
    MOZ_ASSERT(jitInfo);

    return jitInfo;
}

MApplyArgs*
MApplyArgs::New(TempAllocator& alloc, JSFunction* target, MDefinition* fun, MDefinition* argc,
                MDefinition* self)
{
    return new(alloc) MApplyArgs(target, fun, argc, self);
}

MDefinition*
MStringLength::foldsTo(TempAllocator& alloc)
{
    if ((type() == MIRType_Int32) && (string()->isConstantValue())) {
        Value value = string()->constantValue();
        JSAtom* atom = &value.toString()->asAtom();
        return MConstant::New(alloc, Int32Value(atom->length()));
    }

    return this;
}

MDefinition*
MConcat::foldsTo(TempAllocator& alloc)
{
    if (lhs()->isConstantValue() && lhs()->constantValue().toString()->empty())
        return rhs();

    if (rhs()->isConstantValue() && rhs()->constantValue().toString()->empty())
        return lhs();

    return this;
}

static bool
EnsureFloatInputOrConvert(MUnaryInstruction* owner, TempAllocator& alloc)
{
    MDefinition* input = owner->input();
    if (!input->canProduceFloat32()) {
        if (input->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, input, owner);
        return false;
    }
    return true;
}

void
MFloor::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType_Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        setPolicyType(MIRType_Float32);
}

void
MCeil::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType_Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        setPolicyType(MIRType_Float32);
}

void
MRound::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType_Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        setPolicyType(MIRType_Float32);
}

MCompare*
MCompare::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, JSOp op)
{
    return new(alloc) MCompare(left, right, op);
}

MCompare*
MCompare::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right, JSOp op,
                   CompareType compareType)
{
    MOZ_ASSERT(compareType == Compare_Int32 || compareType == Compare_UInt32 ||
               compareType == Compare_Double || compareType == Compare_Float32);
    MCompare* comp = new(alloc) MCompare(left, right, op);
    comp->compareType_ = compareType;
    comp->operandMightEmulateUndefined_ = false;
    comp->setResultType(MIRType_Int32);
    return comp;
}

MTableSwitch*
MTableSwitch::New(TempAllocator& alloc, MDefinition* ins, int32_t low, int32_t high)
{
    return new(alloc) MTableSwitch(alloc, ins, low, high);
}

MGoto*
MGoto::New(TempAllocator& alloc, MBasicBlock* target)
{
    MOZ_ASSERT(target);
    return new(alloc) MGoto(target);
}

void
MUnbox::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    fprintf(fp, " ");
    getOperand(0)->printName(fp);
    fprintf(fp, " ");

    switch (type()) {
      case MIRType_Int32: fprintf(fp, "to Int32"); break;
      case MIRType_Double: fprintf(fp, "to Double"); break;
      case MIRType_Boolean: fprintf(fp, "to Boolean"); break;
      case MIRType_String: fprintf(fp, "to String"); break;
      case MIRType_Symbol: fprintf(fp, "to Symbol"); break;
      case MIRType_Object: fprintf(fp, "to Object"); break;
      default: break;
    }

    switch (mode()) {
      case Fallible: fprintf(fp, " (fallible)"); break;
      case Infallible: fprintf(fp, " (infallible)"); break;
      case TypeBarrier: fprintf(fp, " (typebarrier)"); break;
      default: break;
    }
}

void
MTypeBarrier::printOpcode(FILE* fp) const
{
    PrintOpcodeName(fp, op());
    fprintf(fp, " ");
    getOperand(0)->printName(fp);
}

bool
MTypeBarrier::congruentTo(const MDefinition* def) const
{
    if (!def->isTypeBarrier())
        return false;
    const MTypeBarrier* other = def->toTypeBarrier();
    if (barrierKind() != other->barrierKind() || isGuard() != other->isGuard())
        return false;
    if (!resultTypeSet()->equals(other->resultTypeSet()))
        return false;
    return congruentIfOperandsEqual(other);
}

#ifdef DEBUG
void
MPhi::assertLoopPhi() const
{
    // getLoopPredecessorOperand and getLoopBackedgeOperand rely on these
    // predecessors being at indices 0 and 1.
    MBasicBlock* pred = block()->getPredecessor(0);
    MBasicBlock* back = block()->getPredecessor(1);
    MOZ_ASSERT(pred == block()->loopPredecessor());
    MOZ_ASSERT(pred->successorWithPhis() == block());
    MOZ_ASSERT(pred->positionInPhiSuccessor() == 0);
    MOZ_ASSERT(back == block()->backedge());
    MOZ_ASSERT(back->successorWithPhis() == block());
    MOZ_ASSERT(back->positionInPhiSuccessor() == 1);
}
#endif

void
MPhi::removeOperand(size_t index)
{
    MOZ_ASSERT(index < numOperands());
    MOZ_ASSERT(getUseFor(index)->index() == index);
    MOZ_ASSERT(getUseFor(index)->consumer() == this);

    // If we have phi(..., a, b, c, d, ..., z) and we plan
    // on removing a, then first shift downward so that we have
    // phi(..., b, c, d, ..., z, z):
    MUse* p = inputs_.begin() + index;
    MUse* e = inputs_.end();
    p->producer()->removeUse(p);
    for (; p < e - 1; ++p) {
        MDefinition* producer = (p + 1)->producer();
        p->setProducerUnchecked(producer);
        producer->replaceUse(p + 1, p);
    }

    // truncate the inputs_ list:
    inputs_.popBack();
}

void
MPhi::removeAllOperands()
{
    for (MUse* p = inputs_.begin(), *e = inputs_.end(); p < e; ++p)
        p->producer()->removeUse(p);
    inputs_.clear();
}

MDefinition*
MPhi::foldsTernary()
{
    /* Look if this MPhi is a ternary construct.
     * This is a very loose term as it actually only checks for
     *
     *      MTest X
     *       /  \
     *    ...    ...
     *       \  /
     *     MPhi X Y
     *
     * Which we will simply call:
     * x ? x : y or x ? y : x
     */

    if (numOperands() != 2)
        return nullptr;

    MOZ_ASSERT(block()->numPredecessors() == 2);

    MBasicBlock* pred = block()->immediateDominator();
    if (!pred || !pred->lastIns()->isTest())
        return nullptr;

    MTest* test = pred->lastIns()->toTest();

    // True branch may only dominate one edge of MPhi.
    if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
        test->ifTrue()->dominates(block()->getPredecessor(1)))
    {
        return nullptr;
    }

    // False branch may only dominate one edge of MPhi.
    if (test->ifFalse()->dominates(block()->getPredecessor(0)) ==
        test->ifFalse()->dominates(block()->getPredecessor(1)))
    {
        return nullptr;
    }

    // True and false branch must dominate different edges of MPhi.
    if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
        test->ifFalse()->dominates(block()->getPredecessor(0)))
    {
        return nullptr;
    }

    // We found a ternary construct.
    bool firstIsTrueBranch = test->ifTrue()->dominates(block()->getPredecessor(0));
    MDefinition* trueDef = firstIsTrueBranch ? getOperand(0) : getOperand(1);
    MDefinition* falseDef = firstIsTrueBranch ? getOperand(1) : getOperand(0);

    // Accept either
    // testArg ? testArg : constant or
    // testArg ? constant : testArg
    if (!trueDef->isConstant() && !falseDef->isConstant())
        return nullptr;

    MConstant* c = trueDef->isConstant() ? trueDef->toConstant() : falseDef->toConstant();
    MDefinition* testArg = (trueDef == c) ? falseDef : trueDef;
    if (testArg != test->input())
        return nullptr;

    // This check should be a tautology, except that the constant might be the
    // result of the removal of a branch.  In such case the domination scope of
    // the block which is holding the constant might be incomplete. This
    // condition is used to prevent doing this optimization based on incomplete
    // information.
    //
    // As GVN removed a branch, it will update the dominations rules before
    // trying to fold this MPhi again. Thus, this condition does not inhibit
    // this optimization.
    MBasicBlock* truePred = block()->getPredecessor(firstIsTrueBranch ? 0 : 1);
    MBasicBlock* falsePred = block()->getPredecessor(firstIsTrueBranch ? 1 : 0);
    if (!trueDef->block()->dominates(truePred) ||
        !falseDef->block()->dominates(falsePred))
    {
        return nullptr;
    }

    // If testArg is an int32 type we can:
    // - fold testArg ? testArg : 0 to testArg
    // - fold testArg ? 0 : testArg to 0
    if (testArg->type() == MIRType_Int32 && c->vp()->toNumber() == 0) {
        // When folding to the constant we need to hoist it.
        if (trueDef == c && !c->block()->dominates(block()))
            c->block()->moveBefore(pred->lastIns(), c);
        return trueDef;
    }

    // If testArg is a string type we can:
    // - fold testArg ? testArg : "" to testArg
    // - fold testArg ? "" : testArg to ""
    if (testArg->type() == MIRType_String &&
        c->vp()->toString() == GetJitContext()->runtime->emptyString())
    {
        // When folding to the constant we need to hoist it.
        if (trueDef == c && !c->block()->dominates(block()))
            c->block()->moveBefore(pred->lastIns(), c);
        return trueDef;
    }

    return nullptr;
}

MDefinition*
MPhi::operandIfRedundant()
{
    if (inputs_.length() == 0)
        return nullptr;

    // If this phi is redundant (e.g., phi(a,a) or b=phi(a,this)),
    // returns the operand that it will always be equal to (a, in
    // those two cases).
    MDefinition* first = getOperand(0);
    for (size_t i = 1, e = numOperands(); i < e; i++) {
        MDefinition* op = getOperand(i);
        if (op != first && op != this)
            return nullptr;
    }
    return first;
}

MDefinition*
MPhi::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = operandIfRedundant())
        return def;

    if (MDefinition* def = foldsTernary())
        return def;

    return this;
}

bool
MPhi::congruentTo(const MDefinition* ins) const
{
    if (!ins->isPhi())
        return false;

    // Phis in different blocks may have different control conditions.
    // For example, these phis:
    //
    //   if (p)
    //     goto a
    //   a:
    //     t = phi(x, y)
    //
    //   if (q)
    //     goto b
    //   b:
    //     s = phi(x, y)
    //
    // have identical operands, but they are not equvalent because t is
    // effectively p?x:y and s is effectively q?x:y.
    //
    // For now, consider phis in different blocks incongruent.
    if (ins->block() != block())
        return false;

    return congruentIfOperandsEqual(ins);
}

static inline TemporaryTypeSet*
MakeMIRTypeSet(MIRType type)
{
    MOZ_ASSERT(type != MIRType_Value);
    TypeSet::Type ntype = type == MIRType_Object
                          ? TypeSet::AnyObjectType()
                          : TypeSet::PrimitiveType(ValueTypeFromMIRType(type));
    LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
    return alloc->new_<TemporaryTypeSet>(alloc, ntype);
}

bool
jit::MergeTypes(MIRType* ptype, TemporaryTypeSet** ptypeSet,
                MIRType newType, TemporaryTypeSet* newTypeSet)
{
    if (newTypeSet && newTypeSet->empty())
        return true;
    if (newType != *ptype) {
        if (IsNumberType(newType) && IsNumberType(*ptype)) {
            *ptype = MIRType_Double;
        } else if (*ptype != MIRType_Value) {
            if (!*ptypeSet) {
                *ptypeSet = MakeMIRTypeSet(*ptype);
                if (!*ptypeSet)
                    return false;
            }
            *ptype = MIRType_Value;
        } else if (*ptypeSet && (*ptypeSet)->empty()) {
            *ptype = newType;
        }
    }
    if (*ptypeSet) {
        LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
        if (!newTypeSet && newType != MIRType_Value) {
            newTypeSet = MakeMIRTypeSet(newType);
            if (!newTypeSet)
                return false;
        }
        if (newTypeSet) {
            if (!newTypeSet->isSubset(*ptypeSet))
                *ptypeSet = TypeSet::unionSets(*ptypeSet, newTypeSet, alloc);
        } else {
            *ptypeSet = nullptr;
        }
    }
    return true;
}

bool
MPhi::specializeType()
{
#ifdef DEBUG
    MOZ_ASSERT(!specialized_);
    specialized_ = true;
#endif

    MOZ_ASSERT(!inputs_.empty());

    size_t start;
    if (hasBackedgeType_) {
        // The type of this phi has already been populated with potential types
        // that could come in via loop backedges.
        start = 0;
    } else {
        setResultType(getOperand(0)->type());
        setResultTypeSet(getOperand(0)->resultTypeSet());
        start = 1;
    }

    MIRType resultType = this->type();
    TemporaryTypeSet* resultTypeSet = this->resultTypeSet();

    for (size_t i = start; i < inputs_.length(); i++) {
        MDefinition* def = getOperand(i);
        if (!MergeTypes(&resultType, &resultTypeSet, def->type(), def->resultTypeSet()))
            return false;
    }

    setResultType(resultType);
    setResultTypeSet(resultTypeSet);
    return true;
}

bool
MPhi::addBackedgeType(MIRType type, TemporaryTypeSet* typeSet)
{
    MOZ_ASSERT(!specialized_);

    if (hasBackedgeType_) {
        MIRType resultType = this->type();
        TemporaryTypeSet* resultTypeSet = this->resultTypeSet();

        if (!MergeTypes(&resultType, &resultTypeSet, type, typeSet))
            return false;

        setResultType(resultType);
        setResultTypeSet(resultTypeSet);
    } else {
        setResultType(type);
        setResultTypeSet(typeSet);
        hasBackedgeType_ = true;
    }
    return true;
}

bool
MPhi::typeIncludes(MDefinition* def)
{
    if (def->type() == MIRType_Int32 && this->type() == MIRType_Double)
        return true;

    if (TemporaryTypeSet* types = def->resultTypeSet()) {
        if (this->resultTypeSet())
            return types->isSubset(this->resultTypeSet());
        if (this->type() == MIRType_Value || types->empty())
            return true;
        return this->type() == types->getKnownMIRType();
    }

    if (def->type() == MIRType_Value) {
        // This phi must be able to be any value.
        return this->type() == MIRType_Value
            && (!this->resultTypeSet() || this->resultTypeSet()->unknown());
    }

    return this->mightBeType(def->type());
}

bool
MPhi::checkForTypeChange(MDefinition* ins, bool* ptypeChange)
{
    MIRType resultType = this->type();
    TemporaryTypeSet* resultTypeSet = this->resultTypeSet();

    if (!MergeTypes(&resultType, &resultTypeSet, ins->type(), ins->resultTypeSet()))
        return false;

    if (resultType != this->type() || resultTypeSet != this->resultTypeSet()) {
        *ptypeChange = true;
        setResultType(resultType);
        setResultTypeSet(resultTypeSet);
    }
    return true;
}

void
MCall::addArg(size_t argnum, MDefinition* arg)
{
    // The operand vector is initialized in reverse order by the IonBuilder.
    // It cannot be checked for consistency until all arguments are added.
    // FixedList doesn't initialize its elements, so do an unchecked init.
    initOperand(argnum + NumNonArgumentOperands, arg);
}

void
MBitNot::infer()
{
    if (getOperand(0)->mightBeType(MIRType_Object) || getOperand(0)->mightBeType(MIRType_Symbol))
        specialization_ = MIRType_None;
    else
        specialization_ = MIRType_Int32;
}

static inline bool
IsConstant(MDefinition* def, double v)
{
    if (!def->isConstant())
        return false;

    return NumbersAreIdentical(def->toConstant()->value().toNumber(), v);
}

MDefinition*
MBinaryBitwiseInstruction::foldsTo(TempAllocator& alloc)
{
    if (specialization_ != MIRType_Int32)
        return this;

    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    return this;
}

MDefinition*
MBinaryBitwiseInstruction::foldUnnecessaryBitop()
{
    if (specialization_ != MIRType_Int32)
        return this;

    // Eliminate bitwise operations that are no-ops when used on integer
    // inputs, such as (x | 0).

    MDefinition* lhs = getOperand(0);
    MDefinition* rhs = getOperand(1);

    if (IsConstant(lhs, 0))
        return foldIfZero(0);

    if (IsConstant(rhs, 0))
        return foldIfZero(1);

    if (IsConstant(lhs, -1))
        return foldIfNegOne(0);

    if (IsConstant(rhs, -1))
        return foldIfNegOne(1);

    if (lhs == rhs)
        return foldIfEqual();

    return this;
}

void
MBinaryBitwiseInstruction::infer(BaselineInspector*, jsbytecode*)
{
    if (getOperand(0)->mightBeType(MIRType_Object) || getOperand(0)->mightBeType(MIRType_Symbol) ||
        getOperand(1)->mightBeType(MIRType_Object) || getOperand(1)->mightBeType(MIRType_Symbol))
    {
        specialization_ = MIRType_None;
    } else {
        specializeAsInt32();
    }
}

void
MBinaryBitwiseInstruction::specializeAsInt32()
{
    specialization_ = MIRType_Int32;
    MOZ_ASSERT(type() == MIRType_Int32);

    if (isBitOr() || isBitAnd() || isBitXor())
        setCommutative();
}

void
MShiftInstruction::infer(BaselineInspector*, jsbytecode*)
{
    if (getOperand(0)->mightBeType(MIRType_Object) || getOperand(1)->mightBeType(MIRType_Object) ||
        getOperand(0)->mightBeType(MIRType_Symbol) || getOperand(1)->mightBeType(MIRType_Symbol))
        specialization_ = MIRType_None;
    else
        specialization_ = MIRType_Int32;
}

void
MUrsh::infer(BaselineInspector* inspector, jsbytecode* pc)
{
    if (getOperand(0)->mightBeType(MIRType_Object) || getOperand(1)->mightBeType(MIRType_Object) ||
        getOperand(0)->mightBeType(MIRType_Symbol) || getOperand(1)->mightBeType(MIRType_Symbol))
    {
        specialization_ = MIRType_None;
        setResultType(MIRType_Value);
        return;
    }

    if (inspector->hasSeenDoubleResult(pc)) {
        specialization_ = MIRType_Double;
        setResultType(MIRType_Double);
        return;
    }

    specialization_ = MIRType_Int32;
    setResultType(MIRType_Int32);
}

static inline bool
CanProduceNegativeZero(MDefinition* def) {
    // Test if this instruction can produce negative zero even when bailing out
    // and changing types.
    switch (def->op()) {
        case MDefinition::Op_Constant:
            if (def->type() == MIRType_Double && def->constantValue().toDouble() == -0.0)
                return true;
        case MDefinition::Op_BitAnd:
        case MDefinition::Op_BitOr:
        case MDefinition::Op_BitXor:
        case MDefinition::Op_BitNot:
        case MDefinition::Op_Lsh:
        case MDefinition::Op_Rsh:
            return false;
        default:
            return true;
    }
}

static inline bool
NeedNegativeZeroCheck(MDefinition* def)
{
    // Test if all uses have the same semantics for -0 and 0
    for (MUseIterator use = def->usesBegin(); use != def->usesEnd(); use++) {
        if (use->consumer()->isResumePoint())
            continue;

        MDefinition* use_def = use->consumer()->toDefinition();
        switch (use_def->op()) {
          case MDefinition::Op_Add: {
            // If add is truncating -0 and 0 are observed as the same.
            if (use_def->toAdd()->isTruncated())
                break;

            // x + y gives -0, when both x and y are -0

            // Figure out the order in which the addition's operands will
            // execute. EdgeCaseAnalysis::analyzeLate has renumbered the MIR
            // definitions for us so that this just requires comparing ids.
            MDefinition* first = use_def->toAdd()->getOperand(0);
            MDefinition* second = use_def->toAdd()->getOperand(1);
            if (first->id() > second->id()) {
                MDefinition* temp = first;
                first = second;
                second = temp;
            }

            // Negative zero checks can be removed on the first executed
            // operand only if it is guaranteed the second executed operand
            // will produce a value other than -0. While the second is
            // typed as an int32, a bailout taken between execution of the
            // operands may change that type and cause a -0 to flow to the
            // second.
            //
            // There is no way to test whether there are any bailouts
            // between execution of the operands, so remove negative
            // zero checks from the first only if the second's type is
            // independent from type changes that may occur after bailing.
            if (def == first && CanProduceNegativeZero(second))
                return true;

            // The negative zero check can always be removed on the second
            // executed operand; by the time this executes the first will have
            // been evaluated as int32 and the addition's result cannot be -0.
            break;
          }
          case MDefinition::Op_Sub: {
            // If sub is truncating -0 and 0 are observed as the same
            if (use_def->toSub()->isTruncated())
                break;

            // x + y gives -0, when x is -0 and y is 0

            // We can remove the negative zero check on the rhs, only if we
            // are sure the lhs isn't negative zero.

            // The lhs is typed as integer (i.e. not -0.0), but it can bailout
            // and change type. This should be fine if the lhs is executed
            // first. However if the rhs is executed first, the lhs can bail,
            // change type and become -0.0 while the rhs has already been
            // optimized to not make a difference between zero and negative zero.
            MDefinition* lhs = use_def->toSub()->lhs();
            MDefinition* rhs = use_def->toSub()->rhs();
            if (rhs->id() < lhs->id() && CanProduceNegativeZero(lhs))
                return true;

            /* Fall through...  */
          }
          case MDefinition::Op_StoreElement:
          case MDefinition::Op_StoreElementHole:
          case MDefinition::Op_LoadElement:
          case MDefinition::Op_LoadElementHole:
          case MDefinition::Op_LoadTypedArrayElement:
          case MDefinition::Op_LoadTypedArrayElementHole:
          case MDefinition::Op_CharCodeAt:
          case MDefinition::Op_Mod:
            // Only allowed to remove check when definition is the second operand
            if (use_def->getOperand(0) == def)
                return true;
            for (size_t i = 2, e = use_def->numOperands(); i < e; i++) {
                if (use_def->getOperand(i) == def)
                    return true;
            }
            break;
          case MDefinition::Op_BoundsCheck:
            // Only allowed to remove check when definition is the first operand
            if (use_def->toBoundsCheck()->getOperand(1) == def)
                return true;
            break;
          case MDefinition::Op_ToString:
          case MDefinition::Op_FromCharCode:
          case MDefinition::Op_TableSwitch:
          case MDefinition::Op_Compare:
          case MDefinition::Op_BitAnd:
          case MDefinition::Op_BitOr:
          case MDefinition::Op_BitXor:
          case MDefinition::Op_Abs:
          case MDefinition::Op_TruncateToInt32:
            // Always allowed to remove check. No matter which operand.
            break;
          default:
            return true;
        }
    }
    return false;
}

MDefinition*
MBinaryArithInstruction::foldsTo(TempAllocator& alloc)
{
    if (specialization_ == MIRType_None)
        return this;

    MDefinition* lhs = getOperand(0);
    MDefinition* rhs = getOperand(1);
    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    // 0 + -0 = 0. So we can't remove addition
    if (isAdd() && specialization_ != MIRType_Int32)
        return this;

    if (IsConstant(rhs, getIdentity()))
        return lhs;

    // subtraction isn't commutative. So we can't remove subtraction when lhs equals 0
    if (isSub())
        return this;

    if (IsConstant(lhs, getIdentity()))
        return rhs; // x op id => x

    return this;
}

void
MBinaryArithInstruction::trySpecializeFloat32(TempAllocator& alloc)
{
    // Do not use Float32 if we can use int32.
    if (specialization_ == MIRType_Int32)
        return;

    MDefinition* left = lhs();
    MDefinition* right = rhs();

    if (!left->canProduceFloat32() || !right->canProduceFloat32() ||
        !CheckUsesAreFloat32Consumers(this))
    {
        if (left->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, left, this);
        if (right->type() == MIRType_Float32)
            ConvertDefinitionToDouble<1>(alloc, right, this);
        return;
    }

    specialization_ = MIRType_Float32;
    setResultType(MIRType_Float32);
}

void
MMinMax::trySpecializeFloat32(TempAllocator& alloc)
{
    if (specialization_ == MIRType_Int32)
        return;

    MDefinition* left = lhs();
    MDefinition* right = rhs();

    if (!(left->canProduceFloat32() || (left->isMinMax() && left->type() == MIRType_Float32)) ||
        !(right->canProduceFloat32() || (right->isMinMax() && right->type() == MIRType_Float32)))
    {
        if (left->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, left, this);
        if (right->type() == MIRType_Float32)
            ConvertDefinitionToDouble<1>(alloc, right, this);
        return;
    }

    specialization_ = MIRType_Float32;
    setResultType(MIRType_Float32);
}

MDefinition*
MMinMax::foldsTo(TempAllocator& alloc)
{
    if (!lhs()->isConstant() && !rhs()->isConstant())
        return this;

    // Directly apply math utility to compare the rhs() and lhs() when
    // they are both constants.
    if (lhs()->isConstant() && rhs()->isConstant()) {
        Value lval = lhs()->toConstant()->value();
        Value rval = rhs()->toConstant()->value();
        if (!lval.isNumber() || !rval.isNumber())
            return this;

        double lnum = lval.toNumber();
        double rnum = rval.toNumber();
        double result;
        if (isMax())
            result = js::math_max_impl(lnum, rnum);
        else
            result = js::math_min_impl(lnum, rnum);

        // The folded MConstant should maintain the same MIRType with
        // the original MMinMax.
        if (type() == MIRType_Int32) {
            int32_t cast;
            if (mozilla::NumberEqualsInt32(result, &cast))
                return MConstant::New(alloc, Int32Value(cast));
        } else {
            MOZ_ASSERT(IsFloatingPointType(type()));
            MConstant* constant = MConstant::New(alloc, DoubleValue(result));
            if (type() == MIRType_Float32)
                constant->setResultType(MIRType_Float32);
            return constant;
        }
    }

    MDefinition* operand = lhs()->isConstantValue() ? rhs() : lhs();
    const js::Value& val = lhs()->isConstantValue() ? lhs()->constantValue() : rhs()->constantValue();

    if (operand->isToDouble() && operand->getOperand(0)->type() == MIRType_Int32) {
        // min(int32, cte >= INT32_MAX) = int32
        if (val.isDouble() && val.toDouble() >= INT32_MAX && !isMax()) {
            MLimitedTruncate* limit =
                MLimitedTruncate::New(alloc, operand->getOperand(0), MDefinition::NoTruncate);
            block()->insertBefore(this, limit);
            MToDouble* toDouble = MToDouble::New(alloc, limit);
            block()->insertBefore(this, toDouble);
            return toDouble;
        }

        // max(int32, cte <= INT32_MIN) = int32
        if (val.isDouble() && val.toDouble() <= INT32_MIN && isMax()) {
            MLimitedTruncate* limit =
                MLimitedTruncate::New(alloc, operand->getOperand(0), MDefinition::NoTruncate);
            block()->insertBefore(this, limit);
            MToDouble* toDouble = MToDouble::New(alloc, limit);
            block()->insertBefore(this, toDouble);
            return toDouble;
        }
    }
    return this;
}

bool
MAbs::fallible() const
{
    return !implicitTruncate_ && (!range() || !range()->hasInt32Bounds());
}

void
MAbs::trySpecializeFloat32(TempAllocator& alloc)
{
    // Do not use Float32 if we can use int32.
    if (input()->type() == MIRType_Int32)
        return;

    if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
        if (input()->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType_Float32);
    specialization_ = MIRType_Float32;
}

MDefinition*
MDiv::foldsTo(TempAllocator& alloc)
{
    if (specialization_ == MIRType_None)
        return this;

    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    if (MDefinition* folded = EvaluateExactReciprocal(alloc, this))
        return folded;

    return this;
}

void
MDiv::analyzeEdgeCasesForward()
{
    // This is only meaningful when doing integer division.
    if (specialization_ != MIRType_Int32)
        return;

    // Try removing divide by zero check
    if (rhs()->isConstantValue() && !rhs()->constantValue().isInt32(0))
        canBeDivideByZero_ = false;

    // If lhs is a constant int != INT32_MIN, then
    // negative overflow check can be skipped.
    if (lhs()->isConstantValue() && !lhs()->constantValue().isInt32(INT32_MIN))
        canBeNegativeOverflow_ = false;

    // If rhs is a constant int != -1, likewise.
    if (rhs()->isConstantValue() && !rhs()->constantValue().isInt32(-1))
        canBeNegativeOverflow_ = false;

    // If lhs is != 0, then negative zero check can be skipped.
    if (lhs()->isConstantValue() && !lhs()->constantValue().isInt32(0))
        setCanBeNegativeZero(false);

    // If rhs is >= 0, likewise.
    if (rhs()->isConstantValue()) {
        const js::Value& val = rhs()->constantValue();
        if (val.isInt32() && val.toInt32() >= 0)
            setCanBeNegativeZero(false);
    }
}

void
MDiv::analyzeEdgeCasesBackward()
{
    if (canBeNegativeZero() && !NeedNegativeZeroCheck(this))
        setCanBeNegativeZero(false);
}

bool
MDiv::fallible() const
{
    return !isTruncated();
}

MDefinition*
MMod::foldsTo(TempAllocator& alloc)
{
    if (specialization_ == MIRType_None)
        return this;

    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    return this;
}

void
MMod::analyzeEdgeCasesForward()
{
    // These optimizations make sense only for integer division
    if (specialization_ != MIRType_Int32)
        return;

    if (rhs()->isConstantValue() && !rhs()->constantValue().isInt32(0))
        canBeDivideByZero_ = false;

    if (rhs()->isConstantValue()) {
        int32_t n = rhs()->constantValue().toInt32();
        if (n > 0 && !IsPowerOfTwo(n))
            canBePowerOfTwoDivisor_ = false;
    }
}

bool
MMod::fallible() const
{
    return !isTruncated() &&
           (isUnsigned() || canBeDivideByZero() || canBeNegativeDividend());
}

void
MMathFunction::trySpecializeFloat32(TempAllocator& alloc)
{
    if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
        if (input()->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType_Float32);
    setPolicyType(MIRType_Float32);
}

MHypot* MHypot::New(TempAllocator& alloc, const MDefinitionVector & vector)
{
    uint32_t length = vector.length();
    MHypot * hypot = new(alloc) MHypot;
    if (!hypot->init(alloc, length))
        return nullptr;

    for (uint32_t i = 0; i < length; ++i)
        hypot->initOperand(i, vector[i]);
    return hypot;
}

bool
MAdd::fallible() const
{
    // the add is fallible if range analysis does not say that it is finite, AND
    // either the truncation analysis shows that there are non-truncated uses.
    if (truncateKind() >= IndirectTruncate)
        return false;
    if (range() && range()->hasInt32Bounds())
        return false;
    return true;
}

bool
MSub::fallible() const
{
    // see comment in MAdd::fallible()
    if (truncateKind() >= IndirectTruncate)
        return false;
    if (range() && range()->hasInt32Bounds())
        return false;
    return true;
}

MDefinition*
MMul::foldsTo(TempAllocator& alloc)
{
    MDefinition* out = MBinaryArithInstruction::foldsTo(alloc);
    if (out != this)
        return out;

    if (specialization() != MIRType_Int32)
        return this;

    if (lhs() == rhs())
        setCanBeNegativeZero(false);

    return this;
}

void
MMul::analyzeEdgeCasesForward()
{
    // Try to remove the check for negative zero
    // This only makes sense when using the integer multiplication
    if (specialization() != MIRType_Int32)
        return;

    // If lhs is > 0, no need for negative zero check.
    if (lhs()->isConstantValue()) {
        const js::Value& val = lhs()->constantValue();
        if (val.isInt32() && val.toInt32() > 0)
            setCanBeNegativeZero(false);
    }

    // If rhs is > 0, likewise.
    if (rhs()->isConstantValue()) {
        const js::Value& val = rhs()->constantValue();
        if (val.isInt32() && val.toInt32() > 0)
            setCanBeNegativeZero(false);
    }
}

void
MMul::analyzeEdgeCasesBackward()
{
    if (canBeNegativeZero() && !NeedNegativeZeroCheck(this))
        setCanBeNegativeZero(false);
}

bool
MMul::updateForReplacement(MDefinition* ins_)
{
    MMul* ins = ins_->toMul();
    bool negativeZero = canBeNegativeZero() || ins->canBeNegativeZero();
    setCanBeNegativeZero(negativeZero);
    // Remove the imul annotation when merging imul and normal multiplication.
    if (mode_ == Integer && ins->mode() != Integer)
        mode_ = Normal;
    return true;
}

bool
MMul::canOverflow() const
{
    if (isTruncated())
        return false;
    return !range() || !range()->hasInt32Bounds();
}

bool
MUrsh::fallible() const
{
    if (bailoutsDisabled())
        return false;
    return !range() || !range()->hasInt32Bounds();
}

static inline bool
SimpleArithOperand(MDefinition* op)
{
    return !op->mightBeType(MIRType_Object)
        && !op->mightBeType(MIRType_String)
        && !op->mightBeType(MIRType_Symbol)
        && !op->mightBeType(MIRType_MagicOptimizedArguments)
        && !op->mightBeType(MIRType_MagicHole)
        && !op->mightBeType(MIRType_MagicIsConstructing);
}

void
MBinaryArithInstruction::infer(TempAllocator& alloc, BaselineInspector* inspector, jsbytecode* pc)
{
    MOZ_ASSERT(this->type() == MIRType_Value);

    specialization_ = MIRType_None;

    // Anything complex - strings, symbols, and objects - are not specialized
    // unless baseline type hints suggest it might be profitable
    if (!SimpleArithOperand(getOperand(0)) || !SimpleArithOperand(getOperand(1)))
        return inferFallback(inspector, pc);

    // Retrieve type information of lhs and rhs.
    MIRType lhs = getOperand(0)->type();
    MIRType rhs = getOperand(1)->type();

    // Guess a result type based on the inputs.
    // Don't specialize for neither-integer-nor-double results.
    if (lhs == MIRType_Int32 && rhs == MIRType_Int32)
        setResultType(MIRType_Int32);
    // Double operations are prioritary over float32 operations (i.e. if any operand needs
    // a double as an input, convert all operands to doubles)
    else if (IsFloatingPointType(lhs) || IsFloatingPointType(rhs))
        setResultType(MIRType_Double);
    else
        return inferFallback(inspector, pc);

    // If the operation has ever overflowed, use a double specialization.
    if (inspector->hasSeenDoubleResult(pc))
        setResultType(MIRType_Double);

    // If the operation will always overflow on its constant operands, use a
    // double specialization so that it can be constant folded later.
    if ((isMul() || isDiv()) && lhs == MIRType_Int32 && rhs == MIRType_Int32) {
        bool typeChange = false;
        EvaluateConstantOperands(alloc, this, &typeChange);
        if (typeChange)
            setResultType(MIRType_Double);
    }

    MOZ_ASSERT(lhs < MIRType_String || lhs == MIRType_Value);
    MOZ_ASSERT(rhs < MIRType_String || rhs == MIRType_Value);

    MIRType rval = this->type();

    // Don't specialize values when result isn't double
    if (lhs == MIRType_Value || rhs == MIRType_Value) {
        if (!IsFloatingPointType(rval)) {
            specialization_ = MIRType_None;
            return;
        }
    }

    // Don't specialize as int32 if one of the operands is undefined,
    // since ToNumber(undefined) is NaN.
    if (rval == MIRType_Int32 && (lhs == MIRType_Undefined || rhs == MIRType_Undefined)) {
        specialization_ = MIRType_None;
        return;
    }

    specialization_ = rval;

    if (isAdd() || isMul())
        setCommutative();
    setResultType(rval);
}

void
MBinaryArithInstruction::inferFallback(BaselineInspector* inspector,
                                       jsbytecode* pc)
{
    // Try to specialize based on what baseline observed in practice.
    specialization_ = inspector->expectedBinaryArithSpecialization(pc);
    if (specialization_ != MIRType_None) {
        setResultType(specialization_);
        return;
    }

    // If we can't specialize because we have no type information at all for
    // the lhs or rhs, mark the binary instruction as having no possible types
    // either to avoid degrading subsequent analysis.
    if (getOperand(0)->emptyResultTypeSet() || getOperand(1)->emptyResultTypeSet()) {
        LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
        TemporaryTypeSet* types = alloc->new_<TemporaryTypeSet>();
        if (types)
            setResultTypeSet(types);
    }
}

static bool
SafelyCoercesToDouble(MDefinition* op)
{
    // Strings and symbols are unhandled -- visitToDouble() doesn't support them yet.
    // Null is unhandled -- ToDouble(null) == 0, but (0 == null) is false.
    return SimpleArithOperand(op) && !op->mightBeType(MIRType_Null);
}

static bool
ObjectOrSimplePrimitive(MDefinition* op)
{
    // Return true if op is either undefined/null/boolean/int32 or an object.
    return !op->mightBeType(MIRType_String)
        && !op->mightBeType(MIRType_Symbol)
        && !op->mightBeType(MIRType_Double)
        && !op->mightBeType(MIRType_Float32)
        && !op->mightBeType(MIRType_MagicOptimizedArguments)
        && !op->mightBeType(MIRType_MagicHole)
        && !op->mightBeType(MIRType_MagicIsConstructing);
}

static bool
CanDoValueBitwiseCmp(CompilerConstraintList* constraints,
                     MDefinition* lhs, MDefinition* rhs, bool looseEq)
{
    // Only primitive (not double/string) or objects are supported.
    // I.e. Undefined/Null/Boolean/Int32 and Object
    if (!ObjectOrSimplePrimitive(lhs) || !ObjectOrSimplePrimitive(rhs))
        return false;

    // Objects that emulate undefined are not supported.
    if (MaybeEmulatesUndefined(constraints, lhs) || MaybeEmulatesUndefined(constraints, rhs))
        return false;

    // In the loose comparison more values could be the same,
    // but value comparison reporting otherwise.
    if (looseEq) {

        // Undefined compared loosy to Null is not supported,
        // because tag is different, but value can be the same (undefined == null).
        if ((lhs->mightBeType(MIRType_Undefined) && rhs->mightBeType(MIRType_Null)) ||
            (lhs->mightBeType(MIRType_Null) && rhs->mightBeType(MIRType_Undefined)))
        {
            return false;
        }

        // Int32 compared loosy to Boolean is not supported,
        // because tag is different, but value can be the same (1 == true).
        if ((lhs->mightBeType(MIRType_Int32) && rhs->mightBeType(MIRType_Boolean)) ||
            (lhs->mightBeType(MIRType_Boolean) && rhs->mightBeType(MIRType_Int32)))
        {
            return false;
        }

        // For loosy comparison of an object with a Boolean/Number/String
        // the valueOf the object is taken. Therefore not supported.
        bool simpleLHS = lhs->mightBeType(MIRType_Boolean) || lhs->mightBeType(MIRType_Int32);
        bool simpleRHS = rhs->mightBeType(MIRType_Boolean) || rhs->mightBeType(MIRType_Int32);
        if ((lhs->mightBeType(MIRType_Object) && simpleRHS) ||
            (rhs->mightBeType(MIRType_Object) && simpleLHS))
        {
            return false;
        }
    }

    return true;
}

MIRType
MCompare::inputType()
{
    switch(compareType_) {
      case Compare_Undefined:
        return MIRType_Undefined;
      case Compare_Null:
        return MIRType_Null;
      case Compare_Boolean:
        return MIRType_Boolean;
      case Compare_UInt32:
      case Compare_Int32:
      case Compare_Int32MaybeCoerceBoth:
      case Compare_Int32MaybeCoerceLHS:
      case Compare_Int32MaybeCoerceRHS:
        return MIRType_Int32;
      case Compare_Double:
      case Compare_DoubleMaybeCoerceLHS:
      case Compare_DoubleMaybeCoerceRHS:
        return MIRType_Double;
      case Compare_Float32:
        return MIRType_Float32;
      case Compare_String:
      case Compare_StrictString:
        return MIRType_String;
      case Compare_Object:
        return MIRType_Object;
      case Compare_Unknown:
      case Compare_Value:
        return MIRType_Value;
      default:
        MOZ_CRASH("No known conversion");
    }
}

static inline bool
MustBeUInt32(MDefinition* def, MDefinition** pwrapped)
{
    if (def->isUrsh()) {
        *pwrapped = def->toUrsh()->getOperand(0);
        MDefinition* rhs = def->toUrsh()->getOperand(1);
        return !def->toUrsh()->bailoutsDisabled()
            && rhs->isConstantValue()
            && rhs->constantValue().isInt32()
            && rhs->constantValue().toInt32() == 0;
    }

    if (def->isConstantValue()) {
        *pwrapped = def;
        return def->constantValue().isInt32()
            && def->constantValue().toInt32() >= 0;
    }

    return false;
}

bool
MBinaryInstruction::tryUseUnsignedOperands()
{
    MDefinition* newlhs, *newrhs;
    if (MustBeUInt32(getOperand(0), &newlhs) && MustBeUInt32(getOperand(1), &newrhs)) {
        if (newlhs->type() != MIRType_Int32 || newrhs->type() != MIRType_Int32)
            return false;
        if (newlhs != getOperand(0)) {
            getOperand(0)->setImplicitlyUsedUnchecked();
            replaceOperand(0, newlhs);
        }
        if (newrhs != getOperand(1)) {
            getOperand(1)->setImplicitlyUsedUnchecked();
            replaceOperand(1, newrhs);
        }
        return true;
    }
    return false;
}

void
MCompare::infer(CompilerConstraintList* constraints, BaselineInspector* inspector, jsbytecode* pc)
{
    MOZ_ASSERT(operandMightEmulateUndefined());

    if (!MaybeEmulatesUndefined(constraints, getOperand(0)) &&
        !MaybeEmulatesUndefined(constraints, getOperand(1)))
    {
        markNoOperandEmulatesUndefined();
    }

    MIRType lhs = getOperand(0)->type();
    MIRType rhs = getOperand(1)->type();

    bool looseEq = jsop() == JSOP_EQ || jsop() == JSOP_NE;
    bool strictEq = jsop() == JSOP_STRICTEQ || jsop() == JSOP_STRICTNE;
    bool relationalEq = !(looseEq || strictEq);

    // Comparisons on unsigned integers may be treated as UInt32.
    if (tryUseUnsignedOperands()) {
        compareType_ = Compare_UInt32;
        return;
    }

    // Integer to integer or boolean to boolean comparisons may be treated as Int32.
    if ((lhs == MIRType_Int32 && rhs == MIRType_Int32) ||
        (lhs == MIRType_Boolean && rhs == MIRType_Boolean))
    {
        compareType_ = Compare_Int32MaybeCoerceBoth;
        return;
    }

    // Loose/relational cross-integer/boolean comparisons may be treated as Int32.
    if (!strictEq &&
        (lhs == MIRType_Int32 || lhs == MIRType_Boolean) &&
        (rhs == MIRType_Int32 || rhs == MIRType_Boolean))
    {
        compareType_ = Compare_Int32MaybeCoerceBoth;
        return;
    }

    // Numeric comparisons against a double coerce to double.
    if (IsNumberType(lhs) && IsNumberType(rhs)) {
        compareType_ = Compare_Double;
        return;
    }

    // Any comparison is allowed except strict eq.
    if (!strictEq && IsFloatingPointType(lhs) && SafelyCoercesToDouble(getOperand(1))) {
        compareType_ = Compare_DoubleMaybeCoerceRHS;
        return;
    }
    if (!strictEq && IsFloatingPointType(rhs) && SafelyCoercesToDouble(getOperand(0))) {
        compareType_ = Compare_DoubleMaybeCoerceLHS;
        return;
    }

    // Handle object comparison.
    if (!relationalEq && lhs == MIRType_Object && rhs == MIRType_Object) {
        compareType_ = Compare_Object;
        return;
    }

    // Handle string comparisons. (Relational string compares are still unsupported).
    if (!relationalEq && lhs == MIRType_String && rhs == MIRType_String) {
        compareType_ = Compare_String;
        return;
    }

    if (strictEq && lhs == MIRType_String) {
        // Lowering expects the rhs to be definitly string.
        compareType_ = Compare_StrictString;
        swapOperands();
        return;
    }

    if (strictEq && rhs == MIRType_String) {
        compareType_ = Compare_StrictString;
        return;
    }

    // Handle compare with lhs being Undefined or Null.
    if (!relationalEq && IsNullOrUndefined(lhs)) {
        // Lowering expects the rhs to be null/undefined, so we have to
        // swap the operands. This is necessary since we may not know which
        // operand was null/undefined during lowering (both operands may have
        // MIRType_Value).
        compareType_ = (lhs == MIRType_Null) ? Compare_Null : Compare_Undefined;
        swapOperands();
        return;
    }

    // Handle compare with rhs being Undefined or Null.
    if (!relationalEq && IsNullOrUndefined(rhs)) {
        compareType_ = (rhs == MIRType_Null) ? Compare_Null : Compare_Undefined;
        return;
    }

    // Handle strict comparison with lhs/rhs being typed Boolean.
    if (strictEq && (lhs == MIRType_Boolean || rhs == MIRType_Boolean)) {
        // bool/bool case got an int32 specialization earlier.
        MOZ_ASSERT(!(lhs == MIRType_Boolean && rhs == MIRType_Boolean));

        // Ensure the boolean is on the right so that the type policy knows
        // which side to unbox.
        if (lhs == MIRType_Boolean)
             swapOperands();

        compareType_ = Compare_Boolean;
        return;
    }

    // Determine if we can do the compare based on a quick value check.
    if (!relationalEq && CanDoValueBitwiseCmp(constraints, getOperand(0), getOperand(1), looseEq)) {
        compareType_ = Compare_Value;
        return;
    }

    // Type information is not good enough to pick out a particular type of
    // comparison we can do here. Try to specialize based on any baseline
    // caches that have been generated for the opcode. These will cause the
    // instruction's type policy to insert fallible unboxes to the appropriate
    // input types.

    if (!strictEq)
        compareType_ = inspector->expectedCompareType(pc);
}

MBitNot*
MBitNot::New(TempAllocator& alloc, MDefinition* input)
{
    return new(alloc) MBitNot(input);
}

MBitNot*
MBitNot::NewAsmJS(TempAllocator& alloc, MDefinition* input)
{
    MBitNot* ins = new(alloc) MBitNot(input);
    ins->specialization_ = MIRType_Int32;
    MOZ_ASSERT(ins->type() == MIRType_Int32);
    return ins;
}

MDefinition*
MBitNot::foldsTo(TempAllocator& alloc)
{
    if (specialization_ != MIRType_Int32)
        return this;

    MDefinition* input = getOperand(0);

    if (input->isConstant()) {
        js::Value v = Int32Value(~(input->constantValue().toInt32()));
        return MConstant::New(alloc, v);
    }

    if (input->isBitNot() && input->toBitNot()->specialization_ == MIRType_Int32) {
        MOZ_ASSERT(input->toBitNot()->getOperand(0)->type() == MIRType_Int32);
        return input->toBitNot()->getOperand(0); // ~~x => x
    }

    return this;
}

MDefinition*
MTypeOf::foldsTo(TempAllocator& alloc)
{
    // Note: we can't use input->type() here, type analysis has
    // boxed the input.
    MOZ_ASSERT(input()->type() == MIRType_Value);

    JSType type;

    switch (inputType()) {
      case MIRType_Double:
      case MIRType_Int32:
        type = JSTYPE_NUMBER;
        break;
      case MIRType_String:
        type = JSTYPE_STRING;
        break;
      case MIRType_Symbol:
        type = JSTYPE_SYMBOL;
        break;
      case MIRType_Null:
        type = JSTYPE_OBJECT;
        break;
      case MIRType_Undefined:
        type = JSTYPE_VOID;
        break;
      case MIRType_Boolean:
        type = JSTYPE_BOOLEAN;
        break;
      case MIRType_Object:
        if (!inputMaybeCallableOrEmulatesUndefined()) {
            // Object is not callable and does not emulate undefined, so it's
            // safe to fold to "object".
            type = JSTYPE_OBJECT;
            break;
        }
        // FALL THROUGH
      default:
        return this;
    }

    return MConstant::New(alloc, StringValue(TypeName(type, GetJitContext()->runtime->names())));
}

void
MTypeOf::cacheInputMaybeCallableOrEmulatesUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(inputMaybeCallableOrEmulatesUndefined());

    if (!MaybeEmulatesUndefined(constraints, input()) && !MaybeCallable(constraints, input()))
        markInputNotCallableOrEmulatesUndefined();
}

MBitAnd*
MBitAnd::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitAnd(left, right);
}

MBitAnd*
MBitAnd::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MBitAnd* ins = new(alloc) MBitAnd(left, right);
    ins->specializeAsInt32();
    return ins;
}

MBitOr*
MBitOr::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitOr(left, right);
}

MBitOr*
MBitOr::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MBitOr* ins = new(alloc) MBitOr(left, right);
    ins->specializeAsInt32();
    return ins;
}

MBitXor*
MBitXor::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitXor(left, right);
}

MBitXor*
MBitXor::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MBitXor* ins = new(alloc) MBitXor(left, right);
    ins->specializeAsInt32();
    return ins;
}

MLsh*
MLsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MLsh(left, right);
}

MLsh*
MLsh::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MLsh* ins = new(alloc) MLsh(left, right);
    ins->specializeAsInt32();
    return ins;
}

MRsh*
MRsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MRsh(left, right);
}

MRsh*
MRsh::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MRsh* ins = new(alloc) MRsh(left, right);
    ins->specializeAsInt32();
    return ins;
}

MUrsh*
MUrsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MUrsh(left, right);
}

MUrsh*
MUrsh::NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    MUrsh* ins = new(alloc) MUrsh(left, right);
    ins->specializeAsInt32();

    // Since Ion has no UInt32 type, we use Int32 and we have a special
    // exception to the type rules: we can return values in
    // (INT32_MIN,UINT32_MAX] and still claim that we have an Int32 type
    // without bailing out. This is necessary because Ion has no UInt32
    // type and we can't have bailouts in asm.js code.
    ins->bailoutsDisabled_ = true;

    return ins;
}

MResumePoint*
MResumePoint::New(TempAllocator& alloc, MBasicBlock* block, jsbytecode* pc, MResumePoint* parent,
                  Mode mode)
{
    MResumePoint* resume = new(alloc) MResumePoint(block, pc, parent, mode);
    if (!resume->init(alloc))
        return nullptr;
    resume->inherit(block);
    return resume;
}

MResumePoint*
MResumePoint::New(TempAllocator& alloc, MBasicBlock* block, MResumePoint* model,
                  const MDefinitionVector& operands)
{
    MResumePoint* resume = new(alloc) MResumePoint(block, model->pc(), model->caller(), model->mode());

    // Allocate the same number of operands as the original resume point, and
    // copy operands from the operands vector and not the not from the current
    // block stack.
    if (!resume->operands_.init(alloc, model->numAllocatedOperands()))
        return nullptr;

    // Copy the operands.
    for (size_t i = 0; i < operands.length(); i++)
        resume->initOperand(i, operands[i]);

    return resume;
}

MResumePoint*
MResumePoint::Copy(TempAllocator& alloc, MResumePoint* src)
{
    MResumePoint* resume = new(alloc) MResumePoint(src->block(), src->pc(),
                                                   src->caller(), src->mode());
    // Copy the operands from the original resume point, and not from the
    // current block stack.
    if (!resume->operands_.init(alloc, src->numAllocatedOperands()))
        return nullptr;

    // Copy the operands.
    for (size_t i = 0; i < resume->numOperands(); i++)
        resume->initOperand(i, src->getOperand(i));
    return resume;
}

MResumePoint::MResumePoint(MBasicBlock* block, jsbytecode* pc, MResumePoint* caller,
                           Mode mode)
  : MNode(block),
    pc_(pc),
    caller_(caller),
    instruction_(nullptr),
    mode_(mode)
{
    block->addResumePoint(this);
}

bool
MResumePoint::init(TempAllocator& alloc)
{
    return operands_.init(alloc, block()->stackDepth());
}

void
MResumePoint::inherit(MBasicBlock* block)
{
    // FixedList doesn't initialize its elements, so do unchecked inits.
    for (size_t i = 0; i < stackDepth(); i++)
        initOperand(i, block->getSlot(i));
}

void
MResumePoint::addStore(TempAllocator& alloc, MDefinition* store, const MResumePoint* cache)
{
    MOZ_ASSERT(block()->outerResumePoint() != this);
    MOZ_ASSERT_IF(cache, !cache->stores_.empty());

    if (cache && cache->stores_.begin()->operand == store) {
        // If the last resume point had the same side-effect stack, then we can
        // reuse the current side effect without cloning it. This is a simple
        // way to share common context by making a spaghetti stack.
        if (++cache->stores_.begin() == stores_.begin()) {
            stores_.copy(cache->stores_);
            return;
        }
    }

    // Ensure that the store would not be deleted by DCE.
    MOZ_ASSERT(store->isEffectful());

    MStoreToRecover* top = new(alloc) MStoreToRecover(store);
    stores_.push(top);
}

void
MResumePoint::dump(FILE* fp) const
{
    fprintf(fp, "resumepoint mode=");

    switch (mode()) {
      case MResumePoint::ResumeAt:
        fprintf(fp, "At");
        break;
      case MResumePoint::ResumeAfter:
        fprintf(fp, "After");
        break;
      case MResumePoint::Outer:
        fprintf(fp, "Outer");
        break;
    }

    if (MResumePoint* c = caller())
        fprintf(fp, " (caller in block%u)", c->block()->id());

    for (size_t i = 0; i < numOperands(); i++) {
        fprintf(fp, " ");
        if (operands_[i].hasProducer())
            getOperand(i)->printName(fp);
        else
            fprintf(fp, "(null)");
    }
    fprintf(fp, "\n");
}

void
MResumePoint::dump() const
{
    dump(stderr);
}

bool
MResumePoint::isObservableOperand(MUse* u) const
{
    return isObservableOperand(indexOf(u));
}

bool
MResumePoint::isObservableOperand(size_t index) const
{
    return block()->info().isObservableSlot(index);
}

bool
MResumePoint::isRecoverableOperand(MUse* u) const
{
    return block()->info().isRecoverableOperand(indexOf(u));
}

MDefinition*
MToInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);

    // Fold this operation if the input operand is constant.
    if (input->isConstant()) {
        Value val = input->toConstant()->value();
        DebugOnly<MacroAssembler::IntConversionInputKind> convert = conversion();
        switch (input->type()) {
          case MIRType_Null:
            MOZ_ASSERT(convert == MacroAssembler::IntConversion_Any);
            return MConstant::New(alloc, Int32Value(0));
          case MIRType_Boolean:
            MOZ_ASSERT(convert == MacroAssembler::IntConversion_Any ||
                       convert == MacroAssembler::IntConversion_NumbersOrBoolsOnly);
            return MConstant::New(alloc, Int32Value(val.toBoolean()));
          case MIRType_Int32:
            return MConstant::New(alloc, Int32Value(val.toInt32()));
          case MIRType_Float32:
          case MIRType_Double:
            int32_t ival;
            // Only the value within the range of Int32 can be substitued as constant.
            if (mozilla::NumberEqualsInt32(val.toNumber(), &ival))
                return MConstant::New(alloc, Int32Value(ival));
          default:
            break;
        }
    }

    if (input->type() == MIRType_Int32)
        return input;
    return this;
}

void
MToInt32::analyzeEdgeCasesBackward()
{
    if (!NeedNegativeZeroCheck(this))
        setCanBeNegativeZero(false);
}

MDefinition*
MTruncateToInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->isBox())
        input = input->getOperand(0);

    if (input->type() == MIRType_Int32)
        return input;

    if (input->type() == MIRType_Double && input->isConstant()) {
        const Value& v = input->constantValue();
        int32_t ret = ToInt32(v.toDouble());
        return MConstant::New(alloc, Int32Value(ret));
    }

    return this;
}

MDefinition*
MToDouble::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->isBox())
        input = input->getOperand(0);

    if (input->type() == MIRType_Double)
        return input;

    if (input->isConstant()) {
        const Value& v = input->toConstant()->value();
        if (v.isNumber()) {
            double out = v.toNumber();
            return MConstant::New(alloc, DoubleValue(out));
        }
    }

    return this;
}

MDefinition*
MToFloat32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->isBox())
        input = input->getOperand(0);

    if (input->type() == MIRType_Float32)
        return input;

    // If x is a Float32, Float32(Double(x)) == x
    if (input->isToDouble() && input->toToDouble()->input()->type() == MIRType_Float32)
        return input->toToDouble()->input();

    if (input->isConstant()) {
        const Value& v = input->toConstant()->value();
        if (v.isNumber()) {
            float out = v.toNumber();
            MConstant* c = MConstant::New(alloc, DoubleValue(out));
            c->setResultType(MIRType_Float32);
            return c;
        }
    }
    return this;
}

MDefinition*
MToString::foldsTo(TempAllocator& alloc)
{
    MDefinition* in = input();
    if (in->isBox())
        in = in->getOperand(0);

    if (in->type() == MIRType_String)
        return in;
    return this;
}

MDefinition*
MClampToUint8::foldsTo(TempAllocator& alloc)
{
    if (input()->isConstantValue()) {
        const Value& v = input()->constantValue();
        if (v.isDouble()) {
            int32_t clamped = ClampDoubleToUint8(v.toDouble());
            return MConstant::New(alloc, Int32Value(clamped));
        }
        if (v.isInt32()) {
            int32_t clamped = ClampIntForUint8Array(v.toInt32());
            return MConstant::New(alloc, Int32Value(clamped));
        }
    }
    return this;
}

bool
MCompare::tryFoldEqualOperands(bool* result)
{
    // Temporarily disabled due to bug 1130679.
    return false;

    if (lhs() != rhs())
        return false;

    // Intuitively somebody would think that if lhs == rhs,
    // then we can just return true. (Or false for !==)
    // However NaN !== NaN is true! So we spend some time trying
    // to eliminate this case.

    if (jsop() != JSOP_STRICTEQ && jsop() != JSOP_STRICTNE)
        return false;

    if (compareType_ == Compare_Unknown)
        return false;

    MOZ_ASSERT(compareType_ == Compare_Undefined || compareType_ == Compare_Null ||
               compareType_ == Compare_Boolean || compareType_ == Compare_Int32 ||
               compareType_ == Compare_Int32MaybeCoerceBoth ||
               compareType_ == Compare_Int32MaybeCoerceLHS ||
               compareType_ == Compare_Int32MaybeCoerceRHS || compareType_ == Compare_UInt32 ||
               compareType_ == Compare_Double || compareType_ == Compare_DoubleMaybeCoerceLHS ||
               compareType_ == Compare_DoubleMaybeCoerceRHS || compareType_ == Compare_Float32 ||
               compareType_ == Compare_String || compareType_ == Compare_StrictString ||
               compareType_ == Compare_Object || compareType_ == Compare_Value);

    if (isDoubleComparison() || isFloat32Comparison()) {
        if (!operandsAreNeverNaN())
            return false;
    }

    *result = (jsop() == JSOP_STRICTEQ);
    return true;
}

bool
MCompare::tryFold(bool* result)
{
    JSOp op = jsop();

    if (tryFoldEqualOperands(result))
        return true;

    if (compareType_ == Compare_Null || compareType_ == Compare_Undefined) {
        // The LHS is the value we want to test against null or undefined.
        if (op == JSOP_STRICTEQ || op == JSOP_STRICTNE) {
            if (lhs()->type() == inputType()) {
                *result = (op == JSOP_STRICTEQ);
                return true;
            }
            if (!lhs()->mightBeType(inputType())) {
                *result = (op == JSOP_STRICTNE);
                return true;
            }
        } else {
            MOZ_ASSERT(op == JSOP_EQ || op == JSOP_NE);
            if (IsNullOrUndefined(lhs()->type())) {
                *result = (op == JSOP_EQ);
                return true;
            }
            if (!lhs()->mightBeType(MIRType_Null) &&
                !lhs()->mightBeType(MIRType_Undefined) &&
                !(lhs()->mightBeType(MIRType_Object) && operandMightEmulateUndefined()))
            {
                *result = (op == JSOP_NE);
                return true;
            }
        }
        return false;
    }

    if (compareType_ == Compare_Boolean) {
        MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);
        MOZ_ASSERT(rhs()->type() == MIRType_Boolean);
        MOZ_ASSERT(lhs()->type() != MIRType_Boolean, "Should use Int32 comparison");

        if (!lhs()->mightBeType(MIRType_Boolean)) {
            *result = (op == JSOP_STRICTNE);
            return true;
        }
        return false;
    }

    if (compareType_ == Compare_StrictString) {
        MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);
        MOZ_ASSERT(rhs()->type() == MIRType_String);
        MOZ_ASSERT(lhs()->type() != MIRType_String, "Should use String comparison");

        if (!lhs()->mightBeType(MIRType_String)) {
            *result = (op == JSOP_STRICTNE);
            return true;
        }
        return false;
    }

    return false;
}

bool
MCompare::evaluateConstantOperands(TempAllocator& alloc, bool* result)
{
    if (type() != MIRType_Boolean && type() != MIRType_Int32)
        return false;

    MDefinition* left = getOperand(0);
    MDefinition* right = getOperand(1);

    if (compareType() == Compare_Double) {
        // Optimize "MCompare MConstant (MToDouble SomethingInInt32Range).
        // In most cases the MToDouble was added, because the constant is
        // a double.
        // e.g. v < 9007199254740991, where v is an int32 is always true.
        if (!lhs()->isConstant() && !rhs()->isConstant())
            return false;

        MDefinition* operand = left->isConstant() ? right : left;
        MConstant* constant = left->isConstant() ? left->toConstant() : right->toConstant();
        MOZ_ASSERT(constant->value().isDouble());
        double cte = constant->value().toDouble();

        if (operand->isToDouble() && operand->getOperand(0)->type() == MIRType_Int32) {
            bool replaced = false;
            switch (jsop_) {
              case JSOP_LT:
                if (cte > INT32_MAX || cte < INT32_MIN) {
                    *result = !((constant == lhs()) ^ (cte < INT32_MIN));
                    replaced = true;
                }
                break;
              case JSOP_LE:
                if (constant == lhs()) {
                    if (cte > INT32_MAX || cte <= INT32_MIN) {
                        *result = (cte <= INT32_MIN);
                        replaced = true;
                    }
                } else {
                    if (cte >= INT32_MAX || cte < INT32_MIN) {
                        *result = (cte >= INT32_MIN);
                        replaced = true;
                    }
                }
                break;
              case JSOP_GT:
                if (cte > INT32_MAX || cte < INT32_MIN) {
                    *result = !((constant == rhs()) ^ (cte < INT32_MIN));
                    replaced = true;
                }
                break;
              case JSOP_GE:
                if (constant == lhs()) {
                    if (cte >= INT32_MAX || cte < INT32_MIN) {
                        *result = (cte >= INT32_MAX);
                        replaced = true;
                    }
                } else {
                    if (cte > INT32_MAX || cte <= INT32_MIN) {
                        *result = (cte <= INT32_MIN);
                        replaced = true;
                    }
                }
                break;
              case JSOP_STRICTEQ: // Fall through.
              case JSOP_EQ:
                if (cte > INT32_MAX || cte < INT32_MIN) {
                    *result = false;
                    replaced = true;
                }
                break;
              case JSOP_STRICTNE: // Fall through.
              case JSOP_NE:
                if (cte > INT32_MAX || cte < INT32_MIN) {
                    *result = true;
                    replaced = true;
                }
                break;
              default:
                MOZ_CRASH("Unexpected op.");
            }
            if (replaced) {
                MLimitedTruncate* limit =
                    MLimitedTruncate::New(alloc, operand->getOperand(0), MDefinition::NoTruncate);
                limit->setGuardUnchecked();
                block()->insertBefore(this, limit);
                return true;
            }
        }
    }

    if (!left->isConstant() || !right->isConstant())
        return false;

    Value lhs = left->toConstant()->value();
    Value rhs = right->toConstant()->value();

    // Fold away some String equality comparisons.
    if (lhs.isString() && rhs.isString()) {
        int32_t comp = 0; // Default to equal.
        if (left != right)
            comp = CompareAtoms(&lhs.toString()->asAtom(), &rhs.toString()->asAtom());

        switch (jsop_) {
          case JSOP_LT:
            *result = (comp < 0);
            break;
          case JSOP_LE:
            *result = (comp <= 0);
            break;
          case JSOP_GT:
            *result = (comp > 0);
            break;
          case JSOP_GE:
            *result = (comp >= 0);
            break;
          case JSOP_STRICTEQ: // Fall through.
          case JSOP_EQ:
            *result = (comp == 0);
            break;
          case JSOP_STRICTNE: // Fall through.
          case JSOP_NE:
            *result = (comp != 0);
            break;
          default:
            MOZ_CRASH("Unexpected op.");
        }

        return true;
    }

    if (compareType_ == Compare_UInt32) {
        uint32_t lhsUint = uint32_t(lhs.toInt32());
        uint32_t rhsUint = uint32_t(rhs.toInt32());

        switch (jsop_) {
          case JSOP_LT:
            *result = (lhsUint < rhsUint);
            break;
          case JSOP_LE:
            *result = (lhsUint <= rhsUint);
            break;
          case JSOP_GT:
            *result = (lhsUint > rhsUint);
            break;
          case JSOP_GE:
            *result = (lhsUint >= rhsUint);
            break;
          case JSOP_STRICTEQ: // Fall through.
          case JSOP_EQ:
            *result = (lhsUint == rhsUint);
            break;
          case JSOP_STRICTNE: // Fall through.
          case JSOP_NE:
            *result = (lhsUint != rhsUint);
            break;
          default:
            MOZ_CRASH("Unexpected op.");
        }

        return true;
    }

    if (!lhs.isNumber() || !rhs.isNumber())
        return false;

    switch (jsop_) {
      case JSOP_LT:
        *result = (lhs.toNumber() < rhs.toNumber());
        break;
      case JSOP_LE:
        *result = (lhs.toNumber() <= rhs.toNumber());
        break;
      case JSOP_GT:
        *result = (lhs.toNumber() > rhs.toNumber());
        break;
      case JSOP_GE:
        *result = (lhs.toNumber() >= rhs.toNumber());
        break;
      case JSOP_STRICTEQ: // Fall through.
      case JSOP_EQ:
        *result = (lhs.toNumber() == rhs.toNumber());
        break;
      case JSOP_STRICTNE: // Fall through.
      case JSOP_NE:
        *result = (lhs.toNumber() != rhs.toNumber());
        break;
      default:
        return false;
    }

    return true;
}

MDefinition*
MCompare::foldsTo(TempAllocator& alloc)
{
    bool result;

    if (tryFold(&result) || evaluateConstantOperands(alloc, &result)) {
        if (type() == MIRType_Int32)
            return MConstant::New(alloc, Int32Value(result));

        MOZ_ASSERT(type() == MIRType_Boolean);
        return MConstant::New(alloc, BooleanValue(result));
    }

    return this;
}

void
MCompare::trySpecializeFloat32(TempAllocator& alloc)
{
    MDefinition* lhs = getOperand(0);
    MDefinition* rhs = getOperand(1);

    if (lhs->canProduceFloat32() && rhs->canProduceFloat32() && compareType_ == Compare_Double) {
        compareType_ = Compare_Float32;
    } else {
        if (lhs->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, lhs, this);
        if (rhs->type() == MIRType_Float32)
            ConvertDefinitionToDouble<1>(alloc, rhs, this);
    }
}

void
MCompare::filtersUndefinedOrNull(bool trueBranch, MDefinition** subject, bool* filtersUndefined,
                                 bool* filtersNull)
{
    *filtersNull = *filtersUndefined = false;
    *subject = nullptr;

    if (compareType() != Compare_Undefined && compareType() != Compare_Null)
        return;

    MOZ_ASSERT(jsop() == JSOP_STRICTNE || jsop() == JSOP_NE ||
               jsop() == JSOP_STRICTEQ || jsop() == JSOP_EQ);

    // JSOP_*NE only removes undefined/null from if/true branch
    if (!trueBranch && (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE))
        return;

    // JSOP_*EQ only removes undefined/null from else/false branch
    if (trueBranch && (jsop() == JSOP_STRICTEQ || jsop() == JSOP_EQ))
        return;

    if (jsop() == JSOP_STRICTEQ || jsop() == JSOP_STRICTNE) {
        *filtersUndefined = compareType() == Compare_Undefined;
        *filtersNull = compareType() == Compare_Null;
    } else {
        *filtersUndefined = *filtersNull = true;
    }

    *subject = lhs();
}

void
MNot::cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(operandMightEmulateUndefined());

    if (!MaybeEmulatesUndefined(constraints, getOperand(0)))
        markOperandCantEmulateUndefined();
}

MDefinition*
MNot::foldsTo(TempAllocator& alloc)
{
    // Fold if the input is constant
    if (input()->isConstantValue() && !input()->constantValue().isMagic()) {
        bool result = input()->constantToBoolean();
        if (type() == MIRType_Int32)
            return MConstant::New(alloc, Int32Value(!result));

        // ToBoolean can't cause side effects, so this is safe.
        return MConstant::New(alloc, BooleanValue(!result));
    }

    // If the operand of the Not is itself a Not, they cancel out. But we can't
    // always convert Not(Not(x)) to x because that may loose the conversion to
    // boolean. We can simplify Not(Not(Not(x))) to Not(x) though.
    MDefinition* op = getOperand(0);
    if (op->isNot()) {
        MDefinition* opop = op->getOperand(0);
        if (opop->isNot())
            return opop;
    }

    // NOT of an undefined or null value is always true
    if (input()->type() == MIRType_Undefined || input()->type() == MIRType_Null)
        return MConstant::New(alloc, BooleanValue(true));

    // NOT of a symbol is always false.
    if (input()->type() == MIRType_Symbol)
        return MConstant::New(alloc, BooleanValue(false));

    // NOT of an object that can't emulate undefined is always false.
    if (input()->type() == MIRType_Object && !operandMightEmulateUndefined())
        return MConstant::New(alloc, BooleanValue(false));

    return this;
}

void
MNot::trySpecializeFloat32(TempAllocator& alloc)
{
    MDefinition* in = input();
    if (!in->canProduceFloat32() && in->type() == MIRType_Float32)
        ConvertDefinitionToDouble<0>(alloc, in, this);
}

void
MBeta::printOpcode(FILE* fp) const
{
    MDefinition::printOpcode(fp);

    if (JitContext* context = MaybeGetJitContext()) {
        Sprinter sp(context->cx);
        sp.init();
        comparison_->print(sp);
        fprintf(fp, " %s", sp.string());
    } else {
        fprintf(fp, " ???");
    }
}

bool
MNewObject::shouldUseVM() const
{
    PlainObject* obj = templateObject();
    return obj->isSingleton() || obj->hasDynamicSlots();
}

bool
MCreateThisWithTemplate::canRecoverOnBailout() const
{
    MOZ_ASSERT(templateObject()->is<PlainObject>() || templateObject()->is<UnboxedPlainObject>());
    MOZ_ASSERT_IF(templateObject()->is<PlainObject>(),
                  !templateObject()->as<PlainObject>().denseElementsAreCopyOnWrite());
    return true;
}

MObjectState::MObjectState(MDefinition* obj)
{
    // This instruction is only used as a summary for bailout paths.
    setResultType(MIRType_Object);
    setRecoveredOnBailout();
    NativeObject* templateObject = nullptr;
    if (obj->isNewObject())
        templateObject = obj->toNewObject()->templateObject();
    else if (obj->isCreateThisWithTemplate())
        templateObject = &obj->toCreateThisWithTemplate()->templateObject()->as<PlainObject>();
    else
        templateObject = obj->toNewCallObject()->templateObject();
    numSlots_ = templateObject->slotSpan();
    numFixedSlots_ = templateObject->numFixedSlots();
}

bool
MObjectState::init(TempAllocator& alloc, MDefinition* obj)
{
    if (!MVariadicInstruction::init(alloc, numSlots() + 1))
        return false;
    // +1, for the Object.
    initOperand(0, obj);
    return true;
}

MObjectState*
MObjectState::New(TempAllocator& alloc, MDefinition* obj, MDefinition* undefinedVal)
{
    MObjectState* res = new(alloc) MObjectState(obj);
    if (!res || !res->init(alloc, obj))
        return nullptr;
    for (size_t i = 0; i < res->numSlots(); i++)
        res->initSlot(i, undefinedVal);
    return res;
}

MObjectState*
MObjectState::Copy(TempAllocator& alloc, MObjectState* state)
{
    MDefinition* obj = state->object();
    MObjectState* res = new(alloc) MObjectState(obj);
    if (!res || !res->init(alloc, obj))
        return nullptr;
    for (size_t i = 0; i < res->numSlots(); i++)
        res->initSlot(i, state->getSlot(i));
    return res;
}

MArrayState::MArrayState(MDefinition* arr)
{
    // This instruction is only used as a summary for bailout paths.
    setResultType(MIRType_Object);
    setRecoveredOnBailout();
    numElements_ = arr->toNewArray()->count();
}

bool
MArrayState::init(TempAllocator& alloc, MDefinition* obj, MDefinition* len)
{
    if (!MVariadicInstruction::init(alloc, numElements() + 2))
        return false;
    // +1, for the Array object.
    initOperand(0, obj);
    // +1, for the length value of the array.
    initOperand(1, len);
    return true;
}

MArrayState*
MArrayState::New(TempAllocator& alloc, MDefinition* arr, MDefinition* undefinedVal,
                 MDefinition* initLength)
{
    MArrayState* res = new(alloc) MArrayState(arr);
    if (!res || !res->init(alloc, arr, initLength))
        return nullptr;
    for (size_t i = 0; i < res->numElements(); i++)
        res->initElement(i, undefinedVal);
    return res;
}

MArrayState*
MArrayState::Copy(TempAllocator& alloc, MArrayState* state)
{
    MDefinition* arr = state->array();
    MDefinition* len = state->initializedLength();
    MArrayState* res = new(alloc) MArrayState(arr);
    if (!res || !res->init(alloc, arr, len))
        return nullptr;
    for (size_t i = 0; i < res->numElements(); i++)
        res->initElement(i, state->getElement(i));
    return res;
}

bool
MNewArray::shouldUseVM() const
{
    MOZ_ASSERT(count() < NativeObject::NELEMENTS_LIMIT);

    size_t arraySlots =
        gc::GetGCKindSlots(templateObject()->asTenured().getAllocKind()) - ObjectElements::VALUES_PER_HEADER;

    // Allocate space using the VMCall when mir hints it needs to get allocated
    // immediately, but only when data doesn't fit the available array slots.
    bool allocating = allocatingBehaviour() != NewArray_Unallocating && count() > arraySlots;

    return templateObject()->isSingleton() || allocating;
}

bool
MLoadFixedSlot::mightAlias(const MDefinition* store) const
{
    if (store->isStoreFixedSlot() && store->toStoreFixedSlot()->slot() != slot())
        return false;
    return true;
}

MDefinition*
MLoadFixedSlot::foldsTo(TempAllocator& alloc)
{
    if (!dependency() || !dependency()->isStoreFixedSlot())
        return this;

    MStoreFixedSlot* store = dependency()->toStoreFixedSlot();
    if (!store->block()->dominates(block()))
        return this;

    if (store->object() != object())
        return this;

    if (store->slot() != slot())
        return this;

    return foldsToStoredValue(alloc, store->value());
}

bool
MAsmJSLoadHeap::mightAlias(const MDefinition* def) const
{
    if (def->isAsmJSStoreHeap()) {
        const MAsmJSStoreHeap* store = def->toAsmJSStoreHeap();
        if (store->accessType() != accessType())
            return true;
        if (!ptr()->isConstant() || !store->ptr()->isConstant())
            return true;
        const MConstant* otherPtr = store->ptr()->toConstant();
        return ptr()->toConstant()->value() == otherPtr->value();
    }
    return true;
}

bool
MAsmJSLoadHeap::congruentTo(const MDefinition* ins) const
{
    if (!ins->isAsmJSLoadHeap())
        return false;
    const MAsmJSLoadHeap* load = ins->toAsmJSLoadHeap();
    return load->accessType() == accessType() && congruentIfOperandsEqual(load);
}

bool
MAsmJSLoadGlobalVar::mightAlias(const MDefinition* def) const
{
    if (def->isAsmJSStoreGlobalVar()) {
        const MAsmJSStoreGlobalVar* store = def->toAsmJSStoreGlobalVar();
        return store->globalDataOffset() == globalDataOffset_;
    }
    return true;
}

HashNumber
MAsmJSLoadGlobalVar::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, globalDataOffset_);
    return hash;
}

bool
MAsmJSLoadGlobalVar::congruentTo(const MDefinition* ins) const
{
    if (ins->isAsmJSLoadGlobalVar()) {
        const MAsmJSLoadGlobalVar* load = ins->toAsmJSLoadGlobalVar();
        return globalDataOffset_ == load->globalDataOffset_;
    }
    return false;
}

MDefinition*
MAsmJSLoadGlobalVar::foldsTo(TempAllocator& alloc)
{
    if (!dependency() || !dependency()->isAsmJSStoreGlobalVar())
        return this;

    MAsmJSStoreGlobalVar* store = dependency()->toAsmJSStoreGlobalVar();
    if (!store->block()->dominates(block()))
        return this;

    if (store->globalDataOffset() != globalDataOffset())
        return this;

    if (store->value()->type() != type())
        return this;

    return store->value();
}

HashNumber
MAsmJSLoadFuncPtr::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, globalDataOffset_);
    return hash;
}

bool
MAsmJSLoadFuncPtr::congruentTo(const MDefinition* ins) const
{
    if (ins->isAsmJSLoadFuncPtr()) {
        const MAsmJSLoadFuncPtr* load = ins->toAsmJSLoadFuncPtr();
        return globalDataOffset_ == load->globalDataOffset_;
    }
    return false;
}

HashNumber
MAsmJSLoadFFIFunc::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, globalDataOffset_);
    return hash;
}

bool
MAsmJSLoadFFIFunc::congruentTo(const MDefinition* ins) const
{
    if (ins->isAsmJSLoadFFIFunc()) {
        const MAsmJSLoadFFIFunc* load = ins->toAsmJSLoadFFIFunc();
        return globalDataOffset_ == load->globalDataOffset_;
    }
    return false;
}

bool
MLoadSlot::mightAlias(const MDefinition* store) const
{
    if (store->isStoreSlot() && store->toStoreSlot()->slot() != slot())
        return false;
    return true;
}

HashNumber
MLoadSlot::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, slot_);
    return hash;
}

MDefinition*
MLoadSlot::foldsTo(TempAllocator& alloc)
{
    if (!dependency() || !dependency()->isStoreSlot())
        return this;

    MStoreSlot* store = dependency()->toStoreSlot();
    if (!store->block()->dominates(block()))
        return this;

    if (store->slots() != slots())
        return this;

    return foldsToStoredValue(alloc, store->value());
}

MDefinition*
MFunctionEnvironment::foldsTo(TempAllocator& alloc)
{
    if (!input()->isLambda())
        return this;

    return input()->toLambda()->scopeChain();
}

MDefinition*
MLoadElement::foldsTo(TempAllocator& alloc)
{
    if (!dependency() || !dependency()->isStoreElement())
        return this;

    MStoreElement* store = dependency()->toStoreElement();
    if (!store->block()->dominates(block()))
        return this;

    if (store->elements() != elements())
        return this;

    if (store->index() != index())
        return this;

    return foldsToStoredValue(alloc, store->value());
}

bool
MGuardShapePolymorphic::congruentTo(const MDefinition* ins) const
{
    if (!ins->isGuardShapePolymorphic())
        return false;

    const MGuardShapePolymorphic* other = ins->toGuardShapePolymorphic();
    if (numShapes() != other->numShapes())
        return false;

    for (size_t i = 0; i < numShapes(); i++) {
        if (getShape(i) != other->getShape(i))
            return false;
    }

    return congruentIfOperandsEqual(ins);
}

void
InlinePropertyTable::trimTo(const ObjectVector& targets, const BoolVector& choiceSet)
{
    for (size_t i = 0; i < targets.length(); i++) {
        // If the target was inlined, don't erase the entry.
        if (choiceSet[i])
            continue;

        JSFunction* target = &targets[i]->as<JSFunction>();

        // Eliminate all entries containing the vetoed function from the map.
        size_t j = 0;
        while (j < numEntries()) {
            if (entries_[j]->func == target)
                entries_.erase(&entries_[j]);
            else
                j++;
        }
    }
}

void
InlinePropertyTable::trimToTargets(const ObjectVector& targets)
{
    JitSpew(JitSpew_Inlining, "Got inlineable property cache with %d cases",
            (int)numEntries());

    size_t i = 0;
    while (i < numEntries()) {
        bool foundFunc = false;
        for (size_t j = 0; j < targets.length(); j++) {
            if (entries_[i]->func == targets[j]) {
                foundFunc = true;
                break;
            }
        }
        if (!foundFunc)
            entries_.erase(&(entries_[i]));
        else
            i++;
    }

    JitSpew(JitSpew_Inlining, "%d inlineable cases left after trimming to %d targets",
            (int)numEntries(), (int)targets.length());
}

bool
InlinePropertyTable::hasFunction(JSFunction* func) const
{
    for (size_t i = 0; i < numEntries(); i++) {
        if (entries_[i]->func == func)
            return true;
    }
    return false;
}

TemporaryTypeSet*
InlinePropertyTable::buildTypeSetForFunction(JSFunction* func) const
{
    LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();
    TemporaryTypeSet* types = alloc->new_<TemporaryTypeSet>();
    if (!types)
        return nullptr;
    for (size_t i = 0; i < numEntries(); i++) {
        if (entries_[i]->func == func)
            types->addType(TypeSet::ObjectType(entries_[i]->group), alloc);
    }
    return types;
}

void*
MLoadTypedArrayElementStatic::base() const
{
    return AnyTypedArrayViewData(someTypedArray_);
}

size_t
MLoadTypedArrayElementStatic::length() const
{
    return AnyTypedArrayByteLength(someTypedArray_);
}

bool
MLoadTypedArrayElementStatic::congruentTo(const MDefinition* ins) const
{
    if (!ins->isLoadTypedArrayElementStatic())
        return false;
    const MLoadTypedArrayElementStatic* other = ins->toLoadTypedArrayElementStatic();
    if (offset() != other->offset())
        return false;
    if (needsBoundsCheck() != other->needsBoundsCheck())
        return false;
    if (accessType() != other->accessType())
        return false;
    if (base() != other->base())
        return false;
    return congruentIfOperandsEqual(other);
}

void*
MStoreTypedArrayElementStatic::base() const
{
    return AnyTypedArrayViewData(someTypedArray_);
}

bool
MGetElementCache::allowDoubleResult() const
{
    if (!resultTypeSet())
        return true;

    return resultTypeSet()->hasType(TypeSet::DoubleType());
}

size_t
MStoreTypedArrayElementStatic::length() const
{
    return AnyTypedArrayByteLength(someTypedArray_);
}

bool
MGetPropertyPolymorphic::mightAlias(const MDefinition* store) const
{
    // Allow hoisting this instruction if the store does not write to a
    // slot read by this instruction.

    if (!store->isStoreFixedSlot() && !store->isStoreSlot())
        return true;

    for (size_t i = 0; i < numShapes(); i++) {
        const Shape* shape = this->shape(i);
        if (shape->slot() < shape->numFixedSlots()) {
            // Fixed slot.
            uint32_t slot = shape->slot();
            if (store->isStoreFixedSlot() && store->toStoreFixedSlot()->slot() != slot)
                continue;
            if (store->isStoreSlot())
                continue;
        } else {
            // Dynamic slot.
            uint32_t slot = shape->slot() - shape->numFixedSlots();
            if (store->isStoreSlot() && store->toStoreSlot()->slot() != slot)
                continue;
            if (store->isStoreFixedSlot())
                continue;
        }

        return true;
    }

    return false;
}

void
MGetPropertyCache::setBlock(MBasicBlock* block)
{
    MDefinition::setBlock(block);
    // Track where we started.
    if (!location_.pc) {
        location_.pc = block->trackedPc();
        location_.script = block->info().script();
    }
}

bool
MGetPropertyCache::updateForReplacement(MDefinition* ins) {
    MGetPropertyCache* other = ins->toGetPropertyCache();
    location_.append(&other->location_);
    return true;
}

MDefinition*
MAsmJSUnsignedToDouble::foldsTo(TempAllocator& alloc)
{
    if (input()->isConstantValue()) {
        const Value& v = input()->constantValue();
        if (v.isInt32())
            return MConstant::New(alloc, DoubleValue(uint32_t(v.toInt32())));
    }

    return this;
}

MDefinition*
MAsmJSUnsignedToFloat32::foldsTo(TempAllocator& alloc)
{
    if (input()->isConstantValue()) {
        const Value& v = input()->constantValue();
        if (v.isInt32()) {
            double dval = double(uint32_t(v.toInt32()));
            if (IsFloat32Representable(dval))
                return MConstant::NewAsmJS(alloc, JS::Float32Value(float(dval)), MIRType_Float32);
        }
    }

    return this;
}

MAsmJSCall*
MAsmJSCall::New(TempAllocator& alloc, const CallSiteDesc& desc, Callee callee,
                const Args& args, MIRType resultType, size_t spIncrement)
{
    MAsmJSCall* call = new(alloc) MAsmJSCall(desc, callee, spIncrement);
    call->setResultType(resultType);

    if (!call->argRegs_.init(alloc, args.length()))
        return nullptr;
    for (size_t i = 0; i < call->argRegs_.length(); i++)
        call->argRegs_[i] = args[i].reg;

    if (!call->init(alloc, call->argRegs_.length() + (callee.which() == Callee::Dynamic ? 1 : 0)))
        return nullptr;
    // FixedList doesn't initialize its elements, so do an unchecked init.
    for (size_t i = 0; i < call->argRegs_.length(); i++)
        call->initOperand(i, args[i].def);
    if (callee.which() == Callee::Dynamic)
        call->initOperand(call->argRegs_.length(), callee.dynamic());

    return call;
}

void
MSqrt::trySpecializeFloat32(TempAllocator& alloc) {
    if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
        if (input()->type() == MIRType_Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType_Float32);
    setPolicyType(MIRType_Float32);
}

MDefinition*
MClz::foldsTo(TempAllocator& alloc)
{
    if (num()->isConstantValue()) {
        int32_t n = num()->constantValue().toInt32();
        if (n == 0)
            return MConstant::New(alloc, Int32Value(32));
        return MConstant::New(alloc, Int32Value(mozilla::CountLeadingZeroes32(n)));
    }

    return this;
}

MDefinition*
MBoundsCheck::foldsTo(TempAllocator& alloc)
{
    if (index()->isConstantValue() && length()->isConstantValue()) {
       uint32_t len = length()->constantValue().toInt32();
       uint32_t idx = index()->constantValue().toInt32();
       if (idx + uint32_t(minimum()) < len && idx + uint32_t(maximum()) < len)
           return index();
    }

    return this;
}

MDefinition*
MTableSwitch::foldsTo(TempAllocator& alloc)
{
    MDefinition* op = getOperand(0);

    // If we only have one successor, convert to a plain goto to the only
    // successor. TableSwitch indices are numeric; other types will always go to
    // the only successor.
    if (numSuccessors() == 1 || (op->type() != MIRType_Value && !IsNumberType(op->type())))
        return MGoto::New(alloc, getDefault());

    return this;
}

MDefinition*
MArrayJoin::foldsTo(TempAllocator& alloc) {
    // :TODO: Enable this optimization after fixing Bug 977966 test cases.
    return this;

    MDefinition* arr = array();

    if (!arr->isStringSplit())
        return this;

    this->setRecoveredOnBailout();
    if (arr->hasLiveDefUses()) {
        this->setNotRecoveredOnBailout();
        return this;
    }

    // We're replacing foo.split(bar).join(baz) by
    // foo.replace(bar, baz).  MStringSplit could be recovered by
    // a bailout.  As we are removing its last use, and its result
    // could be captured by a resume point, this MStringSplit will
    // be executed on the bailout path.
    MDefinition* string = arr->toStringSplit()->string();
    MDefinition* pattern = arr->toStringSplit()->separator();
    MDefinition* replacement = sep();

    setNotRecoveredOnBailout();
    return MStringReplace::New(alloc, string, pattern, replacement);
}

bool
jit::ElementAccessIsDenseNative(CompilerConstraintList* constraints,
                                MDefinition* obj, MDefinition* id)
{
    if (obj->mightBeType(MIRType_String))
        return false;

    if (id->type() != MIRType_Int32 && id->type() != MIRType_Double)
        return false;

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types)
        return false;

    // Typed arrays are native classes but do not have dense elements.
    const Class* clasp = types->getKnownClass(constraints);
    return clasp && clasp->isNative() && !IsAnyTypedArrayClass(clasp);
}

bool
jit::ElementAccessIsAnyTypedArray(CompilerConstraintList* constraints,
                                  MDefinition* obj, MDefinition* id,
                                  Scalar::Type* arrayType)
{
    if (obj->mightBeType(MIRType_String))
        return false;

    if (id->type() != MIRType_Int32 && id->type() != MIRType_Double)
        return false;

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types)
        return false;

    *arrayType = types->getTypedArrayType(constraints);
    if (*arrayType != Scalar::MaxTypedArrayViewType)
        return true;
    *arrayType = types->getSharedTypedArrayType(constraints);
    return *arrayType != Scalar::MaxTypedArrayViewType;
}

bool
jit::ElementAccessIsPacked(CompilerConstraintList* constraints, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();
    return types && !types->hasObjectFlags(constraints, OBJECT_FLAG_NON_PACKED);
}

bool
jit::ElementAccessMightBeCopyOnWrite(CompilerConstraintList* constraints, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();
    return !types || types->hasObjectFlags(constraints, OBJECT_FLAG_COPY_ON_WRITE);
}

bool
jit::ElementAccessHasExtraIndexedProperty(CompilerConstraintList* constraints,
                                          MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();

    if (!types || types->hasObjectFlags(constraints, OBJECT_FLAG_LENGTH_OVERFLOW))
        return true;

    return TypeCanHaveExtraIndexedProperties(constraints, types);
}

MIRType
jit::DenseNativeElementType(CompilerConstraintList* constraints, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();
    MIRType elementType = MIRType_None;
    unsigned count = types->getObjectCount();

    for (unsigned i = 0; i < count; i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return MIRType_None;

        HeapTypeSetKey elementTypes = key->property(JSID_VOID);

        MIRType type = elementTypes.knownMIRType(constraints);
        if (type == MIRType_None)
            return MIRType_None;

        if (elementType == MIRType_None)
            elementType = type;
        else if (elementType != type)
            return MIRType_None;
    }

    return elementType;
}

static BarrierKind
PropertyReadNeedsTypeBarrier(CompilerConstraintList* constraints,
                             TypeSet::ObjectKey* key, PropertyName* name,
                             TypeSet* observed)
{
    // If the object being read from has types for the property which haven't
    // been observed at this access site, the read could produce a new type and
    // a barrier is needed. Note that this only covers reads from properties
    // which are accounted for by type information, i.e. native data properties
    // and elements.
    //
    // We also need a barrier if the object is a proxy, because then all bets
    // are off, just as if it has unknown properties.
    if (key->unknownProperties() || observed->empty() ||
        key->clasp()->isProxy())
    {
        return BarrierKind::TypeSet;
    }

    jsid id = name ? NameToId(name) : JSID_VOID;
    HeapTypeSetKey property = key->property(id);
    if (property.maybeTypes()) {
        if (!TypeSetIncludes(observed, MIRType_Value, property.maybeTypes())) {
            // If all possible objects have been observed, we don't have to
            // guard on the specific object types.
            if (property.maybeTypes()->objectsAreSubset(observed)) {
                property.freeze(constraints);
                return BarrierKind::TypeTagOnly;
            }
            return BarrierKind::TypeSet;
        }
    }

    // Type information for global objects is not required to reflect the
    // initial 'undefined' value for properties, in particular global
    // variables declared with 'var'. Until the property is assigned a value
    // other than undefined, a barrier is required.
    if (key->isSingleton()) {
        JSObject* obj = key->singleton();
        if (name && CanHaveEmptyPropertyTypesForOwnProperty(obj) &&
            (!property.maybeTypes() || property.maybeTypes()->empty()))
        {
            return BarrierKind::TypeSet;
        }
    }

    property.freeze(constraints);
    return BarrierKind::NoBarrier;
}

BarrierKind
jit::PropertyReadNeedsTypeBarrier(JSContext* propertycx,
                                  CompilerConstraintList* constraints,
                                  TypeSet::ObjectKey* key, PropertyName* name,
                                  TemporaryTypeSet* observed, bool updateObserved)
{
    // If this access has never executed, try to add types to the observed set
    // according to any property which exists on the object or its prototype.
    if (updateObserved && observed->empty() && name) {
        JSObject* obj;
        if (key->isSingleton())
            obj = key->singleton();
        else
            obj = key->proto().toObjectOrNull();

        while (obj) {
            if (!obj->getClass()->isNative())
                break;

            TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(obj);
            if (propertycx)
                key->ensureTrackedProperty(propertycx, NameToId(name));

            if (!key->unknownProperties()) {
                HeapTypeSetKey property = key->property(NameToId(name));
                if (property.maybeTypes()) {
                    TypeSet::TypeList types;
                    if (!property.maybeTypes()->enumerateTypes(&types))
                        break;
                    if (types.length()) {
                        // Note: the return value here is ignored.
                        observed->addType(types[0], GetJitContext()->temp->lifoAlloc());
                        break;
                    }
                }
            }

            obj = obj->getProto();
        }
    }

    return PropertyReadNeedsTypeBarrier(constraints, key, name, observed);
}

BarrierKind
jit::PropertyReadNeedsTypeBarrier(JSContext* propertycx,
                                  CompilerConstraintList* constraints,
                                  MDefinition* obj, PropertyName* name,
                                  TemporaryTypeSet* observed)
{
    if (observed->unknown())
        return BarrierKind::NoBarrier;

    TypeSet* types = obj->resultTypeSet();
    if (!types || types->unknownObject())
        return BarrierKind::TypeSet;

    BarrierKind res = BarrierKind::NoBarrier;

    bool updateObserved = types->getObjectCount() == 1;
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        if (TypeSet::ObjectKey* key = types->getObject(i)) {
            BarrierKind kind = PropertyReadNeedsTypeBarrier(propertycx, constraints, key, name,
                                                            observed, updateObserved);
            if (kind == BarrierKind::TypeSet)
                return BarrierKind::TypeSet;

            if (kind == BarrierKind::TypeTagOnly) {
                MOZ_ASSERT(res == BarrierKind::NoBarrier || res == BarrierKind::TypeTagOnly);
                res = BarrierKind::TypeTagOnly;
            } else {
                MOZ_ASSERT(kind == BarrierKind::NoBarrier);
            }
        }
    }

    return res;
}

BarrierKind
jit::PropertyReadOnPrototypeNeedsTypeBarrier(CompilerConstraintList* constraints,
                                             MDefinition* obj, PropertyName* name,
                                             TemporaryTypeSet* observed)
{
    if (observed->unknown())
        return BarrierKind::NoBarrier;

    TypeSet* types = obj->resultTypeSet();
    if (!types || types->unknownObject())
        return BarrierKind::TypeSet;

    BarrierKind res = BarrierKind::NoBarrier;

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;
        while (true) {
            if (!key->hasStableClassAndProto(constraints))
                return BarrierKind::TypeSet;
            if (!key->proto().isObject())
                break;
            key = TypeSet::ObjectKey::get(key->proto().toObject());
            BarrierKind kind = PropertyReadNeedsTypeBarrier(constraints, key, name, observed);
            if (kind == BarrierKind::TypeSet)
                return BarrierKind::TypeSet;

            if (kind == BarrierKind::TypeTagOnly) {
                MOZ_ASSERT(res == BarrierKind::NoBarrier || res == BarrierKind::TypeTagOnly);
                res = BarrierKind::TypeTagOnly;
            } else {
                MOZ_ASSERT(kind == BarrierKind::NoBarrier);
            }
        }
    }

    return res;
}

bool
jit::PropertyReadIsIdempotent(CompilerConstraintList* constraints,
                              MDefinition* obj, PropertyName* name)
{
    // Determine if reading a property from obj is likely to be idempotent.

    TypeSet* types = obj->resultTypeSet();
    if (!types || types->unknownObject())
        return false;

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        if (TypeSet::ObjectKey* key = types->getObject(i)) {
            if (key->unknownProperties())
                return false;

            // Check if the property has been reconfigured or is a getter.
            HeapTypeSetKey property = key->property(NameToId(name));
            if (property.nonData(constraints))
                return false;
        }
    }

    return true;
}

void
jit::AddObjectsForPropertyRead(MDefinition* obj, PropertyName* name,
                               TemporaryTypeSet* observed)
{
    // Add objects to observed which *could* be observed by reading name from obj,
    // to hopefully avoid unnecessary type barriers and code invalidations.

    LifoAlloc* alloc = GetJitContext()->temp->lifoAlloc();

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types || types->unknownObject()) {
        observed->addType(TypeSet::AnyObjectType(), alloc);
        return;
    }

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties()) {
            observed->addType(TypeSet::AnyObjectType(), alloc);
            return;
        }

        jsid id = name ? NameToId(name) : JSID_VOID;
        HeapTypeSetKey property = key->property(id);
        HeapTypeSet* types = property.maybeTypes();
        if (!types)
            continue;

        if (types->unknownObject()) {
            observed->addType(TypeSet::AnyObjectType(), alloc);
            return;
        }

        for (size_t i = 0; i < types->getObjectCount(); i++) {
            if (TypeSet::ObjectKey* key = types->getObject(i))
                observed->addType(TypeSet::ObjectType(key), alloc);
        }
    }
}

static bool
PropertyTypeIncludes(TempAllocator& alloc, HeapTypeSetKey property,
                     MDefinition* value, MIRType implicitType)
{
    // If implicitType is not MIRType_None, it is an additional type which the
    // property implicitly includes. In this case, make a new type set which
    // explicitly contains the type.
    TypeSet* types = property.maybeTypes();
    if (implicitType != MIRType_None) {
        TypeSet::Type newType = TypeSet::PrimitiveType(ValueTypeFromMIRType(implicitType));
        if (types)
            types = types->clone(alloc.lifoAlloc());
        else
            types = alloc.lifoAlloc()->new_<TemporaryTypeSet>();
        types->addType(newType, alloc.lifoAlloc());
    }

    return TypeSetIncludes(types, value->type(), value->resultTypeSet());
}

static bool
TryAddTypeBarrierForWrite(TempAllocator& alloc, CompilerConstraintList* constraints,
                          MBasicBlock* current, TemporaryTypeSet* objTypes,
                          PropertyName* name, MDefinition** pvalue, MIRType implicitType)
{
    // Return whether pvalue was modified to include a type barrier ensuring
    // that writing the value to objTypes/id will not require changing type
    // information.

    // All objects in the set must have the same types for name. Otherwise, we
    // could bail out without subsequently triggering a type change that
    // invalidates the compiled code.
    Maybe<HeapTypeSetKey> aggregateProperty;

    for (size_t i = 0; i < objTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = objTypes->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return false;

        jsid id = name ? NameToId(name) : JSID_VOID;
        HeapTypeSetKey property = key->property(id);
        if (!property.maybeTypes() || property.couldBeConstant(constraints))
            return false;

        if (PropertyTypeIncludes(alloc, property, *pvalue, implicitType))
            return false;

        // This freeze is not required for correctness, but ensures that we
        // will recompile if the property types change and the barrier can
        // potentially be removed.
        property.freeze(constraints);

        if (!aggregateProperty) {
            aggregateProperty.emplace(property);
        } else {
            if (!aggregateProperty->maybeTypes()->equals(property.maybeTypes()))
                return false;
        }
    }

    MOZ_ASSERT(aggregateProperty);

    MIRType propertyType = aggregateProperty->knownMIRType(constraints);
    switch (propertyType) {
      case MIRType_Boolean:
      case MIRType_Int32:
      case MIRType_Double:
      case MIRType_String:
      case MIRType_Symbol: {
        // The property is a particular primitive type, guard by unboxing the
        // value before the write.
        if (!(*pvalue)->mightBeType(propertyType)) {
            // The value's type does not match the property type. Just do a VM
            // call as it will always trigger invalidation of the compiled code.
            MOZ_ASSERT_IF((*pvalue)->type() != MIRType_Value, (*pvalue)->type() != propertyType);
            return false;
        }
        MInstruction* ins = MUnbox::New(alloc, *pvalue, propertyType, MUnbox::Fallible);
        current->add(ins);
        *pvalue = ins;
        return true;
      }
      default:;
    }

    if ((*pvalue)->type() != MIRType_Value)
        return false;

    TemporaryTypeSet* types = aggregateProperty->maybeTypes()->clone(alloc.lifoAlloc());
    if (!types)
        return false;

    // If all possible objects can be stored without a barrier, we don't have to
    // guard on the specific object types.
    BarrierKind kind = BarrierKind::TypeSet;
    if ((*pvalue)->resultTypeSet() && (*pvalue)->resultTypeSet()->objectsAreSubset(types))
        kind = BarrierKind::TypeTagOnly;

    MInstruction* ins = MMonitorTypes::New(alloc, *pvalue, types, kind);
    current->add(ins);
    return true;
}

static MInstruction*
AddGroupGuard(TempAllocator& alloc, MBasicBlock* current, MDefinition* obj,
              TypeSet::ObjectKey* key, bool bailOnEquality)
{
    MInstruction* guard;

    if (key->isGroup()) {
        guard = MGuardObjectGroup::New(alloc, obj, key->group(), bailOnEquality,
                                       Bailout_ObjectIdentityOrTypeGuard);
    } else {
        MConstant* singletonConst = MConstant::NewConstraintlessObject(alloc, key->singleton());
        current->add(singletonConst);
        guard = MGuardObjectIdentity::New(alloc, obj, singletonConst, bailOnEquality);
    }

    current->add(guard);

    // For now, never move object group / identity guards.
    guard->setNotMovable();

    return guard;
}

// Whether value can be written to property without changing type information.
bool
jit::CanWriteProperty(TempAllocator& alloc, CompilerConstraintList* constraints,
                      HeapTypeSetKey property, MDefinition* value,
                      MIRType implicitType /* = MIRType_None */)
{
    if (property.couldBeConstant(constraints))
        return false;
    return PropertyTypeIncludes(alloc, property, value, implicitType);
}

bool
jit::PropertyWriteNeedsTypeBarrier(TempAllocator& alloc, CompilerConstraintList* constraints,
                                   MBasicBlock* current, MDefinition** pobj,
                                   PropertyName* name, MDefinition** pvalue,
                                   bool canModify, MIRType implicitType)
{
    // If any value being written is not reflected in the type information for
    // objects which obj could represent, a type barrier is needed when writing
    // the value. As for propertyReadNeedsTypeBarrier, this only applies for
    // properties that are accounted for by type information, i.e. normal data
    // properties and elements.

    TemporaryTypeSet* types = (*pobj)->resultTypeSet();
    if (!types || types->unknownObject())
        return true;

    // If all of the objects being written to have property types which already
    // reflect the value, no barrier at all is needed. Additionally, if all
    // objects being written to have the same types for the property, and those
    // types do *not* reflect the value, add a type barrier for the value.

    bool success = true;
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key || key->unknownProperties())
            continue;

        // TI doesn't track TypedArray indexes and should never insert a type
        // barrier for them.
        if (!name && IsAnyTypedArrayClass(key->clasp()))
            continue;

        jsid id = name ? NameToId(name) : JSID_VOID;
        HeapTypeSetKey property = key->property(id);
        if (!CanWriteProperty(alloc, constraints, property, *pvalue, implicitType)) {
            // Either pobj or pvalue needs to be modified to filter out the
            // types which the value could have but are not in the property,
            // or a VM call is required. A VM call is always required if pobj
            // and pvalue cannot be modified.
            if (!canModify)
                return true;
            success = TryAddTypeBarrierForWrite(alloc, constraints, current, types, name, pvalue,
                                                implicitType);
            break;
        }
    }

    if (success)
        return false;

    // If all of the objects except one have property types which reflect the
    // value, and the remaining object has no types at all for the property,
    // add a guard that the object does not have that remaining object's type.

    if (types->getObjectCount() <= 1)
        return true;

    TypeSet::ObjectKey* excluded = nullptr;
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key || key->unknownProperties())
            continue;
        if (!name && IsAnyTypedArrayClass(key->clasp()))
            continue;

        jsid id = name ? NameToId(name) : JSID_VOID;
        HeapTypeSetKey property = key->property(id);
        if (CanWriteProperty(alloc, constraints, property, *pvalue, implicitType))
            continue;

        if ((property.maybeTypes() && !property.maybeTypes()->empty()) || excluded)
            return true;
        excluded = key;
    }

    MOZ_ASSERT(excluded);

    // If the excluded object is a group with an unboxed layout, make sure it
    // does not have a corresponding native group. Objects with the native
    // group might appear even though they are not in the type set.
    if (excluded->isGroup()) {
        if (UnboxedLayout* layout = excluded->group()->maybeUnboxedLayout()) {
            if (layout->nativeGroup())
                return true;
            excluded->watchStateChangeForUnboxedConvertedToNative(constraints);
        }
    }

    *pobj = AddGroupGuard(alloc, current, *pobj, excluded, /* bailOnEquality = */ true);
    return false;
}
