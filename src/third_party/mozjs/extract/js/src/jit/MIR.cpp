/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIR.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/MathAlgorithms.h"

#include <ctype.h>

#include "jslibmath.h"

#include "builtin/RegExp.h"
#include "builtin/String.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineInspector.h"
#include "jit/IonBuilder.h"
#include "jit/JitSpewer.h"
#include "jit/MIRGraph.h"
#include "jit/RangeAnalysis.h"
#include "js/Conversions.h"
#include "util/Text.h"

#include "jsboolinlines.h"

#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::jit;

using JS::ToInt32;

using mozilla::CheckedInt;
using mozilla::NumbersAreIdentical;
using mozilla::IsFloat32Representable;
using mozilla::IsNaN;
using mozilla::IsPowerOfTwo;
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
CheckUsesAreFloat32Consumers(const MInstruction* ins)
{
    bool allConsumerUses = true;
    for (MUseDefIterator use(ins); allConsumerUses && use; use++)
        allConsumerUses &= use.def()->canConsumeFloat32(use.use());
    return allConsumerUses;
}

void
MDefinition::PrintOpcodeName(GenericPrinter& out, MDefinition::Opcode op)
{
    static const char * const names[] =
    {
#define NAME(x) #x,
        MIR_OPCODE_LIST(NAME)
#undef NAME
    };
    const char* name = names[unsigned(op)];
    size_t len = strlen(name);
    for (size_t i = 0; i < len; i++)
        out.printf("%c", tolower(name[i]));
}

static MConstant*
EvaluateConstantOperands(TempAllocator& alloc, MBinaryInstruction* ins, bool* ptypeChange = nullptr)
{
    MDefinition* left = ins->getOperand(0);
    MDefinition* right = ins->getOperand(1);

    MOZ_ASSERT(IsTypeRepresentableAsDouble(left->type()));
    MOZ_ASSERT(IsTypeRepresentableAsDouble(right->type()));

    if (!left->isConstant() || !right->isConstant())
        return nullptr;

    MConstant* lhs = left->toConstant();
    MConstant* rhs = right->toConstant();
    double ret = JS::GenericNaN();

    switch (ins->op()) {
      case MDefinition::Opcode::BitAnd:
        ret = double(lhs->toInt32() & rhs->toInt32());
        break;
      case MDefinition::Opcode::BitOr:
        ret = double(lhs->toInt32() | rhs->toInt32());
        break;
      case MDefinition::Opcode::BitXor:
        ret = double(lhs->toInt32() ^ rhs->toInt32());
        break;
      case MDefinition::Opcode::Lsh:
        ret = double(uint32_t(lhs->toInt32()) << (rhs->toInt32() & 0x1F));
        break;
      case MDefinition::Opcode::Rsh:
        ret = double(lhs->toInt32() >> (rhs->toInt32() & 0x1F));
        break;
      case MDefinition::Opcode::Ursh:
        ret = double(uint32_t(lhs->toInt32()) >> (rhs->toInt32() & 0x1F));
        break;
      case MDefinition::Opcode::Add:
        ret = lhs->numberToDouble() + rhs->numberToDouble();
        break;
      case MDefinition::Opcode::Sub:
        ret = lhs->numberToDouble() - rhs->numberToDouble();
        break;
      case MDefinition::Opcode::Mul:
        ret = lhs->numberToDouble() * rhs->numberToDouble();
        break;
      case MDefinition::Opcode::Div:
        if (ins->toDiv()->isUnsigned()) {
            if (rhs->isInt32(0)) {
                if (ins->toDiv()->trapOnError())
                    return nullptr;
                ret = 0.0;
            } else {
                ret = double(uint32_t(lhs->toInt32()) / uint32_t(rhs->toInt32()));
            }
        } else {
            ret = NumberDiv(lhs->numberToDouble(), rhs->numberToDouble());
        }
        break;
      case MDefinition::Opcode::Mod:
        if (ins->toMod()->isUnsigned()) {
            if (rhs->isInt32(0)) {
                if (ins->toMod()->trapOnError())
                    return nullptr;
                ret = 0.0;
            } else {
                ret = double(uint32_t(lhs->toInt32()) % uint32_t(rhs->toInt32()));
            }
        } else {
            ret = NumberMod(lhs->numberToDouble(), rhs->numberToDouble());
        }
        break;
      default:
        MOZ_CRASH("NYI");
    }

    if (ins->type() == MIRType::Float32)
        return MConstant::NewFloat32(alloc, float(ret));
    if (ins->type() == MIRType::Double)
        return MConstant::New(alloc, DoubleValue(ret));

    Value retVal;
    retVal.setNumber(JS::CanonicalizeNaN(ret));

    // If this was an int32 operation but the result isn't an int32 (for
    // example, a division where the numerator isn't evenly divisible by the
    // denominator), decline folding.
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    if (!retVal.isInt32()) {
        if (ptypeChange)
            *ptypeChange = true;
        return nullptr;
    }

    return MConstant::New(alloc, retVal);
}

static MMul*
EvaluateExactReciprocal(TempAllocator& alloc, MDiv* ins)
{
    // we should fold only when it is a floating point operation
    if (!IsFloatingPointType(ins->type()))
        return nullptr;

    MDefinition* left = ins->getOperand(0);
    MDefinition* right = ins->getOperand(1);

    if (!right->isConstant())
        return nullptr;

    int32_t num;
    if (!mozilla::NumberIsInt32(right->toConstant()->numberToDouble(), &num))
        return nullptr;

    // check if rhs is a power of two
    if (mozilla::Abs(num) & (mozilla::Abs(num) - 1))
        return nullptr;

    Value ret;
    ret.setDouble(1.0 / double(num));

    MConstant* foldedRhs;
    if (ins->type() == MIRType::Float32)
        foldedRhs = MConstant::NewFloat32(alloc, ret.toDouble());
    else
        foldedRhs = MConstant::New(alloc, ret);

    MOZ_ASSERT(foldedRhs->type() == ins->type());
    ins->block()->insertBefore(ins, foldedRhs);

    MMul* mul = MMul::New(alloc, left, foldedRhs, ins->type());
    mul->setCommutative();
    mul->setMustPreserveNaN(ins->mustPreserveNaN());
    return mul;
}

void
MDefinition::printName(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf("%u", id());
}

HashNumber
MDefinition::valueHash() const
{
    HashNumber out = HashNumber(op());
    for (size_t i = 0, e = numOperands(); i < e; i++)
        out = addU32ToHash(out, getOperand(i)->id());
    if (MDefinition* dep = dependency())
        out = addU32ToHash(out, dep->id());
    return out;
}

HashNumber
MNullaryInstruction::valueHash() const
{
    HashNumber hash = HashNumber(op());
    if (MDefinition* dep = dependency())
        hash = addU32ToHash(hash, dep->id());
    MOZ_ASSERT(hash == MDefinition::valueHash());
    return hash;
}

HashNumber
MUnaryInstruction::valueHash() const
{
    HashNumber hash = HashNumber(op());
    hash = addU32ToHash(hash, getOperand(0)->id());
    if (MDefinition* dep = dependency())
        hash = addU32ToHash(hash, dep->id());
    MOZ_ASSERT(hash == MDefinition::valueHash());
    return hash;
}

HashNumber
MBinaryInstruction::valueHash() const
{
    HashNumber hash = HashNumber(op());
    hash = addU32ToHash(hash, getOperand(0)->id());
    hash = addU32ToHash(hash, getOperand(1)->id());
    if (MDefinition* dep = dependency())
        hash = addU32ToHash(hash, dep->id());
    MOZ_ASSERT(hash == MDefinition::valueHash());
    return hash;
}

HashNumber
MTernaryInstruction::valueHash() const
{
    HashNumber hash = HashNumber(op());
    hash = addU32ToHash(hash, getOperand(0)->id());
    hash = addU32ToHash(hash, getOperand(1)->id());
    hash = addU32ToHash(hash, getOperand(2)->id());
    if (MDefinition* dep = dependency())
        hash = addU32ToHash(hash, dep->id());
    MOZ_ASSERT(hash == MDefinition::valueHash());
    return hash;
}

HashNumber
MQuaternaryInstruction::valueHash() const
{
    HashNumber hash = HashNumber(op());
    hash = addU32ToHash(hash, getOperand(0)->id());
    hash = addU32ToHash(hash, getOperand(1)->id());
    hash = addU32ToHash(hash, getOperand(2)->id());
    hash = addU32ToHash(hash, getOperand(3)->id());
    if (MDefinition* dep = dependency())
        hash = addU32ToHash(hash, dep->id());
    MOZ_ASSERT(hash == MDefinition::valueHash());
    return hash;
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

    if (MIRType::Value != type())
        return false;

    return !resultTypeSet() || resultTypeSet()->hasType(TypeSet::MagicArgType());
}

MDefinition*
MInstruction::foldsToStore(TempAllocator& alloc)
{
    if (!dependency())
        return nullptr;

    MDefinition* store = dependency();
    if (mightAlias(store) != AliasType::MustAlias)
        return nullptr;

    if (!store->block()->dominates(block()))
        return nullptr;

    MDefinition* value;
    switch (store->op()) {
      case Opcode::StoreFixedSlot:
        value = store->toStoreFixedSlot()->value();
        break;
      case Opcode::StoreSlot:
        value = store->toStoreSlot()->value();
        break;
      case Opcode::StoreElement:
        value = store->toStoreElement()->value();
        break;
      case Opcode::StoreUnboxedObjectOrNull:
        value = store->toStoreUnboxedObjectOrNull()->value();
        break;
      default:
        MOZ_CRASH("unknown store");
    }

    // If the type are matching then we return the value which is used as
    // argument of the store.
    if (value->type() != type()) {
        // If we expect to read a type which is more generic than the type seen
        // by the store, then we box the value used by the store.
        if (type() != MIRType::Value)
            return nullptr;
        // We cannot unbox ObjectOrNull yet.
        if (value->type() == MIRType::ObjectOrNull)
            return nullptr;

        MOZ_ASSERT(value->type() < MIRType::Value);
        MBox* box = MBox::New(alloc, value);
        value = box;
    }

    return value;
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

bool
MDefinition::maybeEmulatesUndefined(CompilerConstraintList* constraints)
{
    if (!mightBeType(MIRType::Object))
        return false;

    TemporaryTypeSet* types = resultTypeSet();
    if (!types)
        return true;

    return types->maybeEmulatesUndefined(constraints);
}

static bool
MaybeCallable(CompilerConstraintList* constraints, MDefinition* op)
{
    if (!op->mightBeType(MIRType::Object))
        return false;

    TemporaryTypeSet* types = op->resultTypeSet();
    if (!types)
        return true;

    return types->maybeCallable(constraints);
}

/* static */ const char*
AliasSet::Name(size_t flag)
{
    switch(flag) {
      case 0: return "ObjectFields";
      case 1: return "Element";
      case 2: return "UnboxedElement";
      case 3: return "DynamicSlot";
      case 4: return "FixedSlot";
      case 5: return "DOMProperty";
      case 6: return "FrameArgument";
      case 7: return "WasmGlobalVar";
      case 8: return "WasmHeap";
      case 9: return "TypedArrayLength";
      default:
        MOZ_CRASH("Unknown flag");
    }
}

void
MTest::cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(operandMightEmulateUndefined());

    if (!getOperand(0)->maybeEmulatesUndefined(constraints))
        markNoOperandEmulatesUndefined();
}

MDefinition*
MTest::foldsDoubleNegation(TempAllocator& alloc)
{
    MDefinition* op = getOperand(0);

    if (op->isNot()) {
        // If the operand of the Not is itself a Not, they cancel out.
        MDefinition* opop = op->getOperand(0);
        if (opop->isNot())
            return MTest::New(alloc, opop->toNot()->input(), ifTrue(), ifFalse());
        return MTest::New(alloc, op->toNot()->input(), ifFalse(), ifTrue());
    }
    return nullptr;
}

MDefinition*
MTest::foldsConstant(TempAllocator& alloc)
{
    MDefinition* op = getOperand(0);
    if (MConstant* opConst = op->maybeConstantValue()) {
        bool b;
        if (opConst->valueToBoolean(&b))
            return MGoto::New(alloc, b ? ifTrue() : ifFalse());
    }
    return nullptr;
}

MDefinition*
MTest::foldsTypes(TempAllocator& alloc)
{
    MDefinition* op = getOperand(0);

    switch (op->type()) {
      case MIRType::Undefined:
      case MIRType::Null:
        return MGoto::New(alloc, ifFalse());
      case MIRType::Symbol:
        return MGoto::New(alloc, ifTrue());
      case MIRType::Object:
        if (!operandMightEmulateUndefined())
            return MGoto::New(alloc, ifTrue());
        break;
      default:
        break;
    }
    return nullptr;
}

MDefinition*
MTest::foldsNeedlessControlFlow(TempAllocator& alloc)
{
    for (MInstructionIterator iter(ifTrue()->begin()), end(ifTrue()->end()); iter != end; ) {
        MInstruction* ins = *iter++;
        if (ins->isNop() || ins->isGoto())
            continue;
        if (ins->hasUses())
            return nullptr;
        if (!DeadIfUnused(ins))
            return nullptr;
    }

    for (MInstructionIterator iter(ifFalse()->begin()), end(ifFalse()->end()); iter != end; ) {
        MInstruction* ins = *iter++;
        if (ins->isNop() || ins->isGoto())
            continue;
        if (ins->hasUses())
            return nullptr;
        if (!DeadIfUnused(ins))
            return nullptr;
    }

    if (ifTrue()->numSuccessors() != 1 || ifFalse()->numSuccessors() != 1)
        return nullptr;
    if (ifTrue()->getSuccessor(0) != ifFalse()->getSuccessor(0))
        return nullptr;

    if (ifTrue()->successorWithPhis())
        return nullptr;

    return MGoto::New(alloc, ifTrue());
}

MDefinition*
MTest::foldsTo(TempAllocator& alloc)
{

    if (MDefinition* def = foldsDoubleNegation(alloc))
        return def;

    if (MDefinition* def = foldsConstant(alloc))
        return def;

    if (MDefinition* def = foldsTypes(alloc))
        return def;

    if (MDefinition* def = foldsNeedlessControlFlow(alloc))
        return def;

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
MDefinition::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    for (size_t j = 0, e = numOperands(); j < e; j++) {
        out.printf(" ");
        if (getUseFor(j)->hasProducer())
            getOperand(j)->printName(out);
        else
            out.printf("(null)");
    }
}

void
MDefinition::dump(GenericPrinter& out) const
{
    printName(out);
    out.printf(" = ");
    printOpcode(out);
    out.printf("\n");

    if (isInstruction()) {
        if (MResumePoint* resume = toInstruction()->resumePoint())
            resume->dump(out);
    }
}

void
MDefinition::dump() const
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}

void
MDefinition::dumpLocation(GenericPrinter& out) const
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
        out.printf("  %s %s:%d\n", linkWord, script->filename(), lineno);
        rp = rp->caller();
        linkWord = "in";
    }
}

void
MDefinition::dumpLocation() const
{
    Fprinter out(stderr);
    dumpLocation(out);
    out.finish();
}

#if defined(DEBUG) || defined(JS_JITSPEW)
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

    // Carry over the fact the value has uses which are no longer inspectable
    // with the graph.
    if (isUseRemoved())
        dom->setUseRemovedUnchecked();

    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ++i)
        i->setProducerUnchecked(dom);
    dom->uses_.takeElements(uses_);
}

void
MDefinition::justReplaceAllUsesWithExcept(MDefinition* dom)
{
    MOZ_ASSERT(dom != nullptr);
    MOZ_ASSERT(dom != this);

    // Carry over the fact the value has uses which are no longer inspectable
    // with the graph.
    if (isUseRemoved())
        dom->setUseRemovedUnchecked();

    // Move all uses to new dom. Save the use of the dominating instruction.
    MUse *exceptUse = nullptr;
    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ++i) {
        if (i->consumer() != dom) {
            i->setProducerUnchecked(dom);
        } else {
            MOZ_ASSERT(!exceptUse);
            exceptUse = *i;
        }
    }
    dom->uses_.takeElements(uses_);

    // Restore the use to the original definition.
    dom->uses_.remove(exceptUse);
    exceptUse->setProducerUnchecked(this);
    uses_.pushFront(exceptUse);
}

bool
MDefinition::optimizeOutAllUses(TempAllocator& alloc)
{
    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
        MUse* use = *i++;
        MConstant* constant = use->consumer()->block()->optimizedOutConstant(alloc);
        if (!alloc.ensureBallast())
            return false;

        // Update the resume point operand to use the optimized-out constant.
        use->setProducerUnchecked(constant);
        constant->addUseUnchecked(use);
    }

    // Remove dangling pointers.
    this->uses_.clear();
    return true;
}

void
MDefinition::replaceAllLiveUsesWith(MDefinition* dom)
{
    for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ) {
        MUse* use = *i++;
        MNode* consumer = use->consumer();
        if (consumer->isResumePoint())
            continue;
        if (consumer->isDefinition() && consumer->toDefinition()->isRecoveredOnBailout())
            continue;

        // Update the operand to use the dominating definition.
        use->replaceProducer(dom);
    }
}

bool
MDefinition::emptyResultTypeSet() const
{
    return resultTypeSet() && resultTypeSet()->empty();
}

MConstant*
MConstant::New(TempAllocator& alloc, const Value& v, CompilerConstraintList* constraints)
{
    return new(alloc) MConstant(alloc, v, constraints);
}

MConstant*
MConstant::New(TempAllocator::Fallible alloc, const Value& v, CompilerConstraintList* constraints)
{
    return new(alloc) MConstant(alloc.alloc, v, constraints);
}

MConstant*
MConstant::NewFloat32(TempAllocator& alloc, double d)
{
    MOZ_ASSERT(IsNaN(d) || d == double(float(d)));
    return new(alloc) MConstant(float(d));
}

MConstant*
MConstant::NewInt64(TempAllocator& alloc, int64_t i)
{
    return new(alloc) MConstant(i);
}

MConstant*
MConstant::New(TempAllocator& alloc, const Value& v, MIRType type)
{
    if (type == MIRType::Float32)
        return NewFloat32(alloc, v.toNumber());
    MConstant* res = New(alloc, v);
    MOZ_ASSERT(res->type() == type);
    return res;
}

MConstant*
MConstant::NewConstraintlessObject(TempAllocator& alloc, JSObject* v)
{
    return new(alloc) MConstant(v);
}

static TemporaryTypeSet*
MakeSingletonTypeSetFromKey(TempAllocator& tempAlloc, CompilerConstraintList* constraints,
                            TypeSet::ObjectKey* key)
{
    // Invalidate when this object's ObjectGroup gets unknown properties. This
    // happens for instance when we mutate an object's __proto__, in this case
    // we want to invalidate and mark this TypeSet as containing AnyObject
    // (because mutating __proto__ will change an object's ObjectGroup).
    MOZ_ASSERT(constraints);
    (void)key->hasStableClassAndProto(constraints);

    LifoAlloc* alloc = tempAlloc.lifoAlloc();
    return alloc->new_<TemporaryTypeSet>(alloc, TypeSet::ObjectType(key));
}

TemporaryTypeSet*
jit::MakeSingletonTypeSet(TempAllocator& alloc, CompilerConstraintList* constraints, JSObject* obj)
{
    return MakeSingletonTypeSetFromKey(alloc, constraints, TypeSet::ObjectKey::get(obj));
}

TemporaryTypeSet*
jit::MakeSingletonTypeSet(TempAllocator& alloc, CompilerConstraintList* constraints, ObjectGroup* obj)
{
    return MakeSingletonTypeSetFromKey(alloc, constraints, TypeSet::ObjectKey::get(obj));
}

static TemporaryTypeSet*
MakeUnknownTypeSet(TempAllocator& tempAlloc)
{
    LifoAlloc* alloc = tempAlloc.lifoAlloc();
    return alloc->new_<TemporaryTypeSet>(alloc, TypeSet::UnknownType());
}

#ifdef DEBUG

bool
jit::IonCompilationCanUseNurseryPointers()
{
    // If we are doing backend compilation, which could occur on a helper
    // thread but might actually be on the active thread, check the flag set on
    // the JSContext by AutoEnterIonCompilation.
    if (CurrentThreadIsIonCompiling())
        return !CurrentThreadIsIonCompilingSafeForMinorGC();

    // Otherwise, we must be on the active thread during MIR construction. The
    // store buffer must have been notified that minor GCs must cancel pending
    // or in progress Ion compilations.
    JSContext* cx = TlsContext.get();
    return cx->zone()->group()->storeBuffer().cancelIonCompilations();
}

#endif // DEBUG

MConstant::MConstant(TempAllocator& alloc, const js::Value& vp, CompilerConstraintList* constraints)
  : MNullaryInstruction(classOpcode)
{
    setResultType(MIRTypeFromValue(vp));

    MOZ_ASSERT(payload_.asBits == 0);

    switch (type()) {
      case MIRType::Undefined:
      case MIRType::Null:
        break;
      case MIRType::Boolean:
        payload_.b = vp.toBoolean();
        break;
      case MIRType::Int32:
        payload_.i32 = vp.toInt32();
        break;
      case MIRType::Double:
        payload_.d = vp.toDouble();
        break;
      case MIRType::String:
        MOZ_ASSERT(vp.toString()->isAtom());
        payload_.str = vp.toString();
        break;
      case MIRType::Symbol:
        payload_.sym = vp.toSymbol();
        break;
      case MIRType::Object:
        payload_.obj = &vp.toObject();
        // Create a singleton type set for the object. This isn't necessary for
        // other types as the result type encodes all needed information.
        MOZ_ASSERT_IF(IsInsideNursery(&vp.toObject()), IonCompilationCanUseNurseryPointers());
        setResultTypeSet(MakeSingletonTypeSet(alloc, constraints, &vp.toObject()));
        break;
      case MIRType::MagicOptimizedArguments:
      case MIRType::MagicOptimizedOut:
      case MIRType::MagicHole:
      case MIRType::MagicIsConstructing:
        break;
      case MIRType::MagicUninitializedLexical:
        // JS_UNINITIALIZED_LEXICAL does not escape to script and is not
        // observed in type sets. However, it may flow around freely during
        // Ion compilation. Give it an unknown typeset to poison any type sets
        // it merges with.
        //
        // TODO We could track uninitialized lexicals more precisely by tracking
        // them in type sets.
        setResultTypeSet(MakeUnknownTypeSet(alloc));
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }

    setMovable();
}

MConstant::MConstant(JSObject* obj)
  : MNullaryInstruction(classOpcode)
{
    MOZ_ASSERT_IF(IsInsideNursery(obj), IonCompilationCanUseNurseryPointers());
    setResultType(MIRType::Object);
    payload_.obj = obj;
    setMovable();
}

MConstant::MConstant(float f)
  : MNullaryInstruction(classOpcode)
{
    setResultType(MIRType::Float32);
    payload_.f = f;
    setMovable();
}

MConstant::MConstant(int64_t i)
  : MNullaryInstruction(classOpcode)
{
    setResultType(MIRType::Int64);
    payload_.i64 = i;
    setMovable();
}

#ifdef DEBUG
void
MConstant::assertInitializedPayload() const
{
    // valueHash() and equals() expect the unused payload bits to be
    // initialized to zero. Assert this in debug builds.

    switch (type()) {
      case MIRType::Int32:
      case MIRType::Float32:
#if MOZ_LITTLE_ENDIAN
        MOZ_ASSERT((payload_.asBits >> 32) == 0);
#else
        MOZ_ASSERT((payload_.asBits << 32) == 0);
#endif
        break;
      case MIRType::Boolean:
#if MOZ_LITTLE_ENDIAN
        MOZ_ASSERT((payload_.asBits >> 1) == 0);
#else
        MOZ_ASSERT((payload_.asBits & ~(1ULL << 56)) == 0);
#endif
        break;
      case MIRType::Double:
      case MIRType::Int64:
        break;
      case MIRType::String:
      case MIRType::Object:
      case MIRType::Symbol:
#if MOZ_LITTLE_ENDIAN
        MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits >> 32) == 0);
#else
        MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits << 32) == 0);
#endif
        break;
      default:
        MOZ_ASSERT(IsNullOrUndefined(type()) || IsMagicType(type()));
        MOZ_ASSERT(payload_.asBits == 0);
        break;
    }
}
#endif

static HashNumber
ConstantValueHash(MIRType type, uint64_t payload)
{
    // Build a 64-bit value holding both the payload and the type.
    static const size_t TypeBits = 8;
    static const size_t TypeShift = 64 - TypeBits;
    MOZ_ASSERT(uintptr_t(type) <= (1 << TypeBits) - 1);
    uint64_t bits = (uint64_t(type) << TypeShift) ^ payload;

    // Fold all 64 bits into the 32-bit result. It's tempting to just discard
    // half of the bits, as this is just a hash, however there are many common
    // patterns of values where only the low or the high bits vary, so
    // discarding either side would lead to excessive hash collisions.
    return (HashNumber)bits ^ (HashNumber)(bits >> 32);
}

HashNumber
MConstant::valueHash() const
{
    static_assert(sizeof(Payload) == sizeof(uint64_t),
                  "Code below assumes payload fits in 64 bits");

    assertInitializedPayload();
    return ConstantValueHash(type(), payload_.asBits);
}

bool
MConstant::congruentTo(const MDefinition* ins) const
{
    return ins->isConstant() && equals(ins->toConstant());
}

void
MConstant::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf(" ");
    switch (type()) {
      case MIRType::Undefined:
        out.printf("undefined");
        break;
      case MIRType::Null:
        out.printf("null");
        break;
      case MIRType::Boolean:
        out.printf(toBoolean() ? "true" : "false");
        break;
      case MIRType::Int32:
        out.printf("0x%x", toInt32());
        break;
      case MIRType::Int64:
        out.printf("0x%" PRIx64, toInt64());
        break;
      case MIRType::Double:
        out.printf("%.16g", toDouble());
        break;
      case MIRType::Float32:
      {
        float val = toFloat32();
        out.printf("%.16g", val);
        break;
      }
      case MIRType::Object:
        if (toObject().is<JSFunction>()) {
            JSFunction* fun = &toObject().as<JSFunction>();
            if (fun->displayAtom()) {
                out.put("function ");
                EscapedStringPrinter(out, fun->displayAtom(), 0);
            } else {
                out.put("unnamed function");
            }
            if (fun->hasScript()) {
                JSScript* script = fun->nonLazyScript();
                out.printf(" (%s:%zu)",
                        script->filename() ? script->filename() : "", script->lineno());
            }
            out.printf(" at %p", (void*) fun);
            break;
        }
        out.printf("object %p (%s)", (void*)&toObject(), toObject().getClass()->name);
        break;
      case MIRType::Symbol:
        out.printf("symbol at %p", (void*)toSymbol());
        break;
      case MIRType::String:
        out.printf("string %p", (void*)toString());
        break;
      case MIRType::MagicOptimizedArguments:
        out.printf("magic lazyargs");
        break;
      case MIRType::MagicHole:
        out.printf("magic hole");
        break;
      case MIRType::MagicIsConstructing:
        out.printf("magic is-constructing");
        break;
      case MIRType::MagicOptimizedOut:
        out.printf("magic optimized-out");
        break;
      case MIRType::MagicUninitializedLexical:
        out.printf("magic uninitialized-lexical");
        break;
      default:
        MOZ_CRASH("unexpected type");
    }
}

bool
MConstant::canProduceFloat32() const
{
    if (!isTypeRepresentableAsDouble())
        return false;

    if (type() == MIRType::Int32)
        return IsFloat32Representable(static_cast<double>(toInt32()));
    if (type() == MIRType::Double)
        return IsFloat32Representable(toDouble());
    MOZ_ASSERT(type() == MIRType::Float32);
    return true;
}

Value
MConstant::toJSValue() const
{
    // Wasm has types like int64 that cannot be stored as js::Value. It also
    // doesn't want the NaN canonicalization enforced by js::Value.
    MOZ_ASSERT(!IsCompilingWasm());

    switch (type()) {
      case MIRType::Undefined:
        return UndefinedValue();
      case MIRType::Null:
        return NullValue();
      case MIRType::Boolean:
        return BooleanValue(toBoolean());
      case MIRType::Int32:
        return Int32Value(toInt32());
      case MIRType::Double:
        return DoubleValue(toDouble());
      case MIRType::Float32:
        return Float32Value(toFloat32());
      case MIRType::String:
        return StringValue(toString());
      case MIRType::Symbol:
        return SymbolValue(toSymbol());
      case MIRType::Object:
        return ObjectValue(toObject());
      case MIRType::MagicOptimizedArguments:
        return MagicValue(JS_OPTIMIZED_ARGUMENTS);
      case MIRType::MagicOptimizedOut:
        return MagicValue(JS_OPTIMIZED_OUT);
      case MIRType::MagicHole:
        return MagicValue(JS_ELEMENTS_HOLE);
      case MIRType::MagicIsConstructing:
        return MagicValue(JS_IS_CONSTRUCTING);
      case MIRType::MagicUninitializedLexical:
        return MagicValue(JS_UNINITIALIZED_LEXICAL);
      default:
        MOZ_CRASH("Unexpected type");
    }
}

bool
MConstant::valueToBoolean(bool* res) const
{
    switch (type()) {
      case MIRType::Boolean:
        *res = toBoolean();
        return true;
      case MIRType::Int32:
        *res = toInt32() != 0;
        return true;
      case MIRType::Int64:
        *res = toInt64() != 0;
        return true;
      case MIRType::Double:
        *res = !mozilla::IsNaN(toDouble()) && toDouble() != 0.0;
        return true;
      case MIRType::Float32:
        *res = !mozilla::IsNaN(toFloat32()) && toFloat32() != 0.0f;
        return true;
      case MIRType::Null:
      case MIRType::Undefined:
        *res = false;
        return true;
      case MIRType::Symbol:
        *res = true;
        return true;
      case MIRType::String:
        *res = toString()->length() != 0;
        return true;
      case MIRType::Object:
        *res = !EmulatesUndefined(&toObject());
        return true;
      default:
        MOZ_ASSERT(IsMagicType(type()));
        return false;
    }
}

HashNumber
MWasmFloatConstant::valueHash() const
{
    return ConstantValueHash(type(), u.bits_);
}

bool
MWasmFloatConstant::congruentTo(const MDefinition* ins) const
{
    return ins->isWasmFloatConstant() &&
           type() == ins->type() &&
           u.bits_ == ins->toWasmFloatConstant()->u.bits_;
}

MDefinition*
MSimdValueX4::foldsTo(TempAllocator& alloc)
{
#ifdef DEBUG
    MIRType laneType = SimdTypeToLaneArgumentType(type());
#endif
    bool allConstants = true;
    bool allSame = true;

    for (size_t i = 0; i < 4; ++i) {
        MDefinition* op = getOperand(i);
        MOZ_ASSERT(op->type() == laneType);
        if (!op->isConstant())
            allConstants = false;
        if (i > 0 && op != getOperand(i - 1))
            allSame = false;
    }

    if (!allConstants && !allSame)
        return this;

    if (allConstants) {
        SimdConstant cst;
        switch (type()) {
          case MIRType::Bool32x4: {
            int32_t a[4];
            for (size_t i = 0; i < 4; ++i)
                a[i] = getOperand(i)->toConstant()->valueToBooleanInfallible() ? -1 : 0;
            cst = SimdConstant::CreateX4(a);
            break;
          }
          case MIRType::Int32x4: {
            int32_t a[4];
            for (size_t i = 0; i < 4; ++i)
                a[i] = getOperand(i)->toConstant()->toInt32();
            cst = SimdConstant::CreateX4(a);
            break;
          }
          case MIRType::Float32x4: {
            float a[4];
            for (size_t i = 0; i < 4; ++i)
                a[i] = getOperand(i)->toConstant()->numberToDouble();
            cst = SimdConstant::CreateX4(a);
            break;
          }
          default: MOZ_CRASH("unexpected type in MSimdValueX4::foldsTo");
        }

        return MSimdConstant::New(alloc, cst, type());
    }

    MOZ_ASSERT(allSame);
    return MSimdSplat::New(alloc, getOperand(0), type());
}

MDefinition*
MSimdSplat::foldsTo(TempAllocator& alloc)
{
#ifdef DEBUG
    MIRType laneType = SimdTypeToLaneArgumentType(type());
#endif
    MDefinition* op = getOperand(0);
    if (!op->isConstant())
        return this;
    MOZ_ASSERT(op->type() == laneType);

    SimdConstant cst;
    switch (type()) {
      case MIRType::Bool8x16: {
        int8_t v = op->toConstant()->valueToBooleanInfallible() ? -1 : 0;
        cst = SimdConstant::SplatX16(v);
        break;
      }
      case MIRType::Bool16x8: {
        int16_t v = op->toConstant()->valueToBooleanInfallible() ? -1 : 0;
        cst = SimdConstant::SplatX8(v);
        break;
      }
      case MIRType::Bool32x4: {
        int32_t v = op->toConstant()->valueToBooleanInfallible() ? -1 : 0;
        cst = SimdConstant::SplatX4(v);
        break;
      }
      case MIRType::Int8x16: {
        int32_t v = op->toConstant()->toInt32();
        cst = SimdConstant::SplatX16(v);
        break;
      }
      case MIRType::Int16x8: {
        int32_t v = op->toConstant()->toInt32();
        cst = SimdConstant::SplatX8(v);
        break;
      }
      case MIRType::Int32x4: {
        int32_t v = op->toConstant()->toInt32();
        cst = SimdConstant::SplatX4(v);
        break;
      }
      case MIRType::Float32x4: {
        float v = op->toConstant()->numberToDouble();
        cst = SimdConstant::SplatX4(v);
        break;
      }
      default: MOZ_CRASH("unexpected type in MSimdSplat::foldsTo");
    }

    return MSimdConstant::New(alloc, cst, type());
}

MDefinition*
MSimdUnbox::foldsTo(TempAllocator& alloc)
{
    MDefinition* in = input();

    if (in->isSimdBox()) {
        MSimdBox* box = in->toSimdBox();
        // If the operand is a MSimdBox, then we just reuse the operand of the
        // MSimdBox as long as the type corresponds to what we are supposed to
        // unbox.
        in = box->input();
        if (box->simdType() != simdType())
            return this;
        MOZ_ASSERT(in->type() == type());
        return in;
    }

    return this;
}

MDefinition*
MSimdSwizzle::foldsTo(TempAllocator& alloc)
{
    if (lanesMatch(0, 1, 2, 3))
        return input();
    return this;
}

MDefinition*
MSimdGeneralShuffle::foldsTo(TempAllocator& alloc)
{
    FixedList<uint8_t> lanes;
    if (!lanes.init(alloc, numLanes()))
        return this;

    for (size_t i = 0; i < numLanes(); i++) {
        if (!lane(i)->isConstant() || lane(i)->type() != MIRType::Int32)
            return this;
        int32_t temp = lane(i)->toConstant()->toInt32();
        if (temp < 0 || unsigned(temp) >= numLanes() * numVectors())
            return this;
        lanes[i] = uint8_t(temp);
    }

    if (numVectors() == 1)
        return MSimdSwizzle::New(alloc, vector(0), lanes.data());

    MOZ_ASSERT(numVectors() == 2);
    return MSimdShuffle::New(alloc, vector(0), vector(1), lanes.data());
}

MInstruction*
MSimdConvert::AddLegalized(TempAllocator& alloc, MBasicBlock* addTo, MDefinition* obj,
                           MIRType toType, SimdSign sign, wasm::BytecodeOffset bytecodeOffset)
{
    MIRType fromType = obj->type();

    if (SupportsUint32x4FloatConversions || sign != SimdSign::Unsigned) {
        MInstruction* ins = New(alloc, obj, toType, sign, bytecodeOffset);
        addTo->add(ins);
        return ins;
    }

    // This architecture can't do Uint32x4 <-> Float32x4 conversions (Hi SSE!)
    MOZ_ASSERT(sign == SimdSign::Unsigned);
    if (fromType == MIRType::Int32x4 && toType == MIRType::Float32x4) {
        // Converting Uint32x4 -> Float32x4. This algorithm is from LLVM.
        //
        // Split the input number into high and low parts:
        //
        // uint32_t hi = x >> 16;
        // uint32_t lo = x & 0xffff;
        //
        // Insert these parts as the low mantissa bits in a float32 number with
        // the corresponding exponent:
        //
        // float fhi = (bits-as-float)(hi | 0x53000000); // 0x1.0p39f + hi*2^16
        // float flo = (bits-as-float)(lo | 0x4b000000); // 0x1.0p23f + lo
        //
        // Subtract the bias from the hi part:
        //
        // fhi -= (0x1.0p39 + 0x1.0p23) // hi*2^16 - 0x1.0p23
        //
        // And finally combine:
        //
        // result = flo + fhi // lo + hi*2^16.

        // Compute hi = obj >> 16 (lane-wise unsigned shift).
        MInstruction* c16 = MConstant::New(alloc, Int32Value(16));
        addTo->add(c16);
        MInstruction* hi = MSimdShift::AddLegalized(alloc, addTo, obj, c16, MSimdShift::ursh);

        // Compute lo = obj & 0xffff (lane-wise).
        MInstruction* m16 =
          MSimdConstant::New(alloc, SimdConstant::SplatX4(0xffff), MIRType::Int32x4);
        addTo->add(m16);
        MInstruction* lo = MSimdBinaryBitwise::New(alloc, obj, m16, MSimdBinaryBitwise::and_);
        addTo->add(lo);

        // Mix in the exponents.
        MInstruction* exphi =
          MSimdConstant::New(alloc, SimdConstant::SplatX4(0x53000000), MIRType::Int32x4);
        addTo->add(exphi);
        MInstruction* mhi = MSimdBinaryBitwise::New(alloc, hi, exphi, MSimdBinaryBitwise::or_);
        addTo->add(mhi);
        MInstruction* explo =
          MSimdConstant::New(alloc, SimdConstant::SplatX4(0x4b000000), MIRType::Int32x4);
        addTo->add(explo);
        MInstruction* mlo = MSimdBinaryBitwise::New(alloc, lo, explo, MSimdBinaryBitwise::or_);
        addTo->add(mlo);

        // Bit-cast both to Float32x4.
        MInstruction* fhi = MSimdReinterpretCast::New(alloc, mhi, MIRType::Float32x4);
        addTo->add(fhi);
        MInstruction* flo = MSimdReinterpretCast::New(alloc, mlo, MIRType::Float32x4);
        addTo->add(flo);

        // Subtract out the bias: 0x1.0p39f + 0x1.0p23f.
        // MSVC doesn't support the hexadecimal float syntax.
        const float BiasValue = 549755813888.f + 8388608.f;
        MInstruction* bias =
          MSimdConstant::New(alloc, SimdConstant::SplatX4(BiasValue), MIRType::Float32x4);
        addTo->add(bias);
        MInstruction* fhi_debiased =
          MSimdBinaryArith::AddLegalized(alloc, addTo, fhi, bias, MSimdBinaryArith::Op_sub);

        // Compute the final result.
        return MSimdBinaryArith::AddLegalized(alloc, addTo, fhi_debiased, flo,
                                              MSimdBinaryArith::Op_add);
    }

    if (fromType == MIRType::Float32x4 && toType == MIRType::Int32x4) {
        // The Float32x4 -> Uint32x4 conversion can throw if the input is out of
        // range. This is handled by the LFloat32x4ToUint32x4 expansion.
        MInstruction* ins = New(alloc, obj, toType, sign, bytecodeOffset);
        addTo->add(ins);
        return ins;
    }

    MOZ_CRASH("Unhandled SIMD type conversion");
}

MInstruction*
MSimdBinaryComp::AddLegalized(TempAllocator& alloc, MBasicBlock* addTo, MDefinition* left,
                              MDefinition* right, Operation op, SimdSign sign)
{
    MOZ_ASSERT(left->type() == right->type());
    MIRType opType = left->type();
    MOZ_ASSERT(IsSimdType(opType));
    bool IsEquality = op == equal || op == notEqual;

    // Check if this is an unsupported unsigned compare that needs to be biased.
    // If so, put the bias vector in `bias`.
    if (sign == SimdSign::Unsigned && !IsEquality) {
        MInstruction* bias = nullptr;

        // This is an order comparison of Uint32x4 vectors which are not supported on this target.
        // Simply offset |left| and |right| by INT_MIN, then do a signed comparison.
        if (!SupportsUint32x4Compares && opType == MIRType::Int32x4)
            bias = MSimdConstant::New(alloc, SimdConstant::SplatX4(int32_t(0x80000000)), opType);
        else if (!SupportsUint16x8Compares && opType == MIRType::Int16x8)
            bias = MSimdConstant::New(alloc, SimdConstant::SplatX8(int16_t(0x8000)), opType);
        if (!SupportsUint8x16Compares && opType == MIRType::Int8x16)
            bias = MSimdConstant::New(alloc, SimdConstant::SplatX16(int8_t(0x80)), opType);

        if (bias) {
            addTo->add(bias);

            // Add the bias.
            MInstruction* bleft =
              MSimdBinaryArith::AddLegalized(alloc, addTo, left, bias, MSimdBinaryArith::Op_add);
            MInstruction* bright =
              MSimdBinaryArith::AddLegalized(alloc, addTo, right, bias, MSimdBinaryArith::Op_add);

            // Do the equivalent signed comparison.
            MInstruction* result =
              MSimdBinaryComp::New(alloc, bleft, bright, op, SimdSign::Signed);
            addTo->add(result);

            return result;
        }
    }

    if (sign == SimdSign::Unsigned &&
        ((!SupportsUint32x4Compares && opType == MIRType::Int32x4) ||
         (!SupportsUint16x8Compares && opType == MIRType::Int16x8) ||
         (!SupportsUint8x16Compares && opType == MIRType::Int8x16))) {
        // The sign doesn't matter for equality tests. Flip it to make the
        // backend assertions happy.
        MOZ_ASSERT(IsEquality);
        sign = SimdSign::Signed;
    }

    // This is a legal operation already. Just create the instruction requested.
    MInstruction* result = MSimdBinaryComp::New(alloc, left, right, op, sign);
    addTo->add(result);
    return result;
}

MInstruction*
MSimdBinaryArith::AddLegalized(TempAllocator& alloc, MBasicBlock* addTo, MDefinition* left,
                               MDefinition* right, Operation op)
{
    MOZ_ASSERT(left->type() == right->type());
    MIRType opType = left->type();
    MOZ_ASSERT(IsSimdType(opType));

    // SSE does not have 8x16 multiply instructions.
    if (opType == MIRType::Int8x16 && op == Op_mul) {
        // Express the multiply in terms of Int16x8 multiplies by handling the
        // even and odd lanes separately.

        MInstruction* wideL = MSimdReinterpretCast::New(alloc, left, MIRType::Int16x8);
        addTo->add(wideL);
        MInstruction* wideR = MSimdReinterpretCast::New(alloc, right, MIRType::Int16x8);
        addTo->add(wideR);

        // wideL = yyxx yyxx yyxx yyxx yyxx yyxx yyxx yyxx
        // wideR = bbaa bbaa bbaa bbaa bbaa bbaa bbaa bbaa

        // Shift the odd lanes down to the low bits of the 16x8 vectors.
        MInstruction* eight = MConstant::New(alloc, Int32Value(8));
        addTo->add(eight);
        MInstruction* evenL = wideL;
        MInstruction* evenR = wideR;
        MInstruction* oddL =
          MSimdShift::AddLegalized(alloc, addTo, wideL, eight, MSimdShift::ursh);
        MInstruction* oddR =
          MSimdShift::AddLegalized(alloc, addTo, wideR, eight, MSimdShift::ursh);

        // evenL = yyxx yyxx yyxx yyxx yyxx yyxx yyxx yyxx
        // evenR = bbaa bbaa bbaa bbaa bbaa bbaa bbaa bbaa
        // oddL  = 00yy 00yy 00yy 00yy 00yy 00yy 00yy 00yy
        // oddR  = 00bb 00bb 00bb 00bb 00bb 00bb 00bb 00bb

        // Now do two 16x8 multiplications. We can use the low bits of each.
        MInstruction* even = MSimdBinaryArith::AddLegalized(alloc, addTo, evenL, evenR, Op_mul);
        MInstruction* odd = MSimdBinaryArith::AddLegalized(alloc, addTo, oddL, oddR, Op_mul);

        // even = ~~PP ~~PP ~~PP ~~PP ~~PP ~~PP ~~PP ~~PP
        // odd  = ~~QQ ~~QQ ~~QQ ~~QQ ~~QQ ~~QQ ~~QQ ~~QQ

        MInstruction* mask =
          MSimdConstant::New(alloc, SimdConstant::SplatX8(int16_t(0x00ff)), MIRType::Int16x8);
        addTo->add(mask);
        even = MSimdBinaryBitwise::New(alloc, even, mask, MSimdBinaryBitwise::and_);
        addTo->add(even);
        odd = MSimdShift::AddLegalized(alloc, addTo, odd, eight, MSimdShift::lsh);

        // even = 00PP 00PP 00PP 00PP 00PP 00PP 00PP 00PP
        // odd  = QQ00 QQ00 QQ00 QQ00 QQ00 QQ00 QQ00 QQ00

        // Combine:
        MInstruction* result = MSimdBinaryBitwise::New(alloc, even, odd, MSimdBinaryBitwise::or_);
        addTo->add(result);
        result = MSimdReinterpretCast::New(alloc, result, opType);
        addTo->add(result);
        return result;
    }

    // This is a legal operation already. Just create the instruction requested.
    MInstruction* result = MSimdBinaryArith::New(alloc, left, right, op);
    addTo->add(result);
    return result;
}

MInstruction*
MSimdShift::AddLegalized(TempAllocator& alloc, MBasicBlock* addTo, MDefinition* left,
                         MDefinition* right, Operation op)
{
    MIRType opType = left->type();
    MOZ_ASSERT(IsIntegerSimdType(opType));

    // SSE does not provide 8x16 shift instructions.
    if (opType == MIRType::Int8x16) {
        // Express the shift in terms of Int16x8 shifts by splitting into even
        // and odd lanes, place 8-bit lanes into the high bits of Int16x8
        // vectors `even` and `odd`. Shift, mask, combine.
        //
        //   wide = Int16x8.fromInt8x16Bits(left);
        //   shiftBy = right & 7
        //   mask = Int16x8.splat(0xff00);
        //
        MInstruction* wide = MSimdReinterpretCast::New(alloc, left, MIRType::Int16x8);
        addTo->add(wide);

        // wide = yyxx yyxx yyxx yyxx yyxx yyxx yyxx yyxx

        MInstruction* shiftMask = MConstant::New(alloc, Int32Value(7));
        addTo->add(shiftMask);
        MBinaryBitwiseInstruction* shiftBy = MBitAnd::New(alloc, right, shiftMask);
        shiftBy->setInt32Specialization();
        addTo->add(shiftBy);

        // Move the even 8x16 lanes into the high bits of the 16x8 lanes.
        MInstruction* eight = MConstant::New(alloc, Int32Value(8));
        addTo->add(eight);
        MInstruction* even = MSimdShift::AddLegalized(alloc, addTo, wide, eight, lsh);

        // Leave the odd lanes in place.
        MInstruction* odd = wide;

        // even = xx00 xx00 xx00 xx00 xx00 xx00 xx00 xx00
        // odd  = yyxx yyxx yyxx yyxx yyxx yyxx yyxx yyxx

        MInstruction* mask =
          MSimdConstant::New(alloc, SimdConstant::SplatX8(int16_t(0xff00)), MIRType::Int16x8);
        addTo->add(mask);

        // Left-shift: Clear the low bits in `odd` before shifting.
        if (op == lsh) {
            odd = MSimdBinaryBitwise::New(alloc, odd, mask, MSimdBinaryBitwise::and_);
            addTo->add(odd);
            // odd  = yy00 yy00 yy00 yy00 yy00 yy00 yy00 yy00
        }

        // Do the real shift twice: once for the even lanes, once for the odd
        // lanes. This is a recursive call, but with a different type.
        even = MSimdShift::AddLegalized(alloc, addTo, even, shiftBy, op);
        odd = MSimdShift::AddLegalized(alloc, addTo, odd, shiftBy, op);

        // even = XX~~ XX~~ XX~~ XX~~ XX~~ XX~~ XX~~ XX~~
        // odd  = YY~~ YY~~ YY~~ YY~~ YY~~ YY~~ YY~~ YY~~

        // Right-shift: Clear the low bits in `odd` after shifting.
        if (op != lsh) {
            odd = MSimdBinaryBitwise::New(alloc, odd, mask, MSimdBinaryBitwise::and_);
            addTo->add(odd);
            // odd  = YY00 YY00 YY00 YY00 YY00 YY00 YY00 YY00
        }

        // Move the even lanes back to their original place.
        even = MSimdShift::AddLegalized(alloc, addTo, even, eight, ursh);

        // Now, `odd` contains the odd lanes properly shifted, and `even`
        // contains the even lanes properly shifted:
        //
        // even = 00XX 00XX 00XX 00XX 00XX 00XX 00XX 00XX
        // odd  = YY00 YY00 YY00 YY00 YY00 YY00 YY00 YY00
        //
        // Combine:
        MInstruction* result = MSimdBinaryBitwise::New(alloc, even, odd, MSimdBinaryBitwise::or_);
        addTo->add(result);
        result = MSimdReinterpretCast::New(alloc, result, opType);
        addTo->add(result);
        return result;
    }

    // This is a legal operation already. Just create the instruction requested.
    MInstruction* result = MSimdShift::New(alloc, left, right, op);
    addTo->add(result);
    return result;
}

template <typename T>
static void
PrintOpcodeOperation(T* mir, GenericPrinter& out)
{
    mir->MDefinition::printOpcode(out);
    out.printf(" (%s)", T::OperationName(mir->operation()));
}

void
MSimdBinaryArith::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}
void
MSimdBinarySaturating::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}
void
MSimdBinaryBitwise::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}
void
MSimdUnaryArith::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}
void
MSimdBinaryComp::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}
void
MSimdShift::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeOperation(this, out);
}

void
MSimdInsertElement::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" (lane %u)", lane());
}

void
MSimdBox::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" (%s%s)", SimdTypeToString(simdType()),
               initialHeap() == gc::TenuredHeap ? ", tenured" : "");
}

void
MSimdUnbox::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" (%s)", SimdTypeToString(simdType()));
}

void
MControlInstruction::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    for (size_t j = 0; j < numSuccessors(); j++) {
        if (getSuccessor(j))
            out.printf(" block%u", getSuccessor(j)->id());
        else
            out.printf(" (null-to-be-patched)");
    }
}

void
MCompare::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" %s", CodeName[jsop()]);
}

void
MConstantElements::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf(" 0x%" PRIxPTR, value().asValue());
}

void
MLoadUnboxedScalar::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" %s", ScalarTypeDescr::typeName(storageType()));
}

void
MAssertRange::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.put(" ");
    assertedRange()->dump(out);
}

void MNearbyInt::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    const char* roundingModeStr = nullptr;
    switch (roundingMode_) {
      case RoundingMode::Up:                roundingModeStr = "(up)"; break;
      case RoundingMode::Down:              roundingModeStr = "(down)"; break;
      case RoundingMode::NearestTiesToEven: roundingModeStr = "(nearest ties even)"; break;
      case RoundingMode::TowardsZero:       roundingModeStr = "(towards zero)"; break;
    }
    out.printf(" %s", roundingModeStr);
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
MMathFunction::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" %s", FunctionName(function()));
}

MDefinition*
MMathFunction::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (!input->isConstant() || !input->toConstant()->isTypeRepresentableAsDouble())
        return this;

    double in = input->toConstant()->numberToDouble();
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

    if (input->type() == MIRType::Float32)
        return MConstant::NewFloat32(alloc, out);
    return MConstant::New(alloc, DoubleValue(out));
}

MDefinition*
MAtomicIsLockFree::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (!input->isConstant() || input->type() != MIRType::Int32)
        return this;

    int32_t i = input->toConstant()->toInt32();
    return MConstant::New(alloc, BooleanValue(AtomicOperations::isLockfreeJS(i)));
}

// Define |THIS_SLOT| as part of this translation unit, as it is used to
// specialized the parameterized |New| function calls introduced by
// TRIVIAL_NEW_WRAPPERS.
const int32_t MParameter::THIS_SLOT;

void
MParameter::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    if (index() == THIS_SLOT)
        out.printf(" THIS_SLOT");
    else
        out.printf(" %d", index());
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

WrappedFunction::WrappedFunction(JSFunction* fun)
  : fun_(fun),
    nargs_(fun->nargs()),
    isNative_(fun->isNative()),
    isNativeWithJitEntry_(fun->isNativeWithJitEntry()),
    isConstructor_(fun->isConstructor()),
    isClassConstructor_(fun->isClassConstructor()),
    isSelfHostedBuiltin_(fun->isSelfHostedBuiltin())
{}

MCall*
MCall::New(TempAllocator& alloc, JSFunction* target, size_t maxArgc, size_t numActualArgs,
           bool construct, bool ignoresReturnValue, bool isDOMCall, DOMObjectKind objectKind)
{
    WrappedFunction* wrappedTarget = target ? new(alloc) WrappedFunction(target) : nullptr;
    MOZ_ASSERT(maxArgc >= numActualArgs);
    MCall* ins;
    if (isDOMCall) {
        MOZ_ASSERT(!construct);
        ins = new(alloc) MCallDOMNative(wrappedTarget, numActualArgs, objectKind);
    } else {
        ins = new(alloc) MCall(wrappedTarget, numActualArgs, construct, ignoresReturnValue);
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
        // The only way to reliably avoid side-effects given the information we
        // have here is if we're passing in a known primitive value to an
        // argument that expects a primitive value.
        //
        // XXXbz maybe we need to communicate better information.  For example,
        // a sequence argument will sort of unavoidably have side effects, while
        // a typed array argument won't have any, but both are claimed to be
        // JSJitInfo::Object.  But if we do that, we need to watch out for our
        // movability/DCE-ability bits: if we have an arg type that can reliably
        // throw an exception on conversion, that might not affect our alias set
        // per se, but it should prevent us being moved or DCE-ed, unless we
        // know the incoming things match that arg type and won't throw.
        //
        if ((actualType == MIRType::Value || actualType == MIRType::Object) ||
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
    MOZ_ASSERT(getSingleTarget() &&
               getSingleTarget()->isNative() &&
               getSingleTarget()->hasJitInfo());
    return getSingleTarget()->jitInfo();
}

MDefinition*
MStringLength::foldsTo(TempAllocator& alloc)
{
    if (type() == MIRType::Int32 && string()->isConstant()) {
        JSAtom* atom = &string()->toConstant()->toString()->asAtom();
        return MConstant::New(alloc, Int32Value(atom->length()));
    }

    return this;
}

MDefinition*
MConcat::foldsTo(TempAllocator& alloc)
{
    if (lhs()->isConstant() && lhs()->toConstant()->toString()->empty())
        return rhs();

    if (rhs()->isConstant() && rhs()->toConstant()->toString()->empty())
        return lhs();

    return this;
}

static bool
EnsureFloatInputOrConvert(MUnaryInstruction* owner, TempAllocator& alloc)
{
    MDefinition* input = owner->input();
    if (!input->canProduceFloat32()) {
        if (input->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, input, owner);
        return false;
    }
    return true;
}

void
MFloor::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType::Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        specialization_ = MIRType::Float32;
}

void
MCeil::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType::Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        specialization_ = MIRType::Float32;
}

void
MRound::trySpecializeFloat32(TempAllocator& alloc)
{
    MOZ_ASSERT(type() == MIRType::Int32);
    if (EnsureFloatInputOrConvert(this, alloc))
        specialization_ = MIRType::Float32;
}

void
MNearbyInt::trySpecializeFloat32(TempAllocator& alloc)
{
    if (EnsureFloatInputOrConvert(this, alloc)) {
        specialization_ = MIRType::Float32;
        setResultType(MIRType::Float32);
    }
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

MGoto*
MGoto::New(TempAllocator::Fallible alloc, MBasicBlock* target)
{
    MOZ_ASSERT(target);
    return new(alloc) MGoto(target);
}

MGoto*
MGoto::New(TempAllocator& alloc)
{
    return new(alloc) MGoto(nullptr);
}

void
MUnbox::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf(" ");
    getOperand(0)->printName(out);
    out.printf(" ");

    switch (type()) {
      case MIRType::Int32: out.printf("to Int32"); break;
      case MIRType::Double: out.printf("to Double"); break;
      case MIRType::Boolean: out.printf("to Boolean"); break;
      case MIRType::String: out.printf("to String"); break;
      case MIRType::Symbol: out.printf("to Symbol"); break;
      case MIRType::Object: out.printf("to Object"); break;
      default: break;
    }

    switch (mode()) {
      case Fallible: out.printf(" (fallible)"); break;
      case Infallible: out.printf(" (infallible)"); break;
      case TypeBarrier: out.printf(" (typebarrier)"); break;
      default: break;
    }
}

MDefinition*
MUnbox::foldsTo(TempAllocator &alloc)
{
    if (!input()->isLoadFixedSlot())
        return this;
    MLoadFixedSlot* load = input()->toLoadFixedSlot();
    if (load->type() != MIRType::Value)
        return this;
    if (type() != MIRType::Boolean && !IsNumberType(type()))
        return this;
    // Only optimize if the load comes immediately before the unbox, so it's
    // safe to copy the load's dependency field.
    MInstructionIterator iter(load->block()->begin(load));
    ++iter;
    if (*iter != this)
        return this;

    MLoadFixedSlotAndUnbox* ins = MLoadFixedSlotAndUnbox::New(alloc, load->object(), load->slot(),
                                                              mode(), type(), bailoutKind());
    // As GVN runs after the Alias Analysis, we have to set the dependency by hand
    ins->setDependency(load->dependency());
    return ins;
}

void
MTypeBarrier::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf(" ");
    getOperand(0)->printName(out);
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

MDefinition*
MTypeBarrier::foldsTo(TempAllocator& alloc)
{
    MIRType type = resultTypeSet()->getKnownMIRType();
    if (type == MIRType::Value || type == MIRType::Object)
        return this;

    if (!input()->isConstant())
        return this;

    if (input()->type() != type)
        return this;

    return input();
}

bool
MTypeBarrier::canRedefineInput()
{
    // LTypeBarrier does not need its own def usually, because we can use the
    // input's allocation (LIRGenerator::redefineInput). However, if Spectre
    // mitigations are enabled, guardObjectType may zero the object register on
    // speculatively executed paths, so LTypeBarrier needs to have its own def
    // then to guarantee all uses will see this potentially-zeroed value.

    if (!JitOptions.spectreObjectMitigationsBarriers)
        return true;

    if (barrierKind() == BarrierKind::TypeTagOnly)
        return true;

    TemporaryTypeSet* types = resultTypeSet();
    bool hasSpecificObjects = !types->unknownObject() && types->getObjectCount() > 0;
    if (!hasSpecificObjects)
        return true;

    return false;
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
    for (MUse& p : inputs_)
        p.producer()->removeUse(&p);
    inputs_.clear();
}

MDefinition*
MPhi::foldsTernary(TempAllocator& alloc)
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
    if (testArg->type() == MIRType::Int32 && c->numberToDouble() == 0) {
        testArg->setGuardRangeBailoutsUnchecked();

        // When folding to the constant we need to hoist it.
        if (trueDef == c && !c->block()->dominates(block()))
            c->block()->moveBefore(pred->lastIns(), c);
        return trueDef;
    }

    // If testArg is an double type we can:
    // - fold testArg ? testArg : 0.0 to MNaNToZero(testArg)
    if (testArg->type() == MIRType::Double && mozilla::IsPositiveZero(c->numberToDouble()) &&
        c != trueDef)
    {
        MNaNToZero* replace = MNaNToZero::New(alloc, testArg);
        test->block()->insertBefore(test, replace);
        return replace;
    }

    // If testArg is a string type we can:
    // - fold testArg ? testArg : "" to testArg
    // - fold testArg ? "" : testArg to ""
    if (testArg->type() == MIRType::String &&
        c->toString() == GetJitContext()->runtime->emptyString())
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
MPhi::foldsFilterTypeSet()
{
    // Fold phi with as operands a combination of 'subject' and
    // MFilterTypeSet(subject) to 'subject'.

    if (inputs_.length() == 0)
        return nullptr;

    MDefinition* subject = getOperand(0);
    if (subject->isFilterTypeSet())
        subject = subject->toFilterTypeSet()->input();

    // Not same type, don't fold.
    if (subject->type() != type())
        return nullptr;

    // Phi is better typed (has typeset). Don't fold.
    if (resultTypeSet() && !subject->resultTypeSet())
        return nullptr;

    // Phi is better typed (according to typeset). Don't fold.
    if (subject->resultTypeSet() && resultTypeSet()) {
        if (!subject->resultTypeSet()->isSubset(resultTypeSet()))
            return nullptr;
    }

    for (size_t i = 1, e = numOperands(); i < e; i++) {
        MDefinition* op = getOperand(i);
        if (op == subject)
            continue;
        if (op->isFilterTypeSet() && op->toFilterTypeSet()->input() == subject)
            continue;

        return nullptr;
    }

    return subject;
}

MDefinition*
MPhi::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = operandIfRedundant())
        return def;

    if (MDefinition* def = foldsTernary(alloc))
        return def;

    if (MDefinition* def = foldsFilterTypeSet())
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
MakeMIRTypeSet(TempAllocator& alloc, MIRType type)
{
    MOZ_ASSERT(type != MIRType::Value);
    TypeSet::Type ntype = type == MIRType::Object
                          ? TypeSet::AnyObjectType()
                          : TypeSet::PrimitiveType(ValueTypeFromMIRType(type));
    return alloc.lifoAlloc()->new_<TemporaryTypeSet>(alloc.lifoAlloc(), ntype);
}

bool
jit::MergeTypes(TempAllocator& alloc, MIRType* ptype, TemporaryTypeSet** ptypeSet,
                MIRType newType, TemporaryTypeSet* newTypeSet)
{
    if (newTypeSet && newTypeSet->empty())
        return true;
    LifoAlloc::AutoFallibleScope fallibleAllocator(alloc.lifoAlloc());
    if (newType != *ptype) {
        if (IsTypeRepresentableAsDouble(newType) && IsTypeRepresentableAsDouble(*ptype)) {
            *ptype = MIRType::Double;
        } else if (*ptype != MIRType::Value) {
            if (!*ptypeSet) {
                *ptypeSet = MakeMIRTypeSet(alloc, *ptype);
                if (!*ptypeSet)
                    return false;
            }
            *ptype = MIRType::Value;
        } else if (*ptypeSet && (*ptypeSet)->empty()) {
            *ptype = newType;
        }
    }
    if (*ptypeSet) {
        if (!newTypeSet && newType != MIRType::Value) {
            newTypeSet = MakeMIRTypeSet(alloc, newType);
            if (!newTypeSet)
                return false;
        }
        if (newTypeSet) {
            if (!newTypeSet->isSubset(*ptypeSet)) {
                *ptypeSet = TypeSet::unionSets(*ptypeSet, newTypeSet, alloc.lifoAlloc());
                if (!*ptypeSet)
                    return false;
            }
        } else {
            *ptypeSet = nullptr;
        }
    }
    return true;
}

// Tests whether 'types' includes all possible values represented by
// input/inputTypes.
bool
jit::TypeSetIncludes(TypeSet* types, MIRType input, TypeSet* inputTypes)
{
    if (!types)
        return inputTypes && inputTypes->empty();

    switch (input) {
      case MIRType::Undefined:
      case MIRType::Null:
      case MIRType::Boolean:
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::String:
      case MIRType::Symbol:
      case MIRType::MagicOptimizedArguments:
        return types->hasType(TypeSet::PrimitiveType(ValueTypeFromMIRType(input)));

      case MIRType::Object:
        return types->unknownObject() || (inputTypes && inputTypes->isSubset(types));

      case MIRType::Value:
        return types->unknown() || (inputTypes && inputTypes->isSubset(types));

      default:
        MOZ_CRASH("Bad input type");
    }
}

// Tests if two type combos (type/typeset) are equal.
bool
jit::EqualTypes(MIRType type1, TemporaryTypeSet* typeset1,
                MIRType type2, TemporaryTypeSet* typeset2)
{
    // Types should equal.
    if (type1 != type2)
        return false;

    // Both have equal type and no typeset.
    if (!typeset1 && !typeset2)
        return true;

    // If only one instructions has a typeset.
    // Test if the typset contains the same information as the MIRType.
    if (typeset1 && !typeset2)
        return TypeSetIncludes(typeset1, type2, nullptr);
    if (!typeset1 && typeset2)
        return TypeSetIncludes(typeset2, type1, nullptr);

    // Typesets should equal.
    return typeset1->equals(typeset2);
}

// Tests whether input/inputTypes can always be stored to an unboxed
// object/array property with the given unboxed type.
bool
jit::CanStoreUnboxedType(TempAllocator& alloc,
                         JSValueType unboxedType, MIRType input, TypeSet* inputTypes)
{
    TemporaryTypeSet types;

    switch (unboxedType) {
      case JSVAL_TYPE_BOOLEAN:
      case JSVAL_TYPE_INT32:
      case JSVAL_TYPE_DOUBLE:
      case JSVAL_TYPE_STRING:
        types.addType(TypeSet::PrimitiveType(unboxedType), alloc.lifoAlloc());
        break;

      case JSVAL_TYPE_OBJECT:
        types.addType(TypeSet::AnyObjectType(), alloc.lifoAlloc());
        types.addType(TypeSet::NullType(), alloc.lifoAlloc());
        break;

      default:
        MOZ_CRASH("Bad unboxed type");
    }

    return TypeSetIncludes(&types, input, inputTypes);
}

static bool
CanStoreUnboxedType(TempAllocator& alloc, JSValueType unboxedType, MDefinition* value)
{
    return CanStoreUnboxedType(alloc, unboxedType, value->type(), value->resultTypeSet());
}

bool
MPhi::specializeType(TempAllocator& alloc)
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
        if (!MergeTypes(alloc, &resultType, &resultTypeSet, def->type(), def->resultTypeSet()))
            return false;
    }

    setResultType(resultType);
    setResultTypeSet(resultTypeSet);
    return true;
}

bool
MPhi::addBackedgeType(TempAllocator& alloc, MIRType type, TemporaryTypeSet* typeSet)
{
    MOZ_ASSERT(!specialized_);

    if (hasBackedgeType_) {
        MIRType resultType = this->type();
        TemporaryTypeSet* resultTypeSet = this->resultTypeSet();

        if (!MergeTypes(alloc, &resultType, &resultTypeSet, type, typeSet))
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
    if (def->type() == MIRType::Int32 && this->type() == MIRType::Double)
        return true;

    if (TemporaryTypeSet* types = def->resultTypeSet()) {
        if (this->resultTypeSet())
            return types->isSubset(this->resultTypeSet());
        if (this->type() == MIRType::Value || types->empty())
            return true;
        return this->type() == types->getKnownMIRType();
    }

    if (def->type() == MIRType::Value) {
        // This phi must be able to be any value.
        return this->type() == MIRType::Value
            && (!this->resultTypeSet() || this->resultTypeSet()->unknown());
    }

    return this->mightBeType(def->type());
}

bool
MPhi::checkForTypeChange(TempAllocator& alloc, MDefinition* ins, bool* ptypeChange)
{
    MIRType resultType = this->type();
    TemporaryTypeSet* resultTypeSet = this->resultTypeSet();

    if (!MergeTypes(alloc, &resultType, &resultTypeSet, ins->type(), ins->resultTypeSet()))
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

static inline bool
IsConstant(MDefinition* def, double v)
{
    if (!def->isConstant())
        return false;

    return NumbersAreIdentical(def->toConstant()->numberToDouble(), v);
}

MDefinition*
MBinaryBitwiseInstruction::foldsTo(TempAllocator& alloc)
{
    if (specialization_ != MIRType::Int32)
        return this;

    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    return this;
}

MDefinition*
MBinaryBitwiseInstruction::foldUnnecessaryBitop()
{
    if (specialization_ != MIRType::Int32)
        return this;

    // Fold unsigned shift right operator when the second operand is zero and
    // the only use is an unsigned modulo. Thus, the expression
    // |(x >>> 0) % y| becomes |x % y|.
    if (isUrsh() && hasOneDefUse() && IsUint32Type(this)) {
        MUseDefIterator use(this);
        if (use.def()->isMod() && use.def()->toMod()->isUnsigned())
            return getOperand(0);
        MOZ_ASSERT(!(++use));
    }

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

    if (maskMatchesRightRange) {
        MOZ_ASSERT(lhs->isConstant());
        MOZ_ASSERT(lhs->type() == MIRType::Int32);
        return foldIfAllBitsSet(0);
    }

    if (maskMatchesLeftRange) {
        MOZ_ASSERT(rhs->isConstant());
        MOZ_ASSERT(rhs->type() == MIRType::Int32);
        return foldIfAllBitsSet(1);
    }

    return this;
}

void
MBinaryBitwiseInstruction::infer(BaselineInspector*, jsbytecode*)
{
    if (getOperand(0)->mightBeType(MIRType::Object) || getOperand(0)->mightBeType(MIRType::Symbol) ||
        getOperand(1)->mightBeType(MIRType::Object) || getOperand(1)->mightBeType(MIRType::Symbol))
    {
        specialization_ = MIRType::None;
    } else {
        specializeAs(MIRType::Int32);
    }
}

void
MBinaryBitwiseInstruction::specializeAs(MIRType type)
{
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
    MOZ_ASSERT(this->type() == type);

    specialization_ = type;

    if (isBitOr() || isBitAnd() || isBitXor())
        setCommutative();
}

void
MShiftInstruction::infer(BaselineInspector*, jsbytecode*)
{
    if (getOperand(0)->mightBeType(MIRType::Object) || getOperand(1)->mightBeType(MIRType::Object) ||
        getOperand(0)->mightBeType(MIRType::Symbol) || getOperand(1)->mightBeType(MIRType::Symbol))
        specialization_ = MIRType::None;
    else
        specialization_ = MIRType::Int32;
}

void
MUrsh::infer(BaselineInspector* inspector, jsbytecode* pc)
{
    if (getOperand(0)->mightBeType(MIRType::Object) || getOperand(1)->mightBeType(MIRType::Object) ||
        getOperand(0)->mightBeType(MIRType::Symbol) || getOperand(1)->mightBeType(MIRType::Symbol))
    {
        specialization_ = MIRType::None;
        setResultType(MIRType::Value);
        return;
    }

    if (inspector->hasSeenDoubleResult(pc)) {
        specialization_ = MIRType::Double;
        setResultType(MIRType::Double);
        return;
    }

    specialization_ = MIRType::Int32;
    setResultType(MIRType::Int32);
}

static inline bool
CanProduceNegativeZero(MDefinition* def)
{
    // Test if this instruction can produce negative zero even when bailing out
    // and changing types.
    switch (def->op()) {
        case MDefinition::Opcode::Constant:
            if (def->type() == MIRType::Double && def->toConstant()->toDouble() == -0.0)
                return true;
            MOZ_FALLTHROUGH;
        case MDefinition::Opcode::BitAnd:
        case MDefinition::Opcode::BitOr:
        case MDefinition::Opcode::BitXor:
        case MDefinition::Opcode::BitNot:
        case MDefinition::Opcode::Lsh:
        case MDefinition::Opcode::Rsh:
            return false;
        default:
            return true;
    }
}

static inline bool
NeedNegativeZeroCheck(MDefinition* def)
{
    if (def->isGuardRangeBailouts())
        return true;

    // Test if all uses have the same semantics for -0 and 0
    for (MUseIterator use = def->usesBegin(); use != def->usesEnd(); use++) {
        if (use->consumer()->isResumePoint())
            continue;

        MDefinition* use_def = use->consumer()->toDefinition();
        switch (use_def->op()) {
          case MDefinition::Opcode::Add: {
            // If add is truncating -0 and 0 are observed as the same.
            if (use_def->toAdd()->isTruncated())
                break;

            // x + y gives -0, when both x and y are -0

            // Figure out the order in which the addition's operands will
            // execute. EdgeCaseAnalysis::analyzeLate has renumbered the MIR
            // definitions for us so that this just requires comparing ids.
            MDefinition* first = use_def->toAdd()->lhs();
            MDefinition* second = use_def->toAdd()->rhs();
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
          case MDefinition::Opcode::Sub: {
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

            MOZ_FALLTHROUGH;
          }
          case MDefinition::Opcode::StoreElement:
          case MDefinition::Opcode::StoreElementHole:
          case MDefinition::Opcode::FallibleStoreElement:
          case MDefinition::Opcode::LoadElement:
          case MDefinition::Opcode::LoadElementHole:
          case MDefinition::Opcode::LoadUnboxedScalar:
          case MDefinition::Opcode::LoadTypedArrayElementHole:
          case MDefinition::Opcode::CharCodeAt:
          case MDefinition::Opcode::Mod:
            // Only allowed to remove check when definition is the second operand
            if (use_def->getOperand(0) == def)
                return true;
            for (size_t i = 2, e = use_def->numOperands(); i < e; i++) {
                if (use_def->getOperand(i) == def)
                    return true;
            }
            break;
          case MDefinition::Opcode::BoundsCheck:
            // Only allowed to remove check when definition is the first operand
            if (use_def->toBoundsCheck()->getOperand(1) == def)
                return true;
            break;
          case MDefinition::Opcode::ToString:
          case MDefinition::Opcode::FromCharCode:
          case MDefinition::Opcode::TableSwitch:
          case MDefinition::Opcode::Compare:
          case MDefinition::Opcode::BitAnd:
          case MDefinition::Opcode::BitOr:
          case MDefinition::Opcode::BitXor:
          case MDefinition::Opcode::Abs:
          case MDefinition::Opcode::TruncateToInt32:
            // Always allowed to remove check. No matter which operand.
            break;
          default:
            return true;
        }
    }
    return false;
}

void
MBinaryArithInstruction::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);

    switch (type()) {
      case MIRType::Int32:
        if (isDiv())
            out.printf(" [%s]", toDiv()->isUnsigned() ? "uint32" : "int32");
        else if (isMod())
            out.printf(" [%s]", toMod()->isUnsigned() ? "uint32" : "int32");
        else
            out.printf(" [int32]");
        break;
      case MIRType::Int64:
        if (isDiv())
            out.printf(" [%s]", toDiv()->isUnsigned() ? "uint64" : "int64");
        else if (isMod())
            out.printf(" [%s]", toMod()->isUnsigned() ? "uint64" : "int64");
        else
            out.printf(" [int64]");
        break;
      case MIRType::Float32:
        out.printf(" [float]");
        break;
      case MIRType::Double:
        out.printf(" [double]");
        break;
      default:
        break;
    }
}

MBinaryArithInstruction*
MBinaryArithInstruction::New(TempAllocator& alloc, Opcode op,
                             MDefinition* left, MDefinition* right)
{
    switch (op) {
      case Opcode::Add:
        return MAdd::New(alloc, left, right);
      case Opcode::Sub:
        return MSub::New(alloc, left, right);
      case Opcode::Mul:
        return MMul::New(alloc, left, right);
      case Opcode::Div:
        return MDiv::New(alloc, left, right);
      case Opcode::Mod:
        return MMod::New(alloc, left, right);
      default:
        MOZ_CRASH("unexpected binary opcode");
    }
}

void
MBinaryArithInstruction::setNumberSpecialization(TempAllocator& alloc, BaselineInspector* inspector,
                                                 jsbytecode* pc)
{
    setSpecialization(MIRType::Double);

    // Try to specialize as int32.
    if (getOperand(0)->type() == MIRType::Int32 && getOperand(1)->type() == MIRType::Int32) {
        bool seenDouble = inspector->hasSeenDoubleResult(pc);

        // Use int32 specialization if the operation doesn't overflow on its
        // constant operands and if the operation has never overflowed.
        if (!seenDouble && !constantDoubleResult(alloc))
            setInt32Specialization();
    }
}

bool
MBinaryArithInstruction::constantDoubleResult(TempAllocator& alloc)
{
    bool typeChange = false;
    EvaluateConstantOperands(alloc, this, &typeChange);
    return typeChange;
}

MDefinition*
MRsh::foldsTo(TempAllocator& alloc)
{
    MDefinition* f = MBinaryBitwiseInstruction::foldsTo(alloc);

    if (f != this)
        return f;

    MDefinition* lhs = getOperand(0);
    MDefinition* rhs = getOperand(1);

    if (!lhs->isLsh() || !rhs->isConstant() || rhs->type() != MIRType::Int32)
        return this;

    if (!lhs->getOperand(1)->isConstant() || lhs->getOperand(1)->type() != MIRType::Int32)
        return this;

    uint32_t shift = rhs->toConstant()->toInt32();
    uint32_t shift_lhs = lhs->getOperand(1)->toConstant()->toInt32();
    if (shift != shift_lhs)
        return this;

    switch (shift) {
      case 16:
        return MSignExtendInt32::New(alloc, lhs->getOperand(0), MSignExtendInt32::Half);
      case 24:
        return MSignExtendInt32::New(alloc, lhs->getOperand(0), MSignExtendInt32::Byte);
    }

    return this;
}

MDefinition*
MBinaryArithInstruction::foldsTo(TempAllocator& alloc)
{
    if (specialization_ == MIRType::None)
        return this;

    if (specialization_ == MIRType::Int64)
        return this;

    MDefinition* lhs = getOperand(0);
    MDefinition* rhs = getOperand(1);
    if (MConstant* folded = EvaluateConstantOperands(alloc, this)) {
        if (isTruncated()) {
            if (!folded->block())
                block()->insertBefore(this, folded);
            if (folded->type() != MIRType::Int32)
                return MTruncateToInt32::New(alloc, folded);
        }
        return folded;
    }

    if (mustPreserveNaN_)
        return this;

    // 0 + -0 = 0. So we can't remove addition
    if (isAdd() && specialization_ != MIRType::Int32)
        return this;

    if (IsConstant(rhs, getIdentity())) {
        if (isTruncated())
            return MTruncateToInt32::New(alloc, lhs);
        return lhs;
    }

    // subtraction isn't commutative. So we can't remove subtraction when lhs equals 0
    if (isSub())
        return this;

    if (IsConstant(lhs, getIdentity())) {
        if (isTruncated())
            return MTruncateToInt32::New(alloc, rhs);
        return rhs; // x op id => x
    }

    return this;
}

void
MFilterTypeSet::trySpecializeFloat32(TempAllocator& alloc)
{
    MDefinition* in = input();
    if (in->type() != MIRType::Float32)
        return;

    setResultType(MIRType::Float32);
}

bool
MFilterTypeSet::canProduceFloat32() const
{
    // A FilterTypeSet should be a producer if the input is a producer too.
    // Also, be overly conservative by marking as not float32 producer when the
    // input is a phi, as phis can be cyclic (phiA -> FilterTypeSet -> phiB ->
    // phiA) and FilterTypeSet doesn't belong in the Float32 phi analysis.
    return !input()->isPhi() && input()->canProduceFloat32();
}

bool
MFilterTypeSet::canConsumeFloat32(MUse* operand) const
{
    MOZ_ASSERT(getUseFor(0) == operand);
    // A FilterTypeSet should be a consumer if all uses are consumer. See also
    // comment below MFilterTypeSet::canProduceFloat32.
    bool allConsumerUses = true;
    for (MUseDefIterator use(this); allConsumerUses && use; use++)
        allConsumerUses &= !use.def()->isPhi() && use.def()->canConsumeFloat32(use.use());
    return allConsumerUses;
}

void
MBinaryArithInstruction::trySpecializeFloat32(TempAllocator& alloc)
{
    // Do not use Float32 if we can use int32.
    if (specialization_ == MIRType::Int32)
        return;
    if (specialization_ == MIRType::None)
        return;

    MDefinition* left = lhs();
    MDefinition* right = rhs();

    if (!left->canProduceFloat32() || !right->canProduceFloat32() ||
        !CheckUsesAreFloat32Consumers(this))
    {
        if (left->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, left, this);
        if (right->type() == MIRType::Float32)
            ConvertDefinitionToDouble<1>(alloc, right, this);
        return;
    }

    specialization_ = MIRType::Float32;
    setResultType(MIRType::Float32);
}

void
MMinMax::trySpecializeFloat32(TempAllocator& alloc)
{
    if (specialization_ == MIRType::Int32)
        return;

    MDefinition* left = lhs();
    MDefinition* right = rhs();

    if (!(left->canProduceFloat32() || (left->isMinMax() && left->type() == MIRType::Float32)) ||
        !(right->canProduceFloat32() || (right->isMinMax() && right->type() == MIRType::Float32)))
    {
        if (left->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, left, this);
        if (right->type() == MIRType::Float32)
            ConvertDefinitionToDouble<1>(alloc, right, this);
        return;
    }

    specialization_ = MIRType::Float32;
    setResultType(MIRType::Float32);
}

MDefinition*
MMinMax::foldsTo(TempAllocator& alloc)
{
    if (!lhs()->isConstant() && !rhs()->isConstant())
        return this;

    // Directly apply math utility to compare the rhs() and lhs() when
    // they are both constants.
    if (lhs()->isConstant() && rhs()->isConstant()) {
        if (!lhs()->toConstant()->isTypeRepresentableAsDouble() ||
            !rhs()->toConstant()->isTypeRepresentableAsDouble())
        {
            return this;
        }

        double lnum = lhs()->toConstant()->numberToDouble();
        double rnum = rhs()->toConstant()->numberToDouble();

        double result;
        if (isMax())
            result = js::math_max_impl(lnum, rnum);
        else
            result = js::math_min_impl(lnum, rnum);

        // The folded MConstant should maintain the same MIRType with
        // the original MMinMax.
        if (type() == MIRType::Int32) {
            int32_t cast;
            if (mozilla::NumberEqualsInt32(result, &cast))
                return MConstant::New(alloc, Int32Value(cast));
        } else if (type() == MIRType::Float32) {
            return MConstant::NewFloat32(alloc, result);
        } else {
            MOZ_ASSERT(type() == MIRType::Double);
            return MConstant::New(alloc, DoubleValue(result));
        }
    }

    MDefinition* operand = lhs()->isConstant() ? rhs() : lhs();
    MConstant* constant = lhs()->isConstant() ? lhs()->toConstant() : rhs()->toConstant();

    if (operand->isToDouble() && operand->getOperand(0)->type() == MIRType::Int32) {
        // min(int32, cte >= INT32_MAX) = int32
        if (!isMax() &&
            constant->isTypeRepresentableAsDouble() &&
            constant->numberToDouble() >= INT32_MAX)
        {
            MLimitedTruncate* limit =
                MLimitedTruncate::New(alloc, operand->getOperand(0), MDefinition::NoTruncate);
            block()->insertBefore(this, limit);
            MToDouble* toDouble = MToDouble::New(alloc, limit);
            return toDouble;
        }

        // max(int32, cte <= INT32_MIN) = int32
        if (isMax() &&
            constant->isTypeRepresentableAsDouble() &&
            constant->numberToDouble() <= INT32_MIN)
        {
            MLimitedTruncate* limit =
                MLimitedTruncate::New(alloc, operand->getOperand(0), MDefinition::NoTruncate);
            block()->insertBefore(this, limit);
            MToDouble* toDouble = MToDouble::New(alloc, limit);
            return toDouble;
        }
    }

    if (operand->isArrayLength() && constant->type() == MIRType::Int32) {
        MOZ_ASSERT(operand->type() == MIRType::Int32);

        // max(array.length, 0) = array.length
        // ArrayLength is always >= 0, so just return it.
        if (isMax() && constant->toInt32() <= 0)
            return operand;
    }

    return this;
}

MDefinition*
MPow::foldsConstant(TempAllocator &alloc)
{
    // Both `x` and `p` in `x^p` must be constants in order to precompute.
    if(!(input()->isConstant() && power()->isConstant()))
        return nullptr;
    if(!power()->toConstant()->isTypeRepresentableAsDouble())
        return nullptr;
    if(!input()->toConstant()->isTypeRepresentableAsDouble())
        return nullptr;

    double x = input()->toConstant()->numberToDouble();
    double p = power()->toConstant()->numberToDouble();
    return MConstant::New(alloc, DoubleValue(js::ecmaPow(x, p)));
}

MDefinition*
MPow::foldsConstantPower(TempAllocator &alloc)
{
    // If `p` in `x^p` isn't constant, we can't apply these folds.
    if(!power()->isConstant())
        return nullptr;
    if(!power()->toConstant()->isTypeRepresentableAsDouble())
        return nullptr;

    double pow = power()->toConstant()->numberToDouble();
    MIRType outputType = type();

    // Math.pow(x, 0.5) is a sqrt with edge-case detection.
    if (pow == 0.5)
        return MPowHalf::New(alloc, input());

    // Math.pow(x, -0.5) == 1 / Math.pow(x, 0.5), even for edge cases.
    if (pow == -0.5) {
        MPowHalf* half = MPowHalf::New(alloc, input());
        block()->insertBefore(this, half);
        MConstant* one = MConstant::New(alloc, DoubleValue(1.0));
        block()->insertBefore(this, one);
        return MDiv::New(alloc, one, half, MIRType::Double);
    }

    // Math.pow(x, 1) == x.
    if (pow == 1.0)
        return input();

    // Math.pow(x, 2) == x*x.
    if (pow == 2.0)
        return MMul::New(alloc, input(), input(), outputType);

    // Math.pow(x, 3) == x*x*x.
    if (pow == 3.0) {
        MMul* mul1 = MMul::New(alloc, input(), input(), outputType);
        block()->insertBefore(this, mul1);
        return MMul::New(alloc, input(), mul1, outputType);
    }

    // Math.pow(x, 4) == y*y, where y = x*x.
    if (pow == 4.0) {
        MMul* y = MMul::New(alloc, input(), input(), outputType);
        block()->insertBefore(this, y);
        return MMul::New(alloc, y, y, outputType);
    }

    // No optimization
    return nullptr;
}

MDefinition*
MPow::foldsTo(TempAllocator& alloc)
{
    if(MDefinition *def = foldsConstant(alloc))
        return def;
    if(MDefinition *def = foldsConstantPower(alloc))
        return def;
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
    if (input()->type() == MIRType::Int32)
        return;

    if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
        if (input()->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType::Float32);
    specialization_ = MIRType::Float32;
}

MDefinition*
MDiv::foldsTo(TempAllocator& alloc)
{
    if (specialization_ == MIRType::None)
        return this;

    if (specialization_ == MIRType::Int64)
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
    if (specialization_ != MIRType::Int32)
        return;

    MOZ_ASSERT(lhs()->type() == MIRType::Int32);
    MOZ_ASSERT(rhs()->type() == MIRType::Int32);

    // Try removing divide by zero check
    if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0))
        canBeDivideByZero_ = false;

    // If lhs is a constant int != INT32_MIN, then
    // negative overflow check can be skipped.
    if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(INT32_MIN))
        canBeNegativeOverflow_ = false;

    // If rhs is a constant int != -1, likewise.
    if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(-1))
        canBeNegativeOverflow_ = false;

    // If lhs is != 0, then negative zero check can be skipped.
    if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(0))
        setCanBeNegativeZero(false);

    // If rhs is >= 0, likewise.
    if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
        if (rhs()->toConstant()->toInt32() >= 0)
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
    if (specialization_ == MIRType::None)
        return this;

    if (specialization_ == MIRType::Int64)
        return this;

    if (MDefinition* folded = EvaluateConstantOperands(alloc, this))
        return folded;

    return this;
}

void
MMod::analyzeEdgeCasesForward()
{
    // These optimizations make sense only for integer division
    if (specialization_ != MIRType::Int32)
        return;

    if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0))
        canBeDivideByZero_ = false;

    if (rhs()->isConstant()) {
        int32_t n = rhs()->toConstant()->toInt32();
        if (n > 0 && !IsPowerOfTwo(uint32_t(n)))
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
        if (input()->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType::Float32);
    specialization_ = MIRType::Float32;
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

    if (specialization() != MIRType::Int32)
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
    if (specialization() != MIRType::Int32)
        return;

    // If lhs is > 0, no need for negative zero check.
    if (lhs()->isConstant() && lhs()->type() == MIRType::Int32) {
        if (lhs()->toConstant()->toInt32() > 0)
            setCanBeNegativeZero(false);
    }

    // If rhs is > 0, likewise.
    if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
        if (rhs()->toConstant()->toInt32() > 0)
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
    return !op->mightBeType(MIRType::Object)
        && !op->mightBeType(MIRType::String)
        && !op->mightBeType(MIRType::Symbol)
        && !op->mightBeType(MIRType::MagicOptimizedArguments)
        && !op->mightBeType(MIRType::MagicHole)
        && !op->mightBeType(MIRType::MagicIsConstructing);
}

static bool
SafelyCoercesToDouble(MDefinition* op)
{
    // Strings and symbols are unhandled -- visitToDouble() doesn't support them yet.
    // Null is unhandled -- ToDouble(null) == 0, but (0 == null) is false.
    return SimpleArithOperand(op) && !op->mightBeType(MIRType::Null);
}

MIRType
MCompare::inputType()
{
    switch(compareType_) {
      case Compare_Undefined:
        return MIRType::Undefined;
      case Compare_Null:
        return MIRType::Null;
      case Compare_Boolean:
        return MIRType::Boolean;
      case Compare_UInt32:
      case Compare_Int32:
      case Compare_Int32MaybeCoerceBoth:
      case Compare_Int32MaybeCoerceLHS:
      case Compare_Int32MaybeCoerceRHS:
        return MIRType::Int32;
      case Compare_Double:
      case Compare_DoubleMaybeCoerceLHS:
      case Compare_DoubleMaybeCoerceRHS:
        return MIRType::Double;
      case Compare_Float32:
        return MIRType::Float32;
      case Compare_String:
      case Compare_StrictString:
        return MIRType::String;
      case Compare_Symbol:
        return MIRType::Symbol;
      case Compare_Object:
        return MIRType::Object;
      case Compare_Unknown:
      case Compare_Bitwise:
        return MIRType::Value;
      default:
        MOZ_CRASH("No known conversion");
    }
}

static inline bool
MustBeUInt32(MDefinition* def, MDefinition** pwrapped)
{
    if (def->isUrsh()) {
        *pwrapped = def->toUrsh()->lhs();
        MDefinition* rhs = def->toUrsh()->rhs();
        return def->toUrsh()->bailoutsDisabled() &&
               rhs->maybeConstantValue() &&
               rhs->maybeConstantValue()->isInt32(0);
    }

    if (MConstant* defConst = def->maybeConstantValue()) {
        *pwrapped = defConst;
        return defConst->type() == MIRType::Int32 && defConst->toInt32() >= 0;
    }

    *pwrapped = nullptr;  // silence GCC warning
    return false;
}

/* static */ bool
MBinaryInstruction::unsignedOperands(MDefinition* left, MDefinition* right)
{
    MDefinition* replace;
    if (!MustBeUInt32(left, &replace))
        return false;
    if (replace->type() != MIRType::Int32)
        return false;
    if (!MustBeUInt32(right, &replace))
        return false;
    if (replace->type() != MIRType::Int32)
        return false;
    return true;
}

bool
MBinaryInstruction::unsignedOperands()
{
    return unsignedOperands(getOperand(0), getOperand(1));
}

void
MBinaryInstruction::replaceWithUnsignedOperands()
{
    MOZ_ASSERT(unsignedOperands());

    for (size_t i = 0; i < numOperands(); i++) {
        MDefinition* replace;
        MustBeUInt32(getOperand(i), &replace);
        if (replace == getOperand(i))
            continue;

        getOperand(i)->setImplicitlyUsedUnchecked();
        replaceOperand(i, replace);
    }
}

MCompare::CompareType
MCompare::determineCompareType(JSOp op, MDefinition* left, MDefinition* right)
{
    MIRType lhs = left->type();
    MIRType rhs = right->type();

    bool looseEq = op == JSOP_EQ || op == JSOP_NE;
    bool strictEq = op == JSOP_STRICTEQ || op == JSOP_STRICTNE;
    bool relationalEq = !(looseEq || strictEq);

    // Comparisons on unsigned integers may be treated as UInt32.
    if (unsignedOperands(left, right))
        return Compare_UInt32;

    // Integer to integer or boolean to boolean comparisons may be treated as Int32.
    if ((lhs == MIRType::Int32 && rhs == MIRType::Int32) ||
        (lhs == MIRType::Boolean && rhs == MIRType::Boolean))
    {
        return Compare_Int32MaybeCoerceBoth;
    }

    // Loose/relational cross-integer/boolean comparisons may be treated as Int32.
    if (!strictEq &&
        (lhs == MIRType::Int32 || lhs == MIRType::Boolean) &&
        (rhs == MIRType::Int32 || rhs == MIRType::Boolean))
    {
        return Compare_Int32MaybeCoerceBoth;
    }

    // Numeric comparisons against a double coerce to double.
    if (IsTypeRepresentableAsDouble(lhs) && IsTypeRepresentableAsDouble(rhs))
        return Compare_Double;

    // Any comparison is allowed except strict eq.
    if (!strictEq && IsFloatingPointType(rhs) && SafelyCoercesToDouble(left))
        return Compare_DoubleMaybeCoerceLHS;
    if (!strictEq && IsFloatingPointType(lhs) && SafelyCoercesToDouble(right))
        return Compare_DoubleMaybeCoerceRHS;

    // Handle object comparison.
    if (!relationalEq && lhs == MIRType::Object && rhs == MIRType::Object)
        return Compare_Object;

    // Handle string comparisons. (Relational string compares are still unsupported).
    if (!relationalEq && lhs == MIRType::String && rhs == MIRType::String)
        return Compare_String;

    // Handle symbol comparisons. (Relaational compare will throw)
    if (!relationalEq && lhs == MIRType::Symbol && rhs == MIRType::Symbol)
        return Compare_Symbol;

    // Handle strict string compare.
    if (strictEq && lhs == MIRType::String)
        return Compare_StrictString;
    if (strictEq && rhs == MIRType::String)
        return Compare_StrictString;

    // Handle compare with lhs or rhs being Undefined or Null.
    if (!relationalEq && IsNullOrUndefined(lhs))
        return (lhs == MIRType::Null) ? Compare_Null : Compare_Undefined;
    if (!relationalEq && IsNullOrUndefined(rhs))
        return (rhs == MIRType::Null) ? Compare_Null : Compare_Undefined;

    // Handle strict comparison with lhs/rhs being typed Boolean.
    if (strictEq && (lhs == MIRType::Boolean || rhs == MIRType::Boolean)) {
        // bool/bool case got an int32 specialization earlier.
        MOZ_ASSERT(!(lhs == MIRType::Boolean && rhs == MIRType::Boolean));
        return Compare_Boolean;
    }

    return Compare_Unknown;
}

void
MCompare::cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(operandMightEmulateUndefined());

    if (getOperand(0)->maybeEmulatesUndefined(constraints))
        return;
    if (getOperand(1)->maybeEmulatesUndefined(constraints))
        return;

    markNoOperandEmulatesUndefined();
}

MBitNot*
MBitNot::NewInt32(TempAllocator& alloc, MDefinition* input)
{
    MBitNot* ins = new(alloc) MBitNot(input);
    ins->specialization_ = MIRType::Int32;
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    return ins;
}

MDefinition*
MBitNot::foldsTo(TempAllocator& alloc)
{
    if (specialization_ != MIRType::Int32)
        return this;

    MDefinition* input = getOperand(0);

    if (input->isConstant()) {
        js::Value v = Int32Value(~(input->toConstant()->toInt32()));
        return MConstant::New(alloc, v);
    }

    if (input->isBitNot() && input->toBitNot()->specialization_ == MIRType::Int32) {
        MOZ_ASSERT(input->toBitNot()->getOperand(0)->type() == MIRType::Int32);
        return MTruncateToInt32::New(alloc, input->toBitNot()->input()); // ~~x => x | 0
    }

    return this;
}

MDefinition*
MTypeOf::foldsTo(TempAllocator& alloc)
{
    // Note: we can't use input->type() here, type analysis has
    // boxed the input.
    MOZ_ASSERT(input()->type() == MIRType::Value);

    JSType type;

    switch (inputType()) {
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::Int32:
        type = JSTYPE_NUMBER;
        break;
      case MIRType::String:
        type = JSTYPE_STRING;
        break;
      case MIRType::Symbol:
        type = JSTYPE_SYMBOL;
        break;
      case MIRType::Null:
        type = JSTYPE_OBJECT;
        break;
      case MIRType::Undefined:
        type = JSTYPE_UNDEFINED;
        break;
      case MIRType::Boolean:
        type = JSTYPE_BOOLEAN;
        break;
      case MIRType::Object:
        if (!inputMaybeCallableOrEmulatesUndefined()) {
            // Object is not callable and does not emulate undefined, so it's
            // safe to fold to "object".
            type = JSTYPE_OBJECT;
            break;
        }
        MOZ_FALLTHROUGH;
      default:
        return this;
    }

    return MConstant::New(alloc, StringValue(TypeName(type, GetJitContext()->runtime->names())));
}

void
MTypeOf::cacheInputMaybeCallableOrEmulatesUndefined(CompilerConstraintList* constraints)
{
    MOZ_ASSERT(inputMaybeCallableOrEmulatesUndefined());

    if (!input()->maybeEmulatesUndefined(constraints) && !MaybeCallable(constraints, input()))
        markInputNotCallableOrEmulatesUndefined();
}

MBitAnd*
MBitAnd::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitAnd(left, right, MIRType::Int32);
}

MBitAnd*
MBitAnd::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MBitAnd* ins = new(alloc) MBitAnd(left, right, type);
    ins->specializeAs(type);
    return ins;
}

MBitOr*
MBitOr::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitOr(left, right, MIRType::Int32);
}

MBitOr*
MBitOr::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MBitOr* ins = new(alloc) MBitOr(left, right, type);
    ins->specializeAs(type);
    return ins;
}

MBitXor*
MBitXor::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MBitXor(left, right, MIRType::Int32);
}

MBitXor*
MBitXor::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MBitXor* ins = new(alloc) MBitXor(left, right, type);
    ins->specializeAs(type);
    return ins;
}

MLsh*
MLsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MLsh(left, right, MIRType::Int32);
}

MLsh*
MLsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MLsh* ins = new(alloc) MLsh(left, right, type);
    ins->specializeAs(type);
    return ins;
}

MRsh*
MRsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MRsh(left, right, MIRType::Int32);
}

MRsh*
MRsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MRsh* ins = new(alloc) MRsh(left, right, type);
    ins->specializeAs(type);
    return ins;
}

MUrsh*
MUrsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
{
    return new(alloc) MUrsh(left, right, MIRType::Int32);
}

MUrsh*
MUrsh::New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type)
{
    MUrsh* ins = new(alloc) MUrsh(left, right, type);
    ins->specializeAs(type);

    // Since Ion has no UInt32 type, we use Int32 and we have a special
    // exception to the type rules: we can return values in
    // (INT32_MIN,UINT32_MAX] and still claim that we have an Int32 type
    // without bailing out. This is necessary because Ion has no UInt32
    // type and we can't have bailouts in wasm code.
    ins->bailoutsDisabled_ = true;

    return ins;
}

MResumePoint*
MResumePoint::New(TempAllocator& alloc, MBasicBlock* block, jsbytecode* pc,
                  Mode mode)
{
    MResumePoint* resume = new(alloc) MResumePoint(block, pc, mode);
    if (!resume->init(alloc)) {
        block->discardPreAllocatedResumePoint(resume);
        return nullptr;
    }
    resume->inherit(block);
    return resume;
}

MResumePoint*
MResumePoint::New(TempAllocator& alloc, MBasicBlock* block, MResumePoint* model,
                  const MDefinitionVector& operands)
{
    MResumePoint* resume = new(alloc) MResumePoint(block, model->pc(), model->mode());

    // Allocate the same number of operands as the original resume point, and
    // copy operands from the operands vector and not the not from the current
    // block stack.
    if (!resume->operands_.init(alloc, model->numAllocatedOperands())) {
        block->discardPreAllocatedResumePoint(resume);
        return nullptr;
    }

    // Copy the operands.
    for (size_t i = 0; i < operands.length(); i++)
        resume->initOperand(i, operands[i]);

    return resume;
}

MResumePoint*
MResumePoint::Copy(TempAllocator& alloc, MResumePoint* src)
{
    MResumePoint* resume = new(alloc) MResumePoint(src->block(), src->pc(),
                                                   src->mode());
    // Copy the operands from the original resume point, and not from the
    // current block stack.
    if (!resume->operands_.init(alloc, src->numAllocatedOperands())) {
        src->block()->discardPreAllocatedResumePoint(resume);
        return nullptr;
    }

    // Copy the operands.
    for (size_t i = 0; i < resume->numOperands(); i++)
        resume->initOperand(i, src->getOperand(i));
    return resume;
}

MResumePoint::MResumePoint(MBasicBlock* block, jsbytecode* pc, Mode mode)
  : MNode(block, Kind::ResumePoint),
    pc_(pc),
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

MResumePoint*
MResumePoint::caller() const
{
    return block()->callerResumePoint();
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
MResumePoint::dump(GenericPrinter& out) const
{
    out.printf("resumepoint mode=");

    switch (mode()) {
      case MResumePoint::ResumeAt:
        if (instruction_)
            out.printf("At(%d)", instruction_->id());
        else
            out.printf("At");
        break;
      case MResumePoint::ResumeAfter:
        out.printf("After");
        break;
      case MResumePoint::Outer:
        out.printf("Outer");
        break;
    }

    if (MResumePoint* c = caller())
        out.printf(" (caller in block%u)", c->block()->id());

    for (size_t i = 0; i < numOperands(); i++) {
        out.printf(" ");
        if (operands_[i].hasProducer())
            getOperand(i)->printName(out);
        else
            out.printf("(null)");
    }
    out.printf("\n");
}

void
MResumePoint::dump() const
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
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
MToNumberInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);

    // Fold this operation if the input operand is constant.
    if (input->isConstant()) {
        DebugOnly<IntConversionInputKind> convert = conversion();
        switch (input->type()) {
          case MIRType::Null:
            MOZ_ASSERT(convert.value == IntConversionInputKind::Any);
            return MConstant::New(alloc, Int32Value(0));
          case MIRType::Boolean:
            MOZ_ASSERT(convert.value == IntConversionInputKind::Any ||
                       convert.value == IntConversionInputKind::NumbersOrBoolsOnly);
            return MConstant::New(alloc, Int32Value(input->toConstant()->toBoolean()));
          case MIRType::Int32:
            return MConstant::New(alloc, Int32Value(input->toConstant()->toInt32()));
          case MIRType::Float32:
          case MIRType::Double:
            int32_t ival;
            // Only the value within the range of Int32 can be substituted as constant.
            if (mozilla::NumberIsInt32(input->toConstant()->numberToDouble(), &ival))
                return MConstant::New(alloc, Int32Value(ival));
            break;
          default:
            break;
        }
    }

    // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
    // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
    // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
    // is folded to a MTruncateToInt32 node, which will result in this MIR:
    // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
    // the MUrsh node's type is int32 (since uint32 is not implemented), and
    // that would fold the MTruncateToInt32 node. This will make the modulo
    // unsigned, while is should have been signed.
    if (input->type() == MIRType::Int32 && !IsUint32Type(input))
        return input;

    return this;
}

void
MToNumberInt32::analyzeEdgeCasesBackward()
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

    // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
    // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
    // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
    // is folded to a MTruncateToInt32 node, which will result in this MIR:
    // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
    // the MUrsh node's type is int32 (since uint32 is not implemented), and
    // that would fold the MTruncateToInt32 node. This will make the modulo
    // unsigned, while is should have been signed.
    if (input->type() == MIRType::Int32 && !IsUint32Type(input))
        return input;

    if (input->type() == MIRType::Double && input->isConstant()) {
        int32_t ret = ToInt32(input->toConstant()->toDouble());
        return MConstant::New(alloc, Int32Value(ret));
    }

    return this;
}

MDefinition*
MWasmTruncateToInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->type() == MIRType::Int32)
        return input;

    if (input->type() == MIRType::Double && input->isConstant()) {
        double d = input->toConstant()->toDouble();
        if (IsNaN(d))
            return this;

        if (!isUnsigned() && d <= double(INT32_MAX) && d >= double(INT32_MIN))
            return MConstant::New(alloc, Int32Value(ToInt32(d)));

        if (isUnsigned() && d <= double(UINT32_MAX) && d >= 0)
            return MConstant::New(alloc, Int32Value(ToInt32(d)));
    }

    if (input->type() == MIRType::Float32 && input->isConstant()) {
        double f = double(input->toConstant()->toFloat32());
        if (IsNaN(f))
            return this;

        if (!isUnsigned() && f <= double(INT32_MAX) && f >= double(INT32_MIN))
            return MConstant::New(alloc, Int32Value(ToInt32(f)));

        if (isUnsigned() && f <= double(UINT32_MAX) && f >= 0)
            return MConstant::New(alloc, Int32Value(ToInt32(f)));
    }

    return this;
}

MDefinition*
MWrapInt64ToInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = this->input();
    if (input->isConstant()) {
        uint64_t c = input->toConstant()->toInt64();
        int32_t output = bottomHalf() ? int32_t(c) : int32_t(c >> 32);
        return MConstant::New(alloc, Int32Value(output));
    }

    return this;
}

MDefinition*
MExtendInt32ToInt64::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = this->input();
    if (input->isConstant()) {
        int32_t c = input->toConstant()->toInt32();
        int64_t res = isUnsigned() ? int64_t(uint32_t(c)) : int64_t(c);
        return MConstant::NewInt64(alloc, res);
    }

    return this;
}

MDefinition*
MSignExtendInt32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = this->input();
    if (input->isConstant()) {
        int32_t c = input->toConstant()->toInt32();
        int32_t res;
        switch (mode_) {
          case Byte: res = int32_t(int8_t(c & 0xFF)); break;
          case Half: res = int32_t(int16_t(c & 0xFFFF)); break;
        }
        return MConstant::New(alloc, Int32Value(res));
    }

    return this;
}

MDefinition*
MSignExtendInt64::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = this->input();
    if (input->isConstant()) {
        int64_t c = input->toConstant()->toInt64();
        int64_t res;
        switch (mode_) {
          case Byte: res = int64_t(int8_t(c & 0xFF)); break;
          case Half: res = int64_t(int16_t(c & 0xFFFF)); break;
          case Word: res = int64_t(int32_t(c & 0xFFFFFFFFU)); break;
        }
        return MConstant::NewInt64(alloc, res);
    }

    return this;
}

MDefinition*
MToDouble::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->isBox())
        input = input->getOperand(0);

    if (input->type() == MIRType::Double)
        return input;

    if (input->isConstant() && input->toConstant()->isTypeRepresentableAsDouble())
        return MConstant::New(alloc, DoubleValue(input->toConstant()->numberToDouble()));

    return this;
}

MDefinition*
MToFloat32::foldsTo(TempAllocator& alloc)
{
    MDefinition* input = getOperand(0);
    if (input->isBox())
        input = input->getOperand(0);

    if (input->type() == MIRType::Float32)
        return input;

    // If x is a Float32, Float32(Double(x)) == x
    if (!mustPreserveNaN_ &&
        input->isToDouble() &&
        input->toToDouble()->input()->type() == MIRType::Float32)
    {
        return input->toToDouble()->input();
    }

    if (input->isConstant() && input->toConstant()->isTypeRepresentableAsDouble())
        return MConstant::NewFloat32(alloc, float(input->toConstant()->numberToDouble()));

    return this;
}

MDefinition*
MToString::foldsTo(TempAllocator& alloc)
{
    MDefinition* in = input();
    if (in->isBox())
        in = in->getOperand(0);

    if (in->type() == MIRType::String)
        return in;
    return this;
}

MDefinition*
MClampToUint8::foldsTo(TempAllocator& alloc)
{
    if (MConstant* inputConst = input()->maybeConstantValue()) {
        if (inputConst->isTypeRepresentableAsDouble()) {
            int32_t clamped = ClampDoubleToUint8(inputConst->numberToDouble());
            return MConstant::New(alloc, Int32Value(clamped));
        }
    }
    return this;
}

bool
MCompare::tryFoldEqualOperands(bool* result)
{
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
               compareType_ == Compare_Object || compareType_ == Compare_Bitwise ||
               compareType_ == Compare_Symbol);

    if (isDoubleComparison() || isFloat32Comparison()) {
        if (!operandsAreNeverNaN())
            return false;
    }

    lhs()->setGuardRangeBailoutsUnchecked();

    *result = (jsop() == JSOP_STRICTEQ);
    return true;
}

bool
MCompare::tryFoldTypeOf(bool* result)
{
    if (!lhs()->isTypeOf() && !rhs()->isTypeOf())
        return false;
    if (!lhs()->isConstant() && !rhs()->isConstant())
        return false;

    MTypeOf* typeOf = lhs()->isTypeOf() ? lhs()->toTypeOf() : rhs()->toTypeOf();
    MConstant* constant = lhs()->isConstant() ? lhs()->toConstant() : rhs()->toConstant();

    if (constant->type() != MIRType::String)
        return false;

    if (jsop() != JSOP_STRICTEQ && jsop() != JSOP_STRICTNE &&
        jsop() != JSOP_EQ && jsop() != JSOP_NE)
    {
        return false;
    }

    const JSAtomState& names = GetJitContext()->runtime->names();
    if (constant->toString() == TypeName(JSTYPE_UNDEFINED, names)) {
        if (!typeOf->input()->mightBeType(MIRType::Undefined) &&
            !typeOf->inputMaybeCallableOrEmulatesUndefined())
        {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_BOOLEAN, names)) {
        if (!typeOf->input()->mightBeType(MIRType::Boolean)) {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_NUMBER, names)) {
        if (!typeOf->input()->mightBeType(MIRType::Int32) &&
            !typeOf->input()->mightBeType(MIRType::Float32) &&
            !typeOf->input()->mightBeType(MIRType::Double))
        {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_STRING, names)) {
        if (!typeOf->input()->mightBeType(MIRType::String)) {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_SYMBOL, names)) {
        if (!typeOf->input()->mightBeType(MIRType::Symbol)) {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_OBJECT, names)) {
        if (!typeOf->input()->mightBeType(MIRType::Object) &&
            !typeOf->input()->mightBeType(MIRType::Null))
        {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    } else if (constant->toString() == TypeName(JSTYPE_FUNCTION, names)) {
        if (!typeOf->inputMaybeCallableOrEmulatesUndefined()) {
            *result = (jsop() == JSOP_STRICTNE || jsop() == JSOP_NE);
            return true;
        }
    }

    return false;
}

bool
MCompare::tryFold(bool* result)
{
    JSOp op = jsop();

    if (tryFoldEqualOperands(result))
        return true;

    if (tryFoldTypeOf(result))
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
            if (!lhs()->mightBeType(MIRType::Null) &&
                !lhs()->mightBeType(MIRType::Undefined) &&
                !(lhs()->mightBeType(MIRType::Object) && operandMightEmulateUndefined()))
            {
                *result = (op == JSOP_NE);
                return true;
            }
        }
        return false;
    }

    if (compareType_ == Compare_Boolean) {
        MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);
        MOZ_ASSERT(rhs()->type() == MIRType::Boolean);
        MOZ_ASSERT(lhs()->type() != MIRType::Boolean, "Should use Int32 comparison");

        if (!lhs()->mightBeType(MIRType::Boolean)) {
            *result = (op == JSOP_STRICTNE);
            return true;
        }
        return false;
    }

    if (compareType_ == Compare_StrictString) {
        MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);
        MOZ_ASSERT(rhs()->type() == MIRType::String);
        MOZ_ASSERT(lhs()->type() != MIRType::String, "Should use String comparison");

        if (!lhs()->mightBeType(MIRType::String)) {
            *result = (op == JSOP_STRICTNE);
            return true;
        }
        return false;
    }

    return false;
}

template <typename T>
static bool
FoldComparison(JSOp op, T left, T right)
{
    switch (op) {
      case JSOP_LT: return left < right;
      case JSOP_LE: return left <= right;
      case JSOP_GT: return left > right;
      case JSOP_GE: return left >= right;
      case JSOP_STRICTEQ: case JSOP_EQ: return left == right;
      case JSOP_STRICTNE: case JSOP_NE: return left != right;
      default: MOZ_CRASH("Unexpected op.");
    }
}

bool
MCompare::evaluateConstantOperands(TempAllocator& alloc, bool* result)
{
    if (type() != MIRType::Boolean && type() != MIRType::Int32)
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
        MOZ_ASSERT(constant->type() == MIRType::Double);
        double cte = constant->toDouble();

        if (operand->isToDouble() && operand->getOperand(0)->type() == MIRType::Int32) {
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

    MConstant* lhs = left->toConstant();
    MConstant* rhs = right->toConstant();

    // Fold away some String equality comparisons.
    if (lhs->type() == MIRType::String && rhs->type() == MIRType::String) {
        int32_t comp = 0; // Default to equal.
        if (left != right)
            comp = CompareAtoms(&lhs->toString()->asAtom(), &rhs->toString()->asAtom());
        *result = FoldComparison(jsop_, comp, 0);
        return true;
    }

    if (compareType_ == Compare_UInt32) {
        *result = FoldComparison(jsop_, uint32_t(lhs->toInt32()), uint32_t(rhs->toInt32()));
        return true;
    }

    if (compareType_ == Compare_Int64) {
        *result = FoldComparison(jsop_, lhs->toInt64(), rhs->toInt64());
        return true;
    }

    if (compareType_ == Compare_UInt64) {
        *result = FoldComparison(jsop_, uint64_t(lhs->toInt64()), uint64_t(rhs->toInt64()));
        return true;
    }

    if (lhs->isTypeRepresentableAsDouble() && rhs->isTypeRepresentableAsDouble()) {
        *result = FoldComparison(jsop_, lhs->numberToDouble(), rhs->numberToDouble());
        return true;
    }

    return false;
}

MDefinition*
MCompare::foldsTo(TempAllocator& alloc)
{
    bool result;

    if (tryFold(&result) || evaluateConstantOperands(alloc, &result)) {
        if (type() == MIRType::Int32)
            return MConstant::New(alloc, Int32Value(result));

        MOZ_ASSERT(type() == MIRType::Boolean);
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
        if (lhs->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, lhs, this);
        if (rhs->type() == MIRType::Float32)
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

    if (!getOperand(0)->maybeEmulatesUndefined(constraints))
        markNoOperandEmulatesUndefined();
}

MDefinition*
MNot::foldsTo(TempAllocator& alloc)
{
    // Fold if the input is constant
    if (MConstant* inputConst = input()->maybeConstantValue()) {
        bool b;
        if (inputConst->valueToBoolean(&b)) {
            if (type() == MIRType::Int32 || type() == MIRType::Int64)
                return MConstant::New(alloc, Int32Value(!b));
            return MConstant::New(alloc, BooleanValue(!b));
        }
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
    if (input()->type() == MIRType::Undefined || input()->type() == MIRType::Null)
        return MConstant::New(alloc, BooleanValue(true));

    // NOT of a symbol is always false.
    if (input()->type() == MIRType::Symbol)
        return MConstant::New(alloc, BooleanValue(false));

    // NOT of an object that can't emulate undefined is always false.
    if (input()->type() == MIRType::Object && !operandMightEmulateUndefined())
        return MConstant::New(alloc, BooleanValue(false));

    return this;
}

void
MNot::trySpecializeFloat32(TempAllocator& alloc)
{
    MDefinition* in = input();
    if (!in->canProduceFloat32() && in->type() == MIRType::Float32)
        ConvertDefinitionToDouble<0>(alloc, in, this);
}

void
MBeta::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);

    out.printf(" ");
    comparison_->dump(out);
}

bool
MCreateThisWithTemplate::canRecoverOnBailout() const
{
    MOZ_ASSERT(templateObject()->is<PlainObject>() || templateObject()->is<UnboxedPlainObject>());
    MOZ_ASSERT_IF(templateObject()->is<PlainObject>(),
                  !templateObject()->as<PlainObject>().denseElementsAreCopyOnWrite());
    return true;
}

bool
OperandIndexMap::init(TempAllocator& alloc, JSObject* templateObject)
{
    const UnboxedLayout& layout =
        templateObject->as<UnboxedPlainObject>().layoutDontCheckGeneration();

    const UnboxedLayout::PropertyVector& properties = layout.properties();
    MOZ_ASSERT(properties.length() < 255);

    // Allocate an array of indexes, where the top of each field correspond to
    // the index of the operand in the MObjectState instance.
    if (!map.init(alloc, layout.size()))
        return false;

    // Reset all indexes to 0, which is an error code.
    for (size_t i = 0; i < map.length(); i++)
        map[i] = 0;

    // Map the property offsets to the indexes of MObjectState operands.
    uint8_t index = 1;
    for (size_t i = 0; i < properties.length(); i++, index++)
        map[properties[i].offset] = index;

    return true;
}

MObjectState::MObjectState(MObjectState* state)
  : MVariadicInstruction(classOpcode),
    numSlots_(state->numSlots_),
    numFixedSlots_(state->numFixedSlots_),
    operandIndex_(state->operandIndex_)
{
    // This instruction is only used as a summary for bailout paths.
    setResultType(MIRType::Object);
    setRecoveredOnBailout();
}

MObjectState::MObjectState(JSObject *templateObject, OperandIndexMap* operandIndex)
  : MVariadicInstruction(classOpcode)
{
    // This instruction is only used as a summary for bailout paths.
    setResultType(MIRType::Object);
    setRecoveredOnBailout();

    if (templateObject->is<NativeObject>()) {
        NativeObject* nativeObject = &templateObject->as<NativeObject>();
        numSlots_ = nativeObject->slotSpan();
        numFixedSlots_ = nativeObject->numFixedSlots();
    } else {
        const UnboxedLayout& layout =
            templateObject->as<UnboxedPlainObject>().layoutDontCheckGeneration();
        // Same as UnboxedLayout::makeNativeGroup
        numSlots_ = layout.properties().length();
        numFixedSlots_ = gc::GetGCKindSlots(layout.getAllocKind());
    }

    operandIndex_ = operandIndex;
}

JSObject*
MObjectState::templateObjectOf(MDefinition* obj)
{
    if (obj->isNewObject())
        return obj->toNewObject()->templateObject();
    else if (obj->isCreateThisWithTemplate())
        return obj->toCreateThisWithTemplate()->templateObject();
    else if (obj->isNewCallObject())
        return obj->toNewCallObject()->templateObject();
    else if (obj->isNewIterator())
        return obj->toNewIterator()->templateObject();

    MOZ_CRASH("unreachable");
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

bool
MObjectState::initFromTemplateObject(TempAllocator& alloc, MDefinition* undefinedVal)
{
    JSObject* templateObject = templateObjectOf(object());

    // Initialize all the slots of the object state with the value contained in
    // the template object. This is needed to account values which are baked in
    // the template objects and not visible in IonMonkey, such as the
    // uninitialized-lexical magic value of call objects.
    if (templateObject->is<UnboxedPlainObject>()) {
        UnboxedPlainObject& unboxedObject = templateObject->as<UnboxedPlainObject>();
        const UnboxedLayout& layout = unboxedObject.layoutDontCheckGeneration();
        const UnboxedLayout::PropertyVector& properties = layout.properties();

        for (size_t i = 0; i < properties.length(); i++) {
            Value val = unboxedObject.getValue(properties[i], /* maybeUninitialized = */ true);
            MDefinition* def = undefinedVal;
            if (!val.isUndefined()) {
                MConstant* ins = val.isObject() ?
                    MConstant::NewConstraintlessObject(alloc, &val.toObject()) :
                    MConstant::New(alloc, val);
                block()->insertBefore(this, ins);
                def = ins;
            }
            initSlot(i, def);
        }
    } else {
        NativeObject& nativeObject = templateObject->as<NativeObject>();
        MOZ_ASSERT(nativeObject.slotSpan() == numSlots());

        for (size_t i = 0; i < numSlots(); i++) {
            Value val = nativeObject.getSlot(i);
            MDefinition *def = undefinedVal;
            if (!val.isUndefined()) {
                MConstant* ins = val.isObject() ?
                    MConstant::NewConstraintlessObject(alloc, &val.toObject()) :
                    MConstant::New(alloc, val);
                block()->insertBefore(this, ins);
                def = ins;
            }
            initSlot(i, def);
        }
    }
    return true;
}

MObjectState*
MObjectState::New(TempAllocator& alloc, MDefinition* obj)
{
    JSObject* templateObject = templateObjectOf(obj);
    MOZ_ASSERT(templateObject, "Unexpected object creation.");

    OperandIndexMap* operandIndex = nullptr;
    if (templateObject->is<UnboxedPlainObject>()) {
        operandIndex = new(alloc) OperandIndexMap;
        if (!operandIndex || !operandIndex->init(alloc, templateObject))
            return nullptr;
    }

    MObjectState* res = new(alloc) MObjectState(templateObject, operandIndex);
    if (!res || !res->init(alloc, obj))
        return nullptr;
    return res;
}

MObjectState*
MObjectState::Copy(TempAllocator& alloc, MObjectState* state)
{
    MObjectState* res = new(alloc) MObjectState(state);
    if (!res || !res->init(alloc, state->object()))
        return nullptr;
    for (size_t i = 0; i < res->numSlots(); i++)
        res->initSlot(i, state->getSlot(i));
    return res;
}

MArrayState::MArrayState(MDefinition* arr)
  : MVariadicInstruction(classOpcode)
{
    // This instruction is only used as a summary for bailout paths.
    setResultType(MIRType::Object);
    setRecoveredOnBailout();
    numElements_ = arr->isNewArray() ? arr->toNewArray()->length()
                   : arr->toNewArrayCopyOnWrite()->length();
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

bool
MArrayState::initFromTemplateObject(TempAllocator& alloc, MDefinition* undefinedVal)
{
    if (!array()->isNewArrayCopyOnWrite()) {
        for (size_t i = 0; i < numElements(); i++)
            initElement(i, undefinedVal);

        return true;
    }

    ArrayObject* obj = array()->toNewArrayCopyOnWrite()->templateObject();
    MOZ_ASSERT(obj->length() == numElements());

    // Initialize all the elements of the object state with the value contained in
    // the template object.
    for (size_t i = 0; i < numElements(); i++) {
        Value val = obj->getDenseElement(i);
        MDefinition* def = undefinedVal;
        if (!val.isUndefined()) {
            MConstant* ins = val.isObject() ?
                MConstant::NewConstraintlessObject(alloc, &val.toObject()) :
                MConstant::New(alloc, val);
            block()->insertBefore(this, ins);
            def = ins;
        }
        initElement(i, def);
    }

    return true;
}

MArrayState*
MArrayState::New(TempAllocator& alloc, MDefinition* arr, MDefinition* initLength)
{
    MArrayState* res = new(alloc) MArrayState(arr);
    if (!res || !res->init(alloc, arr, initLength))
        return nullptr;
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

MArgumentState*
MArgumentState::New(TempAllocator::Fallible view, const MDefinitionVector& args)
{
    MArgumentState* res = new(view.alloc) MArgumentState();
    if (!res || !res->init(view.alloc, args.length()))
        return nullptr;
    for (size_t i = 0, e = args.length(); i < e; i++)
        res->initOperand(i, args[i]);
    return res;
}

MArgumentState*
MArgumentState::Copy(TempAllocator& alloc, MArgumentState* state)
{
    MArgumentState* res = new(alloc) MArgumentState();
    if (!res || !res->init(alloc, state->numElements()))
        return nullptr;
    for (size_t i = 0, e = res->numOperands(); i < e; i++)
        res->initOperand(i, state->getOperand(i));
    return res;
}

MNewArray::MNewArray(TempAllocator& alloc, CompilerConstraintList* constraints, uint32_t length,
                     MConstant* templateConst, gc::InitialHeap initialHeap, jsbytecode* pc,
                     bool vmCall)
  : MUnaryInstruction(classOpcode, templateConst),
    length_(length),
    initialHeap_(initialHeap),
    convertDoubleElements_(false),
    pc_(pc),
    vmCall_(vmCall)
{
    setResultType(MIRType::Object);
    if (templateObject()) {
        if (TemporaryTypeSet* types = MakeSingletonTypeSet(alloc, constraints, templateObject())) {
            setResultTypeSet(types);
            if (types->convertDoubleElements(constraints) == TemporaryTypeSet::AlwaysConvertToDoubles)
                convertDoubleElements_ = true;
        }
    }
}

MDefinition::AliasType
MLoadFixedSlot::mightAlias(const MDefinition* def) const
{
    if (def->isStoreFixedSlot()) {
        const MStoreFixedSlot* store = def->toStoreFixedSlot();
        if (store->slot() != slot())
            return AliasType::NoAlias;
        if (store->object() != object())
            return AliasType::MayAlias;
        return AliasType::MustAlias;
    }
    return AliasType::MayAlias;
}

MDefinition*
MLoadFixedSlot::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = foldsToStore(alloc))
        return def;

    return this;
}

MDefinition::AliasType
MLoadFixedSlotAndUnbox::mightAlias(const MDefinition* def) const
{
    if (def->isStoreFixedSlot()) {
        const MStoreFixedSlot* store = def->toStoreFixedSlot();
        if (store->slot() != slot())
            return AliasType::NoAlias;
        if (store->object() != object())
            return AliasType::MayAlias;
        return AliasType::MustAlias;
    }
    return AliasType::MayAlias;
}

MDefinition*
MLoadFixedSlotAndUnbox::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = foldsToStore(alloc))
        return def;

    return this;
}

MDefinition*
MWasmAddOffset::foldsTo(TempAllocator& alloc)
{
    MDefinition* baseArg = base();
    if (!baseArg->isConstant())
        return this;

    MOZ_ASSERT(baseArg->type() == MIRType::Int32);
    CheckedInt<uint32_t> ptr = baseArg->toConstant()->toInt32();

    ptr += offset();

    if (!ptr.isValid())
        return this;

    return MConstant::New(alloc, Int32Value(ptr.value()));
}

bool
MWasmAlignmentCheck::congruentTo(const MDefinition* ins) const
{
    if (!ins->isWasmAlignmentCheck())
        return false;
    const MWasmAlignmentCheck* check = ins->toWasmAlignmentCheck();
    return byteSize_ == check->byteSize() && congruentIfOperandsEqual(check);
}

MDefinition::AliasType
MAsmJSLoadHeap::mightAlias(const MDefinition* def) const
{
    if (def->isAsmJSStoreHeap()) {
        const MAsmJSStoreHeap* store = def->toAsmJSStoreHeap();
        if (store->accessType() != accessType())
            return AliasType::MayAlias;
        if (!base()->isConstant() || !store->base()->isConstant())
            return AliasType::MayAlias;
        const MConstant* otherBase = store->base()->toConstant();
        if (base()->toConstant()->equals(otherBase) && offset() == store->offset())
            return AliasType::MayAlias;
        return AliasType::NoAlias;
    }
    return AliasType::MayAlias;
}

bool
MAsmJSLoadHeap::congruentTo(const MDefinition* ins) const
{
    if (!ins->isAsmJSLoadHeap())
        return false;
    const MAsmJSLoadHeap* load = ins->toAsmJSLoadHeap();
    return load->accessType() == accessType() &&
           load->offset() == offset() &&
           congruentIfOperandsEqual(load);
}

MDefinition::AliasType
MWasmLoadGlobalVar::mightAlias(const MDefinition* def) const
{
    if (def->isWasmStoreGlobalVar()) {
        const MWasmStoreGlobalVar* store = def->toWasmStoreGlobalVar();
        // Global variables can't alias each other or be type-reinterpreted.
        return (store->globalDataOffset() == globalDataOffset_) ? AliasType::MayAlias :
                                                                  AliasType::NoAlias;
    }
    return AliasType::MayAlias;
}

HashNumber
MWasmLoadGlobalVar::valueHash() const
{
    HashNumber hash = MDefinition::valueHash();
    hash = addU32ToHash(hash, globalDataOffset_);
    return hash;
}

bool
MWasmLoadGlobalVar::congruentTo(const MDefinition* ins) const
{
    if (ins->isWasmLoadGlobalVar())
        return globalDataOffset_ == ins->toWasmLoadGlobalVar()->globalDataOffset_;
    return false;
}

MDefinition*
MWasmLoadGlobalVar::foldsTo(TempAllocator& alloc)
{
    if (!dependency() || !dependency()->isWasmStoreGlobalVar())
        return this;

    MWasmStoreGlobalVar* store = dependency()->toWasmStoreGlobalVar();
    if (!store->block()->dominates(block()))
        return this;

    if (store->globalDataOffset() != globalDataOffset())
        return this;

    if (store->value()->type() != type())
        return this;

    return store->value();
}

MDefinition::AliasType
MLoadSlot::mightAlias(const MDefinition* def) const
{
    if (def->isStoreSlot()) {
        const MStoreSlot* store = def->toStoreSlot();
        if (store->slot() != slot())
            return AliasType::NoAlias;

        if (store->slots() != slots())
            return AliasType::MayAlias;

        return AliasType::MustAlias;
    }
    return AliasType::MayAlias;
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
    if (MDefinition* def = foldsToStore(alloc))
        return def;

    return this;
}

void
MLoadSlot::printOpcode(GenericPrinter& out) const
{
    MDefinition::printOpcode(out);
    out.printf(" %d", slot());
}

void
MStoreSlot::printOpcode(GenericPrinter& out) const
{
    PrintOpcodeName(out, op());
    out.printf(" ");
    getOperand(0)->printName(out);
    out.printf(" %d ", slot());
    getOperand(1)->printName(out);
}

MDefinition*
MFunctionEnvironment::foldsTo(TempAllocator& alloc)
{
    if (!input()->isLambda())
        return this;

    return input()->toLambda()->environmentChain();
}

static bool
AddIsANonZeroAdditionOf(MAdd* add, MDefinition* ins)
{
    if (add->lhs() != ins && add->rhs() != ins)
        return false;
    MDefinition* other = (add->lhs() == ins) ? add->rhs() : add->lhs();
    if (!IsNumberType(other->type()))
        return false;
    if (!other->isConstant())
        return false;
    if (other->toConstant()->numberToDouble() == 0)
        return false;
    return true;
}

static bool
DefinitelyDifferentValue(MDefinition* ins1, MDefinition* ins2)
{
    if (ins1 == ins2)
        return false;

    // Drop the MToNumberInt32 added by the TypePolicy for double and float values.
    if (ins1->isToNumberInt32())
        return DefinitelyDifferentValue(ins1->toToNumberInt32()->input(), ins2);
    if (ins2->isToNumberInt32())
        return DefinitelyDifferentValue(ins2->toToNumberInt32()->input(), ins1);

    // Ignore the bounds check, which in most cases will contain the same info.
    if (ins1->isBoundsCheck())
        return DefinitelyDifferentValue(ins1->toBoundsCheck()->index(), ins2);
    if (ins2->isBoundsCheck())
        return DefinitelyDifferentValue(ins2->toBoundsCheck()->index(), ins1);

    // For constants check they are not equal.
    if (ins1->isConstant() && ins2->isConstant())
        return !ins1->toConstant()->equals(ins2->toConstant());

    // Check if "ins1 = ins2 + cte", which would make both instructions
    // have different values.
    if (ins1->isAdd()) {
        if (AddIsANonZeroAdditionOf(ins1->toAdd(), ins2))
            return true;
    }
    if (ins2->isAdd()) {
        if (AddIsANonZeroAdditionOf(ins2->toAdd(), ins1))
            return true;
    }

    return false;
}

MDefinition::AliasType
MLoadElement::mightAlias(const MDefinition* def) const
{
    if (def->isStoreElement()) {
        const MStoreElement* store = def->toStoreElement();
        if (store->index() != index()) {
            if (DefinitelyDifferentValue(store->index(), index()))
                return AliasType::NoAlias;
            return AliasType::MayAlias;
        }

        if (store->elements() != elements())
            return AliasType::MayAlias;

        return AliasType::MustAlias;
    }
    return AliasType::MayAlias;
}

MDefinition*
MLoadElement::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = foldsToStore(alloc))
        return def;

    return this;
}

MDefinition::AliasType
MLoadUnboxedObjectOrNull::mightAlias(const MDefinition* def) const
{
    if (def->isStoreUnboxedObjectOrNull()) {
        const MStoreUnboxedObjectOrNull* store = def->toStoreUnboxedObjectOrNull();
        if (store->index() != index()) {
            if (DefinitelyDifferentValue(store->index(), index()))
                return AliasType::NoAlias;
            return AliasType::MayAlias;
        }

        if (store->elements() != elements())
            return AliasType::MayAlias;

        if (store->offsetAdjustment() != offsetAdjustment())
            return AliasType::MayAlias;

        return AliasType::MustAlias;
    }
    return AliasType::MayAlias;
}

MDefinition*
MLoadUnboxedObjectOrNull::foldsTo(TempAllocator& alloc)
{
    if (MDefinition* def = foldsToStore(alloc))
        return def;

    return this;
}

bool
MGuardReceiverPolymorphic::congruentTo(const MDefinition* ins) const
{
    if (!ins->isGuardReceiverPolymorphic())
        return false;

    const MGuardReceiverPolymorphic* other = ins->toGuardReceiverPolymorphic();

    if (numReceivers() != other->numReceivers())
        return false;
    for (size_t i = 0; i < numReceivers(); i++) {
        if (receiver(i) != other->receiver(i))
            return false;
    }

    return congruentIfOperandsEqual(ins);
}

void
InlinePropertyTable::trimTo(const InliningTargets& targets, const BoolVector& choiceSet)
{
    for (size_t i = 0; i < targets.length(); i++) {
        // If the target was inlined, don't erase the entry.
        if (choiceSet[i])
            continue;

        // If the target wasn't a function we would have veto'ed it
        // and it will not be in the entries list.
        if (!targets[i].target->is<JSFunction>())
            continue;

        JSFunction* target = &targets[i].target->as<JSFunction>();

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
InlinePropertyTable::trimToTargets(const InliningTargets& targets)
{
    JitSpew(JitSpew_Inlining, "Got inlineable property cache with %d cases",
            (int)numEntries());

    size_t i = 0;
    while (i < numEntries()) {
        bool foundFunc = false;
        for (size_t j = 0; j < targets.length(); j++) {
            if (entries_[i]->func == targets[j].target) {
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

bool
InlinePropertyTable::hasObjectGroup(ObjectGroup* group) const
{
    for (size_t i = 0; i < numEntries(); i++) {
        if (entries_[i]->group == group)
            return true;
    }
    return false;
}

TemporaryTypeSet*
InlinePropertyTable::buildTypeSetForFunction(TempAllocator& tempAlloc, JSFunction* func) const
{
    LifoAlloc* alloc = tempAlloc.lifoAlloc();
    TemporaryTypeSet* types = alloc->new_<TemporaryTypeSet>();
    if (!types)
        return nullptr;
    for (size_t i = 0; i < numEntries(); i++) {
        if (entries_[i]->func == func)
            types->addType(TypeSet::ObjectType(entries_[i]->group), alloc);
    }
    return types;
}

bool
InlinePropertyTable::appendRoots(MRootList& roots) const
{
    for (const Entry* entry : entries_) {
        if (!entry->appendRoots(roots))
            return false;
    }
    return true;
}

bool
MGetPropertyCache::allowDoubleResult() const
{
    if (!resultTypeSet())
        return true;

    return resultTypeSet()->hasType(TypeSet::DoubleType());
}

MDefinition::AliasType
MGetPropertyPolymorphic::mightAlias(const MDefinition* store) const
{
    // Allow hoisting this instruction if the store does not write to a
    // slot read by this instruction.

    if (!store->isStoreFixedSlot() && !store->isStoreSlot())
        return AliasType::MayAlias;

    for (size_t i = 0; i < numReceivers(); i++) {
        const Shape* shape = this->shape(i);
        if (!shape)
            continue;
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

        return AliasType::MayAlias;
    }

    return AliasType::NoAlias;
}

bool
MGetPropertyPolymorphic::appendRoots(MRootList& roots) const
{
    if (!roots.append(name_))
        return false;

    for (const PolymorphicEntry& entry : receivers_) {
        if (!entry.appendRoots(roots))
            return false;
    }

    return true;
}

bool
MSetPropertyPolymorphic::appendRoots(MRootList& roots) const
{
    if (!roots.append(name_))
        return false;

    for (const PolymorphicEntry& entry : receivers_) {
        if (!entry.appendRoots(roots))
            return false;
    }

    return true;
}

bool
MGuardReceiverPolymorphic::appendRoots(MRootList& roots) const
{
    for (const ReceiverGuard& guard : receivers_) {
        if (!roots.append(guard))
            return false;
    }
    return true;
}

bool
MDispatchInstruction::appendRoots(MRootList& roots) const
{
    for (const Entry& entry : map_) {
        if (!entry.appendRoots(roots))
            return false;
    }
    return true;
}

bool
MObjectGroupDispatch::appendRoots(MRootList& roots) const
{
    if (inlinePropertyTable_ && !inlinePropertyTable_->appendRoots(roots))
        return false;
    return MDispatchInstruction::appendRoots(roots);
}

bool
MFunctionDispatch::appendRoots(MRootList& roots) const
{
    return MDispatchInstruction::appendRoots(roots);
}

bool
MConstant::appendRoots(MRootList& roots) const
{
    switch (type()) {
      case MIRType::String:
        return roots.append(toString());
      case MIRType::Symbol:
        return roots.append(toSymbol());
      case MIRType::Object:
        return roots.append(&toObject());
      case MIRType::Undefined:
      case MIRType::Null:
      case MIRType::Boolean:
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::MagicOptimizedArguments:
      case MIRType::MagicOptimizedOut:
      case MIRType::MagicHole:
      case MIRType::MagicIsConstructing:
      case MIRType::MagicUninitializedLexical:
        return true;
      default:
        MOZ_CRASH("Unexpected type");
    }
}

MDefinition*
MWasmUnsignedToDouble::foldsTo(TempAllocator& alloc)
{
    if (input()->isConstant() && input()->type() == MIRType::Int32)
        return MConstant::New(alloc, DoubleValue(uint32_t(input()->toConstant()->toInt32())));

    return this;
}

MDefinition*
MWasmUnsignedToFloat32::foldsTo(TempAllocator& alloc)
{
    if (input()->isConstant() && input()->type() == MIRType::Int32) {
        double dval = double(uint32_t(input()->toConstant()->toInt32()));
        if (IsFloat32Representable(dval))
            return MConstant::NewFloat32(alloc, float(dval));
    }

    return this;
}

MWasmCall*
MWasmCall::New(TempAllocator& alloc, const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
               const Args& args, MIRType resultType, uint32_t spIncrement, MDefinition* tableIndex)
{
    MWasmCall* call = new(alloc) MWasmCall(desc, callee, spIncrement);
    call->setResultType(resultType);

    if (!call->argRegs_.init(alloc, args.length()))
        return nullptr;
    for (size_t i = 0; i < call->argRegs_.length(); i++)
        call->argRegs_[i] = args[i].reg;

    if (!call->init(alloc, call->argRegs_.length() + (callee.isTable() ? 1 : 0)))
        return nullptr;
    // FixedList doesn't initialize its elements, so do an unchecked init.
    for (size_t i = 0; i < call->argRegs_.length(); i++)
        call->initOperand(i, args[i].def);
    if (callee.isTable())
        call->initOperand(call->argRegs_.length(), tableIndex);

    return call;
}

MWasmCall*
MWasmCall::NewBuiltinInstanceMethodCall(TempAllocator& alloc,
                                        const wasm::CallSiteDesc& desc,
                                        const wasm::SymbolicAddress builtin,
                                        const ABIArg& instanceArg,
                                        const Args& args,
                                        MIRType resultType,
                                        uint32_t spIncrement)
{
    auto callee = wasm::CalleeDesc::builtinInstanceMethod(builtin);
    MWasmCall* call = MWasmCall::New(alloc, desc, callee, args, resultType, spIncrement, nullptr);
    if (!call)
        return nullptr;

    MOZ_ASSERT(instanceArg != ABIArg());
    call->instanceArg_ = instanceArg;
    return call;
}

void
MSqrt::trySpecializeFloat32(TempAllocator& alloc) {
    if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
        if (input()->type() == MIRType::Float32)
            ConvertDefinitionToDouble<0>(alloc, input(), this);
        return;
    }

    setResultType(MIRType::Float32);
    specialization_ = MIRType::Float32;
}

MDefinition*
MClz::foldsTo(TempAllocator& alloc)
{
    if (num()->isConstant()) {
        MConstant* c = num()->toConstant();
        if (type() == MIRType::Int32) {
            int32_t n = c->toInt32();
            if (n == 0)
                return MConstant::New(alloc, Int32Value(32));
            return MConstant::New(alloc, Int32Value(mozilla::CountLeadingZeroes32(n)));
        }
        int64_t n = c->toInt64();
        if (n == 0)
            return MConstant::NewInt64(alloc, int64_t(64));
        return MConstant::NewInt64(alloc, int64_t(mozilla::CountLeadingZeroes64(n)));
    }

    return this;
}

MDefinition*
MCtz::foldsTo(TempAllocator& alloc)
{
    if (num()->isConstant()) {
        MConstant* c = num()->toConstant();
        if (type() == MIRType::Int32) {
            int32_t n = num()->toConstant()->toInt32();
            if (n == 0)
                return MConstant::New(alloc, Int32Value(32));
            return MConstant::New(alloc, Int32Value(mozilla::CountTrailingZeroes32(n)));
        }
        int64_t n = c->toInt64();
        if (n == 0)
            return MConstant::NewInt64(alloc, int64_t(64));
        return MConstant::NewInt64(alloc, int64_t(mozilla::CountTrailingZeroes64(n)));
    }

    return this;
}

MDefinition*
MPopcnt::foldsTo(TempAllocator& alloc)
{
    if (num()->isConstant()) {
        MConstant* c = num()->toConstant();
        if (type() == MIRType::Int32) {
            int32_t n = num()->toConstant()->toInt32();
            return MConstant::New(alloc, Int32Value(mozilla::CountPopulation32(n)));
        }
        int64_t n = c->toInt64();
        return MConstant::NewInt64(alloc, int64_t(mozilla::CountPopulation64(n)));
    }

    return this;
}

MDefinition*
MBoundsCheck::foldsTo(TempAllocator& alloc)
{
    if (index()->isConstant() && length()->isConstant()) {
        uint32_t len = length()->toConstant()->toInt32();
        uint32_t idx = index()->toConstant()->toInt32();
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
    if (numSuccessors() == 1 || (op->type() != MIRType::Value && !IsNumberType(op->type())))
        return MGoto::New(alloc, getDefault());

    if (MConstant* opConst = op->maybeConstantValue()) {
        if (op->type() == MIRType::Int32) {
            int32_t i = opConst->toInt32() - low_;
            MBasicBlock* target;
            if (size_t(i) < numCases())
                target = getCase(size_t(i));
            else
                target = getDefault();
            MOZ_ASSERT(target);
            return MGoto::New(alloc, target);
        }
    }

    return this;
}

MDefinition*
MArrayJoin::foldsTo(TempAllocator& alloc)
{
    MDefinition* arr = array();

    if (!arr->isStringSplit())
        return this;

    setRecoveredOnBailout();
    if (arr->hasLiveDefUses()) {
        setNotRecoveredOnBailout();
        return this;
    }

    // The MStringSplit won't generate any code.
    arr->setRecoveredOnBailout();

    // We're replacing foo.split(bar).join(baz) by
    // foo.replace(bar, baz).  MStringSplit could be recovered by
    // a bailout.  As we are removing its last use, and its result
    // could be captured by a resume point, this MStringSplit will
    // be executed on the bailout path.
    MDefinition* string = arr->toStringSplit()->string();
    MDefinition* pattern = arr->toStringSplit()->separator();
    MDefinition* replacement = sep();

    MStringReplace *substr = MStringReplace::New(alloc, string, pattern, replacement);
    substr->setFlatReplacement();
    return substr;
}

MDefinition*
MGetFirstDollarIndex::foldsTo(TempAllocator& alloc)
{
    MDefinition* strArg = str();
    if (!strArg->isConstant())
        return this;

    JSAtom* atom = &strArg->toConstant()->toString()->asAtom();
    int32_t index = GetFirstDollarIndexRawFlat(atom);
    return MConstant::New(alloc, Int32Value(index));
}

MConvertUnboxedObjectToNative*
MConvertUnboxedObjectToNative::New(TempAllocator& alloc, MDefinition* obj, ObjectGroup* group)
{
    MConvertUnboxedObjectToNative* res = new(alloc) MConvertUnboxedObjectToNative(obj, group);

    ObjectGroup* nativeGroup = group->unboxedLayout().nativeGroup();

    // Make a new type set for the result of this instruction which replaces
    // the input group with the native group we will convert it to.
    TemporaryTypeSet* types = obj->resultTypeSet();
    if (types && !types->unknownObject()) {
        TemporaryTypeSet* newTypes = types->cloneWithoutObjects(alloc.lifoAlloc());
        if (newTypes) {
            for (size_t i = 0; i < types->getObjectCount(); i++) {
                TypeSet::ObjectKey* key = types->getObject(i);
                if (!key)
                    continue;
                if (key->unknownProperties() || !key->isGroup() || key->group() != group)
                    newTypes->addType(TypeSet::ObjectType(key), alloc.lifoAlloc());
                else
                    newTypes->addType(TypeSet::ObjectType(nativeGroup), alloc.lifoAlloc());
            }
            res->setResultTypeSet(newTypes);
        }
    }

    return res;
}

bool
jit::ElementAccessIsDenseNative(CompilerConstraintList* constraints,
                                MDefinition* obj, MDefinition* id)
{
    if (obj->mightBeType(MIRType::String))
        return false;

    if (id->type() != MIRType::Int32 && id->type() != MIRType::Double)
        return false;

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types)
        return false;

    // Typed arrays are native classes but do not have dense elements.
    const Class* clasp = types->getKnownClass(constraints);
    return clasp && clasp->isNative() && !IsTypedArrayClass(clasp);
}

bool
jit::ElementAccessIsTypedArray(CompilerConstraintList* constraints,
                               MDefinition* obj, MDefinition* id,
                               Scalar::Type* arrayType)
{
    if (obj->mightBeType(MIRType::String))
        return false;

    if (id->type() != MIRType::Int32 && id->type() != MIRType::Double)
        return false;

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types)
        return false;

    *arrayType = types->getTypedArrayType(constraints);
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
jit::ElementAccessMightBeFrozen(CompilerConstraintList* constraints, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();
    return !types || types->hasObjectFlags(constraints, OBJECT_FLAG_FROZEN_ELEMENTS);
}

AbortReasonOr<bool>
jit::ElementAccessHasExtraIndexedProperty(IonBuilder* builder, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();

    if (!types || types->hasObjectFlags(builder->constraints(), OBJECT_FLAG_LENGTH_OVERFLOW))
        return true;

    return TypeCanHaveExtraIndexedProperties(builder, types);
}

MIRType
jit::DenseNativeElementType(CompilerConstraintList* constraints, MDefinition* obj)
{
    TemporaryTypeSet* types = obj->resultTypeSet();
    MIRType elementType = MIRType::None;
    unsigned count = types->getObjectCount();

    for (unsigned i = 0; i < count; i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return MIRType::None;

        HeapTypeSetKey elementTypes = key->property(JSID_VOID);

        MIRType type = elementTypes.knownMIRType(constraints);
        if (type == MIRType::None)
            return MIRType::None;

        if (elementType == MIRType::None)
            elementType = type;
        else if (elementType != type)
            return MIRType::None;
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

    if (!name && IsTypedArrayClass(key->clasp())) {
        Scalar::Type arrayType = Scalar::Type(key->clasp() - &TypedArrayObject::classes[0]);
        MIRType type = MIRTypeForTypedArrayRead(arrayType, true);
        if (observed->mightBeMIRType(type))
            return BarrierKind::NoBarrier;
        return BarrierKind::TypeSet;
    }

    jsid id = name ? NameToId(name) : JSID_VOID;
    HeapTypeSetKey property = key->property(id);
    if (property.maybeTypes()) {
        if (!TypeSetIncludes(observed, MIRType::Value, property.maybeTypes())) {
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
                                  TempAllocator& alloc,
                                  CompilerConstraintList* constraints,
                                  TypeSet::ObjectKey* key, PropertyName* name,
                                  TemporaryTypeSet* observed, bool updateObserved)
{
    if (!updateObserved)
        return PropertyReadNeedsTypeBarrier(constraints, key, name, observed);

    // If this access has never executed, try to add types to the observed set
    // according to any property which exists on the object or its prototype.
    if (observed->empty() && name) {
        TypeSet::ObjectKey* obj = key;
        do {
            if (!obj->clasp()->isNative())
                break;

            if (propertycx)
                obj->ensureTrackedProperty(propertycx, NameToId(name));

            if (obj->unknownProperties())
                break;

            HeapTypeSetKey property = obj->property(NameToId(name));
            if (property.maybeTypes()) {
                TypeSet::TypeList types;
                if (!property.maybeTypes()->enumerateTypes(&types))
                    break;
                if (types.length() == 1) {
                    // Note: the return value here is ignored.
                    observed->addType(types[0], alloc.lifoAlloc());
                }
                break;
            }

            if (!obj->proto().isObject())
                break;
            obj = TypeSet::ObjectKey::get(obj->proto().toObject());
        } while (obj);
    }

    return PropertyReadNeedsTypeBarrier(constraints, key, name, observed);
}

BarrierKind
jit::PropertyReadNeedsTypeBarrier(JSContext* propertycx,
                                  TempAllocator& alloc,
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
            BarrierKind kind = PropertyReadNeedsTypeBarrier(propertycx, alloc, constraints, key,
                                                            name, observed, updateObserved);
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

AbortReasonOr<BarrierKind>
jit::PropertyReadOnPrototypeNeedsTypeBarrier(IonBuilder* builder,
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
            if (!builder->alloc().ensureBallast())
                return builder->abort(AbortReason::Alloc);
            if (!key->hasStableClassAndProto(builder->constraints()))
                return BarrierKind::TypeSet;
            if (!key->proto().isObject())
                break;
            JSObject* proto = builder->checkNurseryObject(key->proto().toObject());
            key = TypeSet::ObjectKey::get(proto);
            BarrierKind kind = PropertyReadNeedsTypeBarrier(builder->constraints(),
                                                            key, name, observed);
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

AbortReasonOr<bool>
PrototypeHasIndexedProperty(IonBuilder* builder, JSObject* obj)
{
    do {
        TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(builder->checkNurseryObject(obj));
        if (ClassCanHaveExtraProperties(key->clasp()))
            return true;
        if (key->unknownProperties())
            return true;
        HeapTypeSetKey index = key->property(JSID_VOID);
        if (index.nonData(builder->constraints()) || index.isOwnProperty(builder->constraints()))
            return true;
        obj = obj->staticPrototype();
        if (!builder->alloc().ensureBallast())
            return builder->abort(AbortReason::Alloc);
    } while (obj);

    return false;
}

// Whether Array.prototype, or an object on its proto chain, has an indexed property.
AbortReasonOr<bool>
jit::ArrayPrototypeHasIndexedProperty(IonBuilder* builder, JSScript* script)
{
    if (JSObject* proto = script->global().maybeGetArrayPrototype())
        return PrototypeHasIndexedProperty(builder, proto);
    return true;
}

// Whether obj or any of its prototypes have an indexed property.
AbortReasonOr<bool>
jit::TypeCanHaveExtraIndexedProperties(IonBuilder* builder, TemporaryTypeSet* types)
{
    const Class* clasp = types->getKnownClass(builder->constraints());

    // Note: typed arrays have indexed properties not accounted for by type
    // information, though these are all in bounds and will be accounted for
    // by JIT paths.
    if (!clasp || (ClassCanHaveExtraProperties(clasp) && !IsTypedArrayClass(clasp)))
        return true;

    if (types->hasObjectFlags(builder->constraints(), OBJECT_FLAG_SPARSE_INDEXES))
        return true;

    JSObject* proto;
    if (!types->getCommonPrototype(builder->constraints(), &proto))
        return true;

    if (!proto)
        return false;

    return PrototypeHasIndexedProperty(builder, proto);
}

static bool
PropertyTypeIncludes(TempAllocator& alloc, HeapTypeSetKey property,
                     MDefinition* value, MIRType implicitType)
{
    // If implicitType is not MIRType::None, it is an additional type which the
    // property implicitly includes. In this case, make a new type set which
    // explicitly contains the type.
    TypeSet* types = property.maybeTypes();
    if (implicitType != MIRType::None) {
        TypeSet::Type newType = TypeSet::PrimitiveType(ValueTypeFromMIRType(implicitType));
        if (types)
            types = types->clone(alloc.lifoAlloc());
        else
            types = alloc.lifoAlloc()->new_<TemporaryTypeSet>();
        if (!types) {
            return false;
        }
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
      case MIRType::Boolean:
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::String:
      case MIRType::Symbol: {
        // The property is a particular primitive type, guard by unboxing the
        // value before the write.
        if (!(*pvalue)->mightBeType(propertyType)) {
            // The value's type does not match the property type. Just do a VM
            // call as it will always trigger invalidation of the compiled code.
            MOZ_ASSERT_IF((*pvalue)->type() != MIRType::Value, (*pvalue)->type() != propertyType);
            return false;
        }
        MInstruction* ins = MUnbox::New(alloc, *pvalue, propertyType, MUnbox::Fallible);
        current->add(ins);
        *pvalue = ins;
        return true;
      }
      default:;
    }

    if ((*pvalue)->type() != MIRType::Value)
        return false;

    TemporaryTypeSet* types = aggregateProperty->maybeTypes()->clone(alloc.lifoAlloc());
    if (!types)
        return false;

    // If all possible objects can be stored without a barrier, we don't have to
    // guard on the specific object types.
    BarrierKind kind = BarrierKind::TypeSet;
    if ((*pvalue)->resultTypeSet() && (*pvalue)->resultTypeSet()->objectsAreSubset(types))
        kind = BarrierKind::TypeTagOnly;

    MInstruction* ins = MTypeBarrier::New(alloc, *pvalue, types, kind);
    current->add(ins);
    ins->setNotMovable();
    if (ins->type() == MIRType::Undefined) {
        ins = MConstant::New(alloc, UndefinedValue());
        current->add(ins);
    } else if (ins->type() == MIRType::Null) {
        ins = MConstant::New(alloc, NullValue());
        current->add(ins);
    }
    *pvalue = ins;
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
                      MIRType implicitType /* = MIRType::None */)
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
        if (!name && IsTypedArrayClass(key->clasp()))
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

    // Perform additional filtering to make sure that any unboxed property
    // being written can accommodate the value.
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (key && key->isGroup() && key->group()->maybeUnboxedLayout()) {
            const UnboxedLayout& layout = key->group()->unboxedLayout();
            if (name) {
                const UnboxedLayout::Property* property = layout.lookup(name);
                if (property && !CanStoreUnboxedType(alloc, property->type, *pvalue))
                    return true;
            }
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
        if (!name && IsTypedArrayClass(key->clasp()))
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
