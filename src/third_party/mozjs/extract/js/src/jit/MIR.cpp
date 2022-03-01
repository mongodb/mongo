/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIR.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/ScopeExit.h"

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/RegExp.h"
#include "jit/AtomicOperations.h"
#include "jit/CompileInfo.h"
#include "jit/JitSpewer.h"
#include "jit/KnownClass.h"
#include "jit/MIRGraph.h"
#include "jit/RangeAnalysis.h"
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo, JSTypedMethodJitInfo
#include "js/ScalarType.h"            // js::Scalar::Type
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Uint8Clamped.h"
#include "wasm/WasmCode.h"

#include "builtin/Boolean-inl.h"

#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::ToInt32;

using mozilla::CheckedInt;
using mozilla::DebugOnly;
using mozilla::IsFloat32Representable;
using mozilla::IsNaN;
using mozilla::IsPowerOfTwo;
using mozilla::Maybe;
using mozilla::NumbersAreIdentical;

NON_GC_POINTER_TYPE_ASSERTIONS_GENERATED

#ifdef DEBUG
size_t MUse::index() const { return consumer()->indexOf(this); }
#endif

template <size_t Op>
static void ConvertDefinitionToDouble(TempAllocator& alloc, MDefinition* def,
                                      MInstruction* consumer) {
  MInstruction* replace = MToDouble::New(alloc, def);
  consumer->replaceOperand(Op, replace);
  consumer->block()->insertBefore(consumer, replace);
}

static bool CheckUsesAreFloat32Consumers(const MInstruction* ins) {
  if (ins->isImplicitlyUsed()) {
    return false;
  }
  bool allConsumerUses = true;
  for (MUseDefIterator use(ins); allConsumerUses && use; use++) {
    allConsumerUses &= use.def()->canConsumeFloat32(use.use());
  }
  return allConsumerUses;
}

#ifdef JS_JITSPEW
static const char* OpcodeName(MDefinition::Opcode op) {
  static const char* const names[] = {
#  define NAME(x) #  x,
      MIR_OPCODE_LIST(NAME)
#  undef NAME
  };
  return names[unsigned(op)];
}

void MDefinition::PrintOpcodeName(GenericPrinter& out, Opcode op) {
  const char* name = OpcodeName(op);
  size_t len = strlen(name);
  for (size_t i = 0; i < len; i++) {
    out.printf("%c", unicode::ToLowerCase(name[i]));
  }
}
#endif

static MConstant* EvaluateInt64ConstantOperands(TempAllocator& alloc,
                                                MBinaryInstruction* ins) {
  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  if (!left->isConstant() || !right->isConstant()) {
    return nullptr;
  }

  MOZ_ASSERT(left->type() == MIRType::Int64);
  MOZ_ASSERT(right->type() == MIRType::Int64);

  int64_t lhs = left->toConstant()->toInt64();
  int64_t rhs = right->toConstant()->toInt64();
  int64_t ret;

  switch (ins->op()) {
    case MDefinition::Opcode::BitAnd:
      ret = lhs & rhs;
      break;
    case MDefinition::Opcode::BitOr:
      ret = lhs | rhs;
      break;
    case MDefinition::Opcode::BitXor:
      ret = lhs ^ rhs;
      break;
    case MDefinition::Opcode::Lsh:
      ret = lhs << (rhs & 0x3F);
      break;
    case MDefinition::Opcode::Rsh:
      ret = lhs >> (rhs & 0x3F);
      break;
    case MDefinition::Opcode::Ursh:
      ret = uint64_t(lhs) >> (uint64_t(rhs) & 0x3F);
      break;
    case MDefinition::Opcode::Add:
      ret = lhs + rhs;
      break;
    case MDefinition::Opcode::Sub:
      ret = lhs - rhs;
      break;
    case MDefinition::Opcode::Mul:
      ret = lhs * rhs;
      break;
    case MDefinition::Opcode::Div:
      if (rhs == 0) {
        // Division by zero will trap at runtime.
        return nullptr;
      }
      if (ins->toDiv()->isUnsigned()) {
        ret = int64_t(uint64_t(lhs) / uint64_t(rhs));
      } else if (lhs == INT64_MIN || rhs == -1) {
        // Overflow will trap at runtime.
        return nullptr;
      } else {
        ret = lhs / rhs;
      }
      break;
    case MDefinition::Opcode::Mod:
      if (rhs == 0) {
        // Division by zero will trap at runtime.
        return nullptr;
      }
      if (!ins->toMod()->isUnsigned() && (lhs < 0 || rhs < 0)) {
        // Handle all negative values at runtime, for simplicity.
        return nullptr;
      }
      ret = int64_t(uint64_t(lhs) % uint64_t(rhs));
      break;
    default:
      MOZ_CRASH("NYI");
  }

  return MConstant::NewInt64(alloc, ret);
}

static MConstant* EvaluateConstantOperands(TempAllocator& alloc,
                                           MBinaryInstruction* ins,
                                           bool* ptypeChange = nullptr) {
  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  MOZ_ASSERT(IsTypeRepresentableAsDouble(left->type()));
  MOZ_ASSERT(IsTypeRepresentableAsDouble(right->type()));

  if (!left->isConstant() || !right->isConstant()) {
    return nullptr;
  }

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
          if (ins->toDiv()->trapOnError()) {
            return nullptr;
          }
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
          if (ins->toMod()->trapOnError()) {
            return nullptr;
          }
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

  if (ins->type() == MIRType::Float32) {
    return MConstant::NewFloat32(alloc, float(ret));
  }
  if (ins->type() == MIRType::Double) {
    return MConstant::New(alloc, DoubleValue(ret));
  }

  Value retVal;
  retVal.setNumber(JS::CanonicalizeNaN(ret));

  // If this was an int32 operation but the result isn't an int32 (for
  // example, a division where the numerator isn't evenly divisible by the
  // denominator), decline folding.
  MOZ_ASSERT(ins->type() == MIRType::Int32);
  if (!retVal.isInt32()) {
    if (ptypeChange) {
      *ptypeChange = true;
    }
    return nullptr;
  }

  return MConstant::New(alloc, retVal);
}

static MMul* EvaluateExactReciprocal(TempAllocator& alloc, MDiv* ins) {
  // we should fold only when it is a floating point operation
  if (!IsFloatingPointType(ins->type())) {
    return nullptr;
  }

  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  if (!right->isConstant()) {
    return nullptr;
  }

  int32_t num;
  if (!mozilla::NumberIsInt32(right->toConstant()->numberToDouble(), &num)) {
    return nullptr;
  }

  // check if rhs is a power of two
  if (mozilla::Abs(num) & (mozilla::Abs(num) - 1)) {
    return nullptr;
  }

  Value ret;
  ret.setDouble(1.0 / double(num));

  MConstant* foldedRhs;
  if (ins->type() == MIRType::Float32) {
    foldedRhs = MConstant::NewFloat32(alloc, ret.toDouble());
  } else {
    foldedRhs = MConstant::New(alloc, ret);
  }

  MOZ_ASSERT(foldedRhs->type() == ins->type());
  ins->block()->insertBefore(ins, foldedRhs);

  MMul* mul = MMul::New(alloc, left, foldedRhs, ins->type());
  mul->setMustPreserveNaN(ins->mustPreserveNaN());
  return mul;
}

#ifdef JS_JITSPEW
const char* MDefinition::opName() const { return OpcodeName(op()); }

void MDefinition::printName(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf("%u", id());
}
#endif

HashNumber MDefinition::valueHash() const {
  HashNumber out = HashNumber(op());
  for (size_t i = 0, e = numOperands(); i < e; i++) {
    out = addU32ToHash(out, getOperand(i)->id());
  }
  if (MDefinition* dep = dependency()) {
    out = addU32ToHash(out, dep->id());
  }
  return out;
}

HashNumber MNullaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MUnaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MBinaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MTernaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  hash = addU32ToHash(hash, getOperand(2)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MQuaternaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  hash = addU32ToHash(hash, getOperand(2)->id());
  hash = addU32ToHash(hash, getOperand(3)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

bool MDefinition::congruentIfOperandsEqual(const MDefinition* ins) const {
  if (op() != ins->op()) {
    return false;
  }

  if (type() != ins->type()) {
    return false;
  }

  if (isEffectful() || ins->isEffectful()) {
    return false;
  }

  if (numOperands() != ins->numOperands()) {
    return false;
  }

  for (size_t i = 0, e = numOperands(); i < e; i++) {
    if (getOperand(i) != ins->getOperand(i)) {
      return false;
    }
  }

  return true;
}

MDefinition* MDefinition::foldsTo(TempAllocator& alloc) {
  // In the default case, there are no constants to fold.
  return this;
}

bool MDefinition::mightBeMagicType() const {
  if (IsMagicType(type())) {
    return true;
  }

  if (MIRType::Value != type()) {
    return false;
  }

  return true;
}

bool MDefinition::definitelyType(std::initializer_list<MIRType> types) const {
#ifdef DEBUG
  // Only support specialized, non-magic types.
  auto isSpecializedNonMagic = [](MIRType type) {
    return type <= MIRType::Object;
  };
#endif

  MOZ_ASSERT(types.size() > 0);
  MOZ_ASSERT(std::all_of(types.begin(), types.end(), isSpecializedNonMagic));

  if (type() == MIRType::Value) {
    return false;
  }

  return std::find(types.begin(), types.end(), type()) != types.end();
}

MDefinition* MInstruction::foldsToStore(TempAllocator& alloc) {
  if (!dependency()) {
    return nullptr;
  }

  MDefinition* store = dependency();
  if (mightAlias(store) != AliasType::MustAlias) {
    return nullptr;
  }

  if (!store->block()->dominates(block())) {
    return nullptr;
  }

  MDefinition* value;
  switch (store->op()) {
    case Opcode::StoreFixedSlot:
      value = store->toStoreFixedSlot()->value();
      break;
    case Opcode::StoreDynamicSlot:
      value = store->toStoreDynamicSlot()->value();
      break;
    case Opcode::StoreElement:
      value = store->toStoreElement()->value();
      break;
    default:
      MOZ_CRASH("unknown store");
  }

  // If the type are matching then we return the value which is used as
  // argument of the store.
  if (value->type() != type()) {
    // If we expect to read a type which is more generic than the type seen
    // by the store, then we box the value used by the store.
    if (type() != MIRType::Value) {
      return nullptr;
    }

    MOZ_ASSERT(value->type() < MIRType::Value);
    MBox* box = MBox::New(alloc, value);
    value = box;
  }

  return value;
}

void MDefinition::analyzeEdgeCasesForward() {}

void MDefinition::analyzeEdgeCasesBackward() {}

void MInstruction::setResumePoint(MResumePoint* resumePoint) {
  MOZ_ASSERT(!resumePoint_);
  resumePoint_ = resumePoint;
  resumePoint_->setInstruction(this);
}

void MInstruction::stealResumePoint(MInstruction* other) {
  MResumePoint* resumePoint = other->resumePoint_;
  other->resumePoint_ = nullptr;

  resumePoint->resetInstruction();
  setResumePoint(resumePoint);
}

void MInstruction::moveResumePointAsEntry() {
  MOZ_ASSERT(isNop());
  block()->clearEntryResumePoint();
  block()->setEntryResumePoint(resumePoint_);
  resumePoint_->resetInstruction();
  resumePoint_ = nullptr;
}

void MInstruction::clearResumePoint() {
  resumePoint_->resetInstruction();
  block()->discardPreAllocatedResumePoint(resumePoint_);
  resumePoint_ = nullptr;
}

MDefinition* MTest::foldsDoubleNegation(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  if (op->isNot()) {
    // If the operand of the Not is itself a Not, they cancel out.
    MDefinition* opop = op->getOperand(0);
    if (opop->isNot()) {
      return MTest::New(alloc, opop->toNot()->input(), ifTrue(), ifFalse());
    }
    return MTest::New(alloc, op->toNot()->input(), ifFalse(), ifTrue());
  }
  return nullptr;
}

MDefinition* MTest::foldsConstant(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);
  if (MConstant* opConst = op->maybeConstantValue()) {
    bool b;
    if (opConst->valueToBoolean(&b)) {
      return MGoto::New(alloc, b ? ifTrue() : ifFalse());
    }
  }
  return nullptr;
}

MDefinition* MTest::foldsTypes(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  switch (op->type()) {
    case MIRType::Undefined:
    case MIRType::Null:
      return MGoto::New(alloc, ifFalse());
    case MIRType::Symbol:
      return MGoto::New(alloc, ifTrue());
    default:
      break;
  }
  return nullptr;
}

MDefinition* MTest::foldsNeedlessControlFlow(TempAllocator& alloc) {
  for (MInstructionIterator iter(ifTrue()->begin()), end(ifTrue()->end());
       iter != end;) {
    MInstruction* ins = *iter++;
    if (ins->isNop() || ins->isGoto()) {
      continue;
    }
    if (ins->hasUses()) {
      return nullptr;
    }
    if (!DeadIfUnused(ins)) {
      return nullptr;
    }
  }

  for (MInstructionIterator iter(ifFalse()->begin()), end(ifFalse()->end());
       iter != end;) {
    MInstruction* ins = *iter++;
    if (ins->isNop() || ins->isGoto()) {
      continue;
    }
    if (ins->hasUses()) {
      return nullptr;
    }
    if (!DeadIfUnused(ins)) {
      return nullptr;
    }
  }

  if (ifTrue()->numSuccessors() != 1 || ifFalse()->numSuccessors() != 1) {
    return nullptr;
  }
  if (ifTrue()->getSuccessor(0) != ifFalse()->getSuccessor(0)) {
    return nullptr;
  }

  if (ifTrue()->successorWithPhis()) {
    return nullptr;
  }

  return MGoto::New(alloc, ifTrue());
}

MDefinition* MTest::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsDoubleNegation(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsConstant(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsTypes(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsNeedlessControlFlow(alloc)) {
    return def;
  }

  return this;
}

AliasSet MThrow::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MNewArrayDynamicLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MNewTypedArrayDynamicLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

#ifdef JS_JITSPEW
void MDefinition::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  for (size_t j = 0, e = numOperands(); j < e; j++) {
    out.printf(" ");
    if (getUseFor(j)->hasProducer()) {
      getOperand(j)->printName(out);
      out.printf(":%s", StringFromMIRType(getOperand(j)->type()));
    } else {
      out.printf("(null)");
    }
  }
}

void MDefinition::dump(GenericPrinter& out) const {
  printName(out);
  out.printf(":%s", StringFromMIRType(type()));
  out.printf(" = ");
  printOpcode(out);
  out.printf("\n");

  if (isInstruction()) {
    if (MResumePoint* resume = toInstruction()->resumePoint()) {
      resume->dump(out);
    }
  }
}

void MDefinition::dump() const {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}

void MDefinition::dumpLocation(GenericPrinter& out) const {
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
    out.printf("  %s %s:%u\n", linkWord, script->filename(), lineno);
    rp = rp->caller();
    linkWord = "in";
  }
}

void MDefinition::dumpLocation() const {
  Fprinter out(stderr);
  dumpLocation(out);
  out.finish();
}
#endif

#ifdef DEBUG
bool MDefinition::trackedSiteMatchesBlock(const BytecodeSite* site) const {
  return site == block()->trackedSite();
}
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)
size_t MDefinition::useCount() const {
  size_t count = 0;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    count++;
  }
  return count;
}

size_t MDefinition::defUseCount() const {
  size_t count = 0;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if ((*i)->consumer()->isDefinition()) {
      count++;
    }
  }
  return count;
}
#endif

bool MDefinition::hasOneUse() const {
  MUseIterator i(uses_.begin());
  if (i == uses_.end()) {
    return false;
  }
  i++;
  return i == uses_.end();
}

bool MDefinition::hasOneDefUse() const {
  bool hasOneDefUse = false;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if (!(*i)->consumer()->isDefinition()) {
      continue;
    }

    // We already have a definition use. So 1+
    if (hasOneDefUse) {
      return false;
    }

    // We saw one definition. Loop to test if there is another.
    hasOneDefUse = true;
  }

  return hasOneDefUse;
}

bool MDefinition::hasDefUses() const {
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if ((*i)->consumer()->isDefinition()) {
      return true;
    }
  }

  return false;
}

bool MDefinition::hasLiveDefUses() const {
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    MNode* ins = (*i)->consumer();
    if (ins->isDefinition()) {
      if (!ins->toDefinition()->isRecoveredOnBailout()) {
        return true;
      }
    } else {
      MOZ_ASSERT(ins->isResumePoint());
      if (!ins->toResumePoint()->isRecoverableOperand(*i)) {
        return true;
      }
    }
  }

  return false;
}

MDefinition* MDefinition::maybeSingleDefUse() const {
  MUseDefIterator use(this);
  if (!use) {
    // No def-uses.
    return nullptr;
  }

  MDefinition* useDef = use.def();

  use++;
  if (use) {
    // More than one def-use.
    return nullptr;
  }

  return useDef;
}

MDefinition* MDefinition::maybeMostRecentlyAddedDefUse() const {
  MUseDefIterator use(this);
  if (!use) {
    // No def-uses.
    return nullptr;
  }

  MDefinition* mostRecentUse = use.def();

#ifdef DEBUG
  // This function relies on addUse adding new uses to the front of the list.
  // Check this invariant by asserting the next few uses are 'older'. Skip this
  // for phis because setBackedge can add a new use for a loop phi even if the
  // loop body has a use with an id greater than the loop phi's id.
  if (!mostRecentUse->isPhi()) {
    static constexpr size_t NumUsesToCheck = 3;
    use++;
    for (size_t i = 0; use && i < NumUsesToCheck; i++, use++) {
      MOZ_ASSERT(use.def()->id() <= mostRecentUse->id());
    }
  }
#endif

  return mostRecentUse;
}

void MDefinition::replaceAllUsesWith(MDefinition* dom) {
  for (size_t i = 0, e = numOperands(); i < e; ++i) {
    getOperand(i)->setImplicitlyUsedUnchecked();
  }

  justReplaceAllUsesWith(dom);
}

void MDefinition::justReplaceAllUsesWith(MDefinition* dom) {
  MOZ_ASSERT(dom != nullptr);
  MOZ_ASSERT(dom != this);

  // Carry over the fact the value has uses which are no longer inspectable
  // with the graph.
  if (isImplicitlyUsed()) {
    dom->setImplicitlyUsedUnchecked();
  }

  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ++i) {
    i->setProducerUnchecked(dom);
  }
  dom->uses_.takeElements(uses_);
}

bool MDefinition::optimizeOutAllUses(TempAllocator& alloc) {
  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
    MUse* use = *i++;
    MConstant* constant = use->consumer()->block()->optimizedOutConstant(alloc);
    if (!alloc.ensureBallast()) {
      return false;
    }

    // Update the resume point operand to use the optimized-out constant.
    use->setProducerUnchecked(constant);
    constant->addUseUnchecked(use);
  }

  // Remove dangling pointers.
  this->uses_.clear();
  return true;
}

void MDefinition::replaceAllLiveUsesWith(MDefinition* dom) {
  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
    MUse* use = *i++;
    MNode* consumer = use->consumer();
    if (consumer->isResumePoint()) {
      continue;
    }
    if (consumer->isDefinition() &&
        consumer->toDefinition()->isRecoveredOnBailout()) {
      continue;
    }

    // Update the operand to use the dominating definition.
    use->replaceProducer(dom);
  }
}

MConstant* MConstant::New(TempAllocator& alloc, const Value& v) {
  return new (alloc) MConstant(alloc, v);
}

MConstant* MConstant::New(TempAllocator::Fallible alloc, const Value& v) {
  return new (alloc) MConstant(alloc.alloc, v);
}

MConstant* MConstant::NewFloat32(TempAllocator& alloc, double d) {
  MOZ_ASSERT(IsNaN(d) || d == double(float(d)));
  return new (alloc) MConstant(float(d));
}

MConstant* MConstant::NewInt64(TempAllocator& alloc, int64_t i) {
  return new (alloc) MConstant(MIRType::Int64, i);
}

MConstant* MConstant::NewIntPtr(TempAllocator& alloc, intptr_t i) {
  return new (alloc) MConstant(MIRType::IntPtr, i);
}

MConstant* MConstant::New(TempAllocator& alloc, const Value& v, MIRType type) {
  if (type == MIRType::Float32) {
    return NewFloat32(alloc, v.toNumber());
  }
  MConstant* res = New(alloc, v);
  MOZ_ASSERT(res->type() == type);
  return res;
}

MConstant* MConstant::NewObject(TempAllocator& alloc, JSObject* v) {
  return new (alloc) MConstant(v);
}

MConstant* MConstant::NewShape(TempAllocator& alloc, Shape* s) {
  return new (alloc) MConstant(s);
}

MConstant::MConstant(TempAllocator& alloc, const js::Value& vp)
    : MNullaryInstruction(classOpcode) {
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
    case MIRType::BigInt:
      MOZ_ASSERT(!IsInsideNursery(vp.toBigInt()));
      payload_.bi = vp.toBigInt();
      break;
    case MIRType::Object:
      MOZ_ASSERT(!IsInsideNursery(&vp.toObject()));
      payload_.obj = &vp.toObject();
      break;
    case MIRType::MagicOptimizedOut:
    case MIRType::MagicHole:
    case MIRType::MagicIsConstructing:
    case MIRType::MagicUninitializedLexical:
      break;
    default:
      MOZ_CRASH("Unexpected type");
  }

  setMovable();
}

MConstant::MConstant(JSObject* obj) : MNullaryInstruction(classOpcode) {
  MOZ_ASSERT(!IsInsideNursery(obj));
  setResultType(MIRType::Object);
  payload_.obj = obj;
  setMovable();
}

MConstant::MConstant(Shape* shape) : MNullaryInstruction(classOpcode) {
  setResultType(MIRType::Shape);
  payload_.shape = shape;
  setMovable();
}

MConstant::MConstant(float f) : MNullaryInstruction(classOpcode) {
  setResultType(MIRType::Float32);
  payload_.f = f;
  setMovable();
}

MConstant::MConstant(MIRType type, int64_t i)
    : MNullaryInstruction(classOpcode) {
  MOZ_ASSERT(type == MIRType::Int64 || type == MIRType::IntPtr);
  setResultType(type);
  if (type == MIRType::Int64) {
    payload_.i64 = i;
  } else {
    payload_.iptr = i;
  }
  setMovable();
}

#ifdef DEBUG
void MConstant::assertInitializedPayload() const {
  // valueHash() and equals() expect the unused payload bits to be
  // initialized to zero. Assert this in debug builds.

  switch (type()) {
    case MIRType::Int32:
    case MIRType::Float32:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT((payload_.asBits >> 32) == 0);
#  else
      MOZ_ASSERT((payload_.asBits << 32) == 0);
#  endif
      break;
    case MIRType::Boolean:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT((payload_.asBits >> 1) == 0);
#  else
      MOZ_ASSERT((payload_.asBits & ~(1ULL << 56)) == 0);
#  endif
      break;
    case MIRType::Double:
    case MIRType::Int64:
      break;
    case MIRType::String:
    case MIRType::Object:
    case MIRType::Symbol:
    case MIRType::BigInt:
    case MIRType::IntPtr:
    case MIRType::Shape:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits >> 32) == 0);
#  else
      MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits << 32) == 0);
#  endif
      break;
    default:
      MOZ_ASSERT(IsNullOrUndefined(type()) || IsMagicType(type()));
      MOZ_ASSERT(payload_.asBits == 0);
      break;
  }
}
#endif

static HashNumber ConstantValueHash(MIRType type, uint64_t payload) {
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

HashNumber MConstant::valueHash() const {
  static_assert(sizeof(Payload) == sizeof(uint64_t),
                "Code below assumes payload fits in 64 bits");

  assertInitializedPayload();
  return ConstantValueHash(type(), payload_.asBits);
}

bool MConstant::congruentTo(const MDefinition* ins) const {
  return ins->isConstant() && equals(ins->toConstant());
}

#ifdef JS_JITSPEW
void MConstant::printOpcode(GenericPrinter& out) const {
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
      out.printf("0x%x", uint32_t(toInt32()));
      break;
    case MIRType::Int64:
      out.printf("0x%" PRIx64, uint64_t(toInt64()));
      break;
    case MIRType::IntPtr:
      out.printf("0x%" PRIxPTR, uintptr_t(toIntPtr()));
      break;
    case MIRType::Double:
      out.printf("%.16g", toDouble());
      break;
    case MIRType::Float32: {
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
        if (fun->hasBaseScript()) {
          BaseScript* script = fun->baseScript();
          out.printf(" (%s:%u)", script->filename() ? script->filename() : "",
                     script->lineno());
        }
        out.printf(" at %p", (void*)fun);
        break;
      }
      out.printf("object %p (%s)", (void*)&toObject(),
                 toObject().getClass()->name);
      break;
    case MIRType::Symbol:
      out.printf("symbol at %p", (void*)toSymbol());
      break;
    case MIRType::BigInt:
      out.printf("BigInt at %p", (void*)toBigInt());
      break;
    case MIRType::String:
      out.printf("string %p", (void*)toString());
      break;
    case MIRType::Shape:
      out.printf("shape at %p", (void*)toShape());
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
#endif

bool MConstant::canProduceFloat32() const {
  if (!isTypeRepresentableAsDouble()) {
    return false;
  }

  if (type() == MIRType::Int32) {
    return IsFloat32Representable(static_cast<double>(toInt32()));
  }
  if (type() == MIRType::Double) {
    return IsFloat32Representable(toDouble());
  }
  MOZ_ASSERT(type() == MIRType::Float32);
  return true;
}

Value MConstant::toJSValue() const {
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
    case MIRType::BigInt:
      return BigIntValue(toBigInt());
    case MIRType::Object:
      return ObjectValue(toObject());
    case MIRType::Shape:
      return PrivateGCThingValue(toShape());
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

bool MConstant::valueToBoolean(bool* res) const {
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
    case MIRType::BigInt:
      *res = !toBigInt()->isZero();
      return true;
    case MIRType::String:
      *res = toString()->length() != 0;
      return true;
    case MIRType::Object:
      // TODO(Warp): Lazy groups have been removed.
      // We have to call EmulatesUndefined but that reads obj->group->clasp
      // and so it's racy when the object has a lazy group. The main callers
      // of this (MTest, MNot) already know how to fold the object case, so
      // just give up.
      return false;
    default:
      MOZ_ASSERT(IsMagicType(type()));
      return false;
  }
}

HashNumber MWasmFloatConstant::valueHash() const {
#ifdef ENABLE_WASM_SIMD
  return ConstantValueHash(type(), u.bits_[0] ^ u.bits_[1]);
#else
  return ConstantValueHash(type(), u.bits_[0]);
#endif
}

bool MWasmFloatConstant::congruentTo(const MDefinition* ins) const {
  return ins->isWasmFloatConstant() && type() == ins->type() &&
#ifdef ENABLE_WASM_SIMD
         u.bits_[1] == ins->toWasmFloatConstant()->u.bits_[1] &&
#endif
         u.bits_[0] == ins->toWasmFloatConstant()->u.bits_[0];
}

HashNumber MWasmNullConstant::valueHash() const {
  return ConstantValueHash(MIRType::RefOrNull, 0);
}

#ifdef JS_JITSPEW
void MControlInstruction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  for (size_t j = 0; j < numSuccessors(); j++) {
    if (getSuccessor(j)) {
      out.printf(" block%u", getSuccessor(j)->id());
    } else {
      out.printf(" (null-to-be-patched)");
    }
  }
}

void MCompare::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", CodeName(jsop()));
}

void MLoadUnboxedScalar::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", Scalar::name(storageType()));
}

void MLoadDataViewElement::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", Scalar::name(storageType()));
}

void MAssertRange::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.put(" ");
  assertedRange()->dump(out);
}

void MNearbyInt::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  const char* roundingModeStr = nullptr;
  switch (roundingMode_) {
    case RoundingMode::Up:
      roundingModeStr = "(up)";
      break;
    case RoundingMode::Down:
      roundingModeStr = "(down)";
      break;
    case RoundingMode::NearestTiesToEven:
      roundingModeStr = "(nearest ties even)";
      break;
    case RoundingMode::TowardsZero:
      roundingModeStr = "(towards zero)";
      break;
  }
  out.printf(" %s", roundingModeStr);
}
#endif

MDefinition* MSign::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() ||
      !input->toConstant()->isTypeRepresentableAsDouble()) {
    return this;
  }

  double in = input->toConstant()->numberToDouble();
  double out = js::math_sign_impl(in);

  if (type() == MIRType::Int32) {
    // Decline folding if this is an int32 operation, but the result type
    // isn't an int32.
    Value outValue = NumberValue(out);
    if (!outValue.isInt32()) {
      return this;
    }

    return MConstant::New(alloc, outValue);
  }

  return MConstant::New(alloc, DoubleValue(out));
}

const char* MMathFunction::FunctionName(UnaryMathFunction function) {
  return GetUnaryMathFunctionName(function);
}

#ifdef JS_JITSPEW
void MMathFunction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", FunctionName(function()));
}
#endif

MDefinition* MMathFunction::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() ||
      !input->toConstant()->isTypeRepresentableAsDouble()) {
    return this;
  }

  UnaryMathFunctionType funPtr = GetUnaryMathFunctionPtr(function());

  double in = input->toConstant()->numberToDouble();

  // The function pointer call can't GC.
  JS::AutoSuppressGCAnalysis nogc;
  double out = funPtr(in);

  if (input->type() == MIRType::Float32) {
    return MConstant::NewFloat32(alloc, out);
  }
  return MConstant::New(alloc, DoubleValue(out));
}

MDefinition* MAtomicIsLockFree::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() || input->type() != MIRType::Int32) {
    return this;
  }

  int32_t i = input->toConstant()->toInt32();
  return MConstant::New(alloc, BooleanValue(AtomicOperations::isLockfreeJS(i)));
}

// Define |THIS_SLOT| as part of this translation unit, as it is used to
// specialized the parameterized |New| function calls introduced by
// TRIVIAL_NEW_WRAPPERS.
const int32_t MParameter::THIS_SLOT;

#ifdef JS_JITSPEW
void MParameter::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  if (index() == THIS_SLOT) {
    out.printf(" THIS_SLOT");
  } else {
    out.printf(" %d", index());
  }
}
#endif

HashNumber MParameter::valueHash() const {
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, index_);
  return hash;
}

bool MParameter::congruentTo(const MDefinition* ins) const {
  if (!ins->isParameter()) {
    return false;
  }

  return ins->toParameter()->index() == index_;
}

WrappedFunction::WrappedFunction(JSFunction* nativeFun, uint16_t nargs,
                                 FunctionFlags flags)
    : nativeFun_(nativeFun), nargs_(nargs), flags_(flags) {
  MOZ_ASSERT_IF(nativeFun, isNativeWithoutJitEntry());

#ifdef DEBUG
  // If we are not running off-main thread we can assert that the
  // metadata is consistent.
  if (!CanUseExtraThreads() && nativeFun) {
    MOZ_ASSERT(nativeFun->nargs() == nargs);

    MOZ_ASSERT(nativeFun->isNativeWithoutJitEntry() ==
               isNativeWithoutJitEntry());
    MOZ_ASSERT(nativeFun->hasJitEntry() == hasJitEntry());
    MOZ_ASSERT(nativeFun->isConstructor() == isConstructor());
    MOZ_ASSERT(nativeFun->isClassConstructor() == isClassConstructor());
  }
#endif
}

MCall* MCall::New(TempAllocator& alloc, WrappedFunction* target, size_t maxArgc,
                  size_t numActualArgs, bool construct, bool ignoresReturnValue,
                  bool isDOMCall, DOMObjectKind objectKind) {
  MOZ_ASSERT(maxArgc >= numActualArgs);
  MCall* ins;
  if (isDOMCall) {
    MOZ_ASSERT(!construct);
    ins = new (alloc) MCallDOMNative(target, numActualArgs, objectKind);
  } else {
    ins =
        new (alloc) MCall(target, numActualArgs, construct, ignoresReturnValue);
  }
  if (!ins->init(alloc, maxArgc + NumNonArgumentOperands)) {
    return nullptr;
  }
  return ins;
}

AliasSet MCallDOMNative::getAliasSet() const {
  const JSJitInfo* jitInfo = getJitInfo();

  // If we don't know anything about the types of our arguments, we have to
  // assume that type-coercions can have side-effects, so we need to alias
  // everything.
  if (jitInfo->aliasSet() == JSJitInfo::AliasEverything ||
      !jitInfo->isTypedMethodJitInfo()) {
    return AliasSet::Store(AliasSet::Any);
  }

  uint32_t argIndex = 0;
  const JSTypedMethodJitInfo* methodInfo =
      reinterpret_cast<const JSTypedMethodJitInfo*>(jitInfo);
  for (const JSJitInfo::ArgType* argType = methodInfo->argTypes;
       *argType != JSJitInfo::ArgTypeListEnd; ++argType, ++argIndex) {
    if (argIndex >= numActualArgs()) {
      // Passing through undefined can't have side-effects
      continue;
    }
    // getArg(0) is "this", so skip it
    MDefinition* arg = getArg(argIndex + 1);
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
        (*argType & JSJitInfo::Object)) {
      return AliasSet::Store(AliasSet::Any);
    }
  }

  // We checked all the args, and they check out.  So we only alias DOM
  // mutations or alias nothing, depending on the alias set in the jitinfo.
  if (jitInfo->aliasSet() == JSJitInfo::AliasNone) {
    return AliasSet::None();
  }

  MOZ_ASSERT(jitInfo->aliasSet() == JSJitInfo::AliasDOMSets);
  return AliasSet::Load(AliasSet::DOMProperty);
}

void MCallDOMNative::computeMovable() {
  // We are movable if the jitinfo says we can be and if we're also not
  // effectful.  The jitinfo can't check for the latter, since it depends on
  // the types of our arguments.
  const JSJitInfo* jitInfo = getJitInfo();

  MOZ_ASSERT_IF(jitInfo->isMovable,
                jitInfo->aliasSet() != JSJitInfo::AliasEverything);

  if (jitInfo->isMovable && !isEffectful()) {
    setMovable();
  }
}

bool MCallDOMNative::congruentTo(const MDefinition* ins) const {
  if (!isMovable()) {
    return false;
  }

  if (!ins->isCall()) {
    return false;
  }

  const MCall* call = ins->toCall();

  if (!call->isCallDOMNative()) {
    return false;
  }

  if (getSingleTarget() != call->getSingleTarget()) {
    return false;
  }

  if (isConstructing() != call->isConstructing()) {
    return false;
  }

  if (numActualArgs() != call->numActualArgs()) {
    return false;
  }

  if (!congruentIfOperandsEqual(call)) {
    return false;
  }

  // The other call had better be movable at this point!
  MOZ_ASSERT(call->isMovable());

  return true;
}

const JSJitInfo* MCallDOMNative::getJitInfo() const {
  MOZ_ASSERT(getSingleTarget()->hasJitInfo());
  return getSingleTarget()->jitInfo();
}

MDefinition* MStringLength::foldsTo(TempAllocator& alloc) {
  if (string()->isConstant()) {
    JSAtom* atom = &string()->toConstant()->toString()->asAtom();
    return MConstant::New(alloc, Int32Value(atom->length()));
  }

  return this;
}

MDefinition* MConcat::foldsTo(TempAllocator& alloc) {
  if (lhs()->isConstant() && lhs()->toConstant()->toString()->empty()) {
    return rhs();
  }

  if (rhs()->isConstant() && rhs()->toConstant()->toString()->empty()) {
    return lhs();
  }

  return this;
}

MDefinition* MCharCodeAt::foldsTo(TempAllocator& alloc) {
  MDefinition* string = this->string();
  if (!string->isConstant()) {
    return this;
  }

  MDefinition* index = this->index();
  if (index->isSpectreMaskIndex()) {
    index = index->toSpectreMaskIndex()->index();
  }
  if (!index->isConstant()) {
    return this;
  }

  JSAtom* atom = &string->toConstant()->toString()->asAtom();
  int32_t idx = index->toConstant()->toInt32();
  if (idx < 0 || uint32_t(idx) >= atom->length()) {
    return this;
  }

  char16_t ch = atom->latin1OrTwoByteChar(idx);
  return MConstant::New(alloc, Int32Value(ch));
}

static bool EnsureFloatInputOrConvert(MUnaryInstruction* owner,
                                      TempAllocator& alloc) {
  MDefinition* input = owner->input();
  if (!input->canProduceFloat32()) {
    if (input->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, input, owner);
    }
    return false;
  }
  return true;
}

void MFloor::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MCeil::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MRound::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MTrunc::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MNearbyInt::trySpecializeFloat32(TempAllocator& alloc) {
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
    setResultType(MIRType::Float32);
  }
}

MGoto* MGoto::New(TempAllocator& alloc, MBasicBlock* target) {
  return new (alloc) MGoto(target);
}

MGoto* MGoto::New(TempAllocator::Fallible alloc, MBasicBlock* target) {
  MOZ_ASSERT(target);
  return new (alloc) MGoto(target);
}

MGoto* MGoto::New(TempAllocator& alloc) { return new (alloc) MGoto(nullptr); }

#ifdef JS_JITSPEW
void MUnbox::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf(" ");
  getOperand(0)->printName(out);
  out.printf(" ");

  switch (type()) {
    case MIRType::Int32:
      out.printf("to Int32");
      break;
    case MIRType::Double:
      out.printf("to Double");
      break;
    case MIRType::Boolean:
      out.printf("to Boolean");
      break;
    case MIRType::String:
      out.printf("to String");
      break;
    case MIRType::Symbol:
      out.printf("to Symbol");
      break;
    case MIRType::BigInt:
      out.printf("to BigInt");
      break;
    case MIRType::Object:
      out.printf("to Object");
      break;
    default:
      break;
  }

  switch (mode()) {
    case Fallible:
      out.printf(" (fallible)");
      break;
    case Infallible:
      out.printf(" (infallible)");
      break;
    default:
      break;
  }
}
#endif

MDefinition* MUnbox::foldsTo(TempAllocator& alloc) {
  if (input()->isBox()) {
    MDefinition* unboxed = input()->toBox()->input();

    // Fold MUnbox(MBox(x)) => x if types match.
    if (unboxed->type() == type()) {
      if (fallible()) {
        unboxed->setImplicitlyUsedUnchecked();
      }
      return unboxed;
    }

    // Fold MUnbox(MBox(x)) => MToDouble(x) if possible.
    if (type() == MIRType::Double &&
        IsTypeRepresentableAsDouble(unboxed->type())) {
      if (unboxed->isConstant()) {
        return MConstant::New(
            alloc, DoubleValue(unboxed->toConstant()->numberToDouble()));
      }

      return MToDouble::New(alloc, unboxed);
    }

    // MUnbox<Int32>(MBox<Double>(x)) will always fail, even if x can be
    // represented as an Int32. Fold to avoid unnecessary bailouts.
    if (type() == MIRType::Int32 && unboxed->type() == MIRType::Double) {
      auto* folded = MToNumberInt32::New(alloc, unboxed,
                                         IntConversionInputKind::NumbersOnly);
      folded->setGuard();
      return folded;
    }
  }

  return this;
}

#ifdef DEBUG
void MPhi::assertLoopPhi() const {
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

void MPhi::removeOperand(size_t index) {
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

void MPhi::removeAllOperands() {
  for (MUse& p : inputs_) {
    p.producer()->removeUse(&p);
  }
  inputs_.clear();
}

MDefinition* MPhi::foldsTernary(TempAllocator& alloc) {
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

  if (numOperands() != 2) {
    return nullptr;
  }

  MOZ_ASSERT(block()->numPredecessors() == 2);

  MBasicBlock* pred = block()->immediateDominator();
  if (!pred || !pred->lastIns()->isTest()) {
    return nullptr;
  }

  MTest* test = pred->lastIns()->toTest();

  // True branch may only dominate one edge of MPhi.
  if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
      test->ifTrue()->dominates(block()->getPredecessor(1))) {
    return nullptr;
  }

  // False branch may only dominate one edge of MPhi.
  if (test->ifFalse()->dominates(block()->getPredecessor(0)) ==
      test->ifFalse()->dominates(block()->getPredecessor(1))) {
    return nullptr;
  }

  // True and false branch must dominate different edges of MPhi.
  if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
      test->ifFalse()->dominates(block()->getPredecessor(0))) {
    return nullptr;
  }

  // We found a ternary construct.
  bool firstIsTrueBranch =
      test->ifTrue()->dominates(block()->getPredecessor(0));
  MDefinition* trueDef = firstIsTrueBranch ? getOperand(0) : getOperand(1);
  MDefinition* falseDef = firstIsTrueBranch ? getOperand(1) : getOperand(0);

  // Accept either
  // testArg ? testArg : constant or
  // testArg ? constant : testArg
  if (!trueDef->isConstant() && !falseDef->isConstant()) {
    return nullptr;
  }

  MConstant* c =
      trueDef->isConstant() ? trueDef->toConstant() : falseDef->toConstant();
  MDefinition* testArg = (trueDef == c) ? falseDef : trueDef;
  if (testArg != test->input()) {
    return nullptr;
  }

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
      !falseDef->block()->dominates(falsePred)) {
    return nullptr;
  }

  // If testArg is an int32 type we can:
  // - fold testArg ? testArg : 0 to testArg
  // - fold testArg ? 0 : testArg to 0
  if (testArg->type() == MIRType::Int32 && c->numberToDouble() == 0) {
    testArg->setGuardRangeBailoutsUnchecked();

    // When folding to the constant we need to hoist it.
    if (trueDef == c && !c->block()->dominates(block())) {
      c->block()->moveBefore(pred->lastIns(), c);
    }
    return trueDef;
  }

  // If testArg is an double type we can:
  // - fold testArg ? testArg : 0.0 to MNaNToZero(testArg)
  if (testArg->type() == MIRType::Double &&
      mozilla::IsPositiveZero(c->numberToDouble()) && c != trueDef) {
    MNaNToZero* replace = MNaNToZero::New(alloc, testArg);
    test->block()->insertBefore(test, replace);
    return replace;
  }

  // If testArg is a string type we can:
  // - fold testArg ? testArg : "" to testArg
  // - fold testArg ? "" : testArg to ""
  if (testArg->type() == MIRType::String &&
      c->toString() == GetJitContext()->runtime->emptyString()) {
    // When folding to the constant we need to hoist it.
    if (trueDef == c && !c->block()->dominates(block())) {
      c->block()->moveBefore(pred->lastIns(), c);
    }
    return trueDef;
  }

  return nullptr;
}

MDefinition* MPhi::operandIfRedundant() {
  if (inputs_.length() == 0) {
    return nullptr;
  }

  // If this phi is redundant (e.g., phi(a,a) or b=phi(a,this)),
  // returns the operand that it will always be equal to (a, in
  // those two cases).
  MDefinition* first = getOperand(0);
  for (size_t i = 1, e = numOperands(); i < e; i++) {
    MDefinition* op = getOperand(i);
    if (op != first && op != this) {
      return nullptr;
    }
  }
  return first;
}

MDefinition* MPhi::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = operandIfRedundant()) {
    return def;
  }

  if (MDefinition* def = foldsTernary(alloc)) {
    return def;
  }

  return this;
}

bool MPhi::congruentTo(const MDefinition* ins) const {
  if (!ins->isPhi()) {
    return false;
  }

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
  if (ins->block() != block()) {
    return false;
  }

  return congruentIfOperandsEqual(ins);
}

bool MPhi::updateForReplacement(MDefinition* def) {
  // This function is called to fix the current Phi flags using it as a
  // replacement of the other Phi instruction |def|.
  //
  // When dealing with usage analysis, any Use will replace all other values,
  // such as Unused and Unknown. Unless both are Unused, the merge would be
  // Unknown.
  MPhi* other = def->toPhi();
  if (usageAnalysis_ == PhiUsage::Used ||
      other->usageAnalysis_ == PhiUsage::Used) {
    usageAnalysis_ = PhiUsage::Used;
  } else if (usageAnalysis_ != other->usageAnalysis_) {
    //    this == unused && other == unknown
    // or this == unknown && other == unused
    usageAnalysis_ = PhiUsage::Unknown;
  } else {
    //    this == unused && other == unused
    // or this == unknown && other = unknown
    MOZ_ASSERT(usageAnalysis_ == PhiUsage::Unused ||
               usageAnalysis_ == PhiUsage::Unknown);
    MOZ_ASSERT(usageAnalysis_ == other->usageAnalysis_);
  }
  return true;
}

/* static */
bool MPhi::markIteratorPhis(const PhiVector& iterators) {
  // Find and mark phis that must transitively hold an iterator live.

  Vector<MPhi*, 8, SystemAllocPolicy> worklist;

  for (MPhi* iter : iterators) {
    if (!iter->isInWorklist()) {
      if (!worklist.append(iter)) {
        return false;
      }
      iter->setInWorklist();
    }
  }

  while (!worklist.empty()) {
    MPhi* phi = worklist.popCopy();
    phi->setNotInWorklist();

    phi->setIterator();
    phi->setImplicitlyUsedUnchecked();

    for (MUseDefIterator iter(phi); iter; iter++) {
      MDefinition* use = iter.def();
      if (!use->isInWorklist() && use->isPhi() && !use->toPhi()->isIterator()) {
        if (!worklist.append(use->toPhi())) {
          return false;
        }
        use->setInWorklist();
      }
    }
  }

  return true;
}

bool MPhi::typeIncludes(MDefinition* def) {
  MOZ_ASSERT(!IsMagicType(def->type()));

  if (def->type() == MIRType::Int32 && this->type() == MIRType::Double) {
    return true;
  }

  if (def->type() == MIRType::Value) {
    // This phi must be able to be any value.
    return this->type() == MIRType::Value;
  }

  return this->mightBeType(def->type());
}

void MCall::addArg(size_t argnum, MDefinition* arg) {
  // The operand vector is initialized in reverse order by WarpBuilder.
  // It cannot be checked for consistency until all arguments are added.
  // FixedList doesn't initialize its elements, so do an unchecked init.
  initOperand(argnum + NumNonArgumentOperands, arg);
}

static inline bool IsConstant(MDefinition* def, double v) {
  if (!def->isConstant()) {
    return false;
  }

  return NumbersAreIdentical(def->toConstant()->numberToDouble(), v);
}

MDefinition* MBinaryBitwiseInstruction::foldsTo(TempAllocator& alloc) {
  // Identity operations are removed (for int32 only) in foldUnnecessaryBitop.

  if (type() == MIRType::Int32) {
    if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
      return folded;
    }
  } else if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
  }

  return this;
}

MDefinition* MBinaryBitwiseInstruction::foldUnnecessaryBitop() {
  // It's probably OK to perform this optimization only for int32, as it will
  // have the greatest effect for asm.js code that is compiled with the JS
  // pipeline, and that code will not see int64 values.

  if (type() != MIRType::Int32) {
    return this;
  }

  // Fold unsigned shift right operator when the second operand is zero and
  // the only use is an unsigned modulo. Thus, the expression
  // |(x >>> 0) % y| becomes |x % y|.
  if (isUrsh() && IsUint32Type(this)) {
    MDefinition* defUse = maybeSingleDefUse();
    if (defUse && defUse->isMod() && defUse->toMod()->isUnsigned()) {
      return getOperand(0);
    }
  }

  // Eliminate bitwise operations that are no-ops when used on integer
  // inputs, such as (x | 0).

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  if (IsConstant(lhs, 0)) {
    return foldIfZero(0);
  }

  if (IsConstant(rhs, 0)) {
    return foldIfZero(1);
  }

  if (IsConstant(lhs, -1)) {
    return foldIfNegOne(0);
  }

  if (IsConstant(rhs, -1)) {
    return foldIfNegOne(1);
  }

  if (lhs == rhs) {
    return foldIfEqual();
  }

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

static inline bool CanProduceNegativeZero(MDefinition* def) {
  // Test if this instruction can produce negative zero even when bailing out
  // and changing types.
  switch (def->op()) {
    case MDefinition::Opcode::Constant:
      if (def->type() == MIRType::Double &&
          def->toConstant()->toDouble() == -0.0) {
        return true;
      }
      [[fallthrough]];
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

static inline bool NeedNegativeZeroCheck(MDefinition* def) {
  if (def->isGuardRangeBailouts()) {
    return true;
  }

  // Test if all uses have the same semantics for -0 and 0
  for (MUseIterator use = def->usesBegin(); use != def->usesEnd(); use++) {
    if (use->consumer()->isResumePoint()) {
      continue;
    }

    MDefinition* use_def = use->consumer()->toDefinition();
    switch (use_def->op()) {
      case MDefinition::Opcode::Add: {
        // If add is truncating -0 and 0 are observed as the same.
        if (use_def->toAdd()->isTruncated()) {
          break;
        }

        // x + y gives -0, when both x and y are -0

        // Figure out the order in which the addition's operands will
        // execute. EdgeCaseAnalysis::analyzeLate has renumbered the MIR
        // definitions for us so that this just requires comparing ids.
        MDefinition* first = use_def->toAdd()->lhs();
        MDefinition* second = use_def->toAdd()->rhs();
        if (first->id() > second->id()) {
          std::swap(first, second);
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
        if (def == first && CanProduceNegativeZero(second)) {
          return true;
        }

        // The negative zero check can always be removed on the second
        // executed operand; by the time this executes the first will have
        // been evaluated as int32 and the addition's result cannot be -0.
        break;
      }
      case MDefinition::Opcode::Sub: {
        // If sub is truncating -0 and 0 are observed as the same
        if (use_def->toSub()->isTruncated()) {
          break;
        }

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
        if (rhs->id() < lhs->id() && CanProduceNegativeZero(lhs)) {
          return true;
        }

        [[fallthrough]];
      }
      case MDefinition::Opcode::StoreElement:
      case MDefinition::Opcode::StoreHoleValueElement:
      case MDefinition::Opcode::LoadElement:
      case MDefinition::Opcode::LoadElementHole:
      case MDefinition::Opcode::LoadUnboxedScalar:
      case MDefinition::Opcode::LoadDataViewElement:
      case MDefinition::Opcode::LoadTypedArrayElementHole:
      case MDefinition::Opcode::CharCodeAt:
      case MDefinition::Opcode::Mod:
      case MDefinition::Opcode::InArray:
        // Only allowed to remove check when definition is the second operand
        if (use_def->getOperand(0) == def) {
          return true;
        }
        for (size_t i = 2, e = use_def->numOperands(); i < e; i++) {
          if (use_def->getOperand(i) == def) {
            return true;
          }
        }
        break;
      case MDefinition::Opcode::BoundsCheck:
        // Only allowed to remove check when definition is the first operand
        if (use_def->toBoundsCheck()->getOperand(1) == def) {
          return true;
        }
        break;
      case MDefinition::Opcode::ToString:
      case MDefinition::Opcode::FromCharCode:
      case MDefinition::Opcode::FromCodePoint:
      case MDefinition::Opcode::TableSwitch:
      case MDefinition::Opcode::Compare:
      case MDefinition::Opcode::BitAnd:
      case MDefinition::Opcode::BitOr:
      case MDefinition::Opcode::BitXor:
      case MDefinition::Opcode::Abs:
      case MDefinition::Opcode::TruncateToInt32:
        // Always allowed to remove check. No matter which operand.
        break;
      case MDefinition::Opcode::StoreElementHole:
      case MDefinition::Opcode::StoreTypedArrayElementHole:
      case MDefinition::Opcode::PostWriteElementBarrier:
        // Only allowed to remove check when definition is the third operand.
        for (size_t i = 0, e = use_def->numOperands(); i < e; i++) {
          if (i == 2) {
            continue;
          }
          if (use_def->getOperand(i) == def) {
            return true;
          }
        }
        break;
      default:
        return true;
    }
  }
  return false;
}

#ifdef JS_JITSPEW
void MBinaryArithInstruction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);

  switch (type()) {
    case MIRType::Int32:
      if (isDiv()) {
        out.printf(" [%s]", toDiv()->isUnsigned() ? "uint32" : "int32");
      } else if (isMod()) {
        out.printf(" [%s]", toMod()->isUnsigned() ? "uint32" : "int32");
      } else {
        out.printf(" [int32]");
      }
      break;
    case MIRType::Int64:
      if (isDiv()) {
        out.printf(" [%s]", toDiv()->isUnsigned() ? "uint64" : "int64");
      } else if (isMod()) {
        out.printf(" [%s]", toMod()->isUnsigned() ? "uint64" : "int64");
      } else {
        out.printf(" [int64]");
      }
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
#endif

MDefinition* MRsh::foldsTo(TempAllocator& alloc) {
  MDefinition* f = MBinaryBitwiseInstruction::foldsTo(alloc);

  if (f != this) {
    return f;
  }

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  // It's probably OK to perform this optimization only for int32, as it will
  // have the greatest effect for asm.js code that is compiled with the JS
  // pipeline, and that code will not see int64 values.

  if (!lhs->isLsh() || !rhs->isConstant() || rhs->type() != MIRType::Int32) {
    return this;
  }

  if (!lhs->getOperand(1)->isConstant() ||
      lhs->getOperand(1)->type() != MIRType::Int32) {
    return this;
  }

  uint32_t shift = rhs->toConstant()->toInt32();
  uint32_t shift_lhs = lhs->getOperand(1)->toConstant()->toInt32();
  if (shift != shift_lhs) {
    return this;
  }

  switch (shift) {
    case 16:
      return MSignExtendInt32::New(alloc, lhs->getOperand(0),
                                   MSignExtendInt32::Half);
    case 24:
      return MSignExtendInt32::New(alloc, lhs->getOperand(0),
                                   MSignExtendInt32::Byte);
  }

  return this;
}

MDefinition* MBinaryArithInstruction::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  if (type() == MIRType::Int64) {
    MOZ_ASSERT(!isTruncated());

    if (MConstant* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      if (!folded->block()) {
        block()->insertBefore(this, folded);
      }
      return folded;
    }
    if (isSub() || isDiv() || isMod()) {
      return this;
    }
    if (rhs->isConstant() &&
        rhs->toConstant()->toInt64() == int64_t(getIdentity())) {
      return lhs;
    }
    if (lhs->isConstant() &&
        lhs->toConstant()->toInt64() == int64_t(getIdentity())) {
      return rhs;
    }
    return this;
  }

  if (MConstant* folded = EvaluateConstantOperands(alloc, this)) {
    if (isTruncated()) {
      if (!folded->block()) {
        block()->insertBefore(this, folded);
      }
      if (folded->type() != MIRType::Int32) {
        return MTruncateToInt32::New(alloc, folded);
      }
    }
    return folded;
  }

  if (mustPreserveNaN_) {
    return this;
  }

  // 0 + -0 = 0. So we can't remove addition
  if (isAdd() && type() != MIRType::Int32) {
    return this;
  }

  if (IsConstant(rhs, getIdentity())) {
    if (isTruncated()) {
      return MTruncateToInt32::New(alloc, lhs);
    }
    return lhs;
  }

  // subtraction isn't commutative. So we can't remove subtraction when lhs
  // equals 0
  if (isSub()) {
    return this;
  }

  if (IsConstant(lhs, getIdentity())) {
    if (isTruncated()) {
      return MTruncateToInt32::New(alloc, rhs);
    }
    return rhs;  // id op x => x
  }

  return this;
}

void MBinaryArithInstruction::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  // Do not use Float32 if we can use int32.
  if (type() == MIRType::Int32) {
    return;
  }

  MDefinition* left = lhs();
  MDefinition* right = rhs();

  if (!left->canProduceFloat32() || !right->canProduceFloat32() ||
      !CheckUsesAreFloat32Consumers(this)) {
    if (left->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, left, this);
    }
    if (right->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<1>(alloc, right, this);
    }
    return;
  }

  setResultType(MIRType::Float32);
}

void MMinMax::trySpecializeFloat32(TempAllocator& alloc) {
  if (type() == MIRType::Int32) {
    return;
  }

  MDefinition* left = lhs();
  MDefinition* right = rhs();

  if (!(left->canProduceFloat32() ||
        (left->isMinMax() && left->type() == MIRType::Float32)) ||
      !(right->canProduceFloat32() ||
        (right->isMinMax() && right->type() == MIRType::Float32))) {
    if (left->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, left, this);
    }
    if (right->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<1>(alloc, right, this);
    }
    return;
  }

  setResultType(MIRType::Float32);
}

MDefinition* MMinMax::foldsTo(TempAllocator& alloc) {
  if (lhs() == rhs()) {
    return lhs();
  }
  if (!lhs()->isConstant() && !rhs()->isConstant()) {
    return this;
  }

  // Directly apply math utility to compare the rhs() and lhs() when
  // they are both constants.
  if (lhs()->isConstant() && rhs()->isConstant()) {
    if (!lhs()->toConstant()->isTypeRepresentableAsDouble() ||
        !rhs()->toConstant()->isTypeRepresentableAsDouble()) {
      return this;
    }

    double lnum = lhs()->toConstant()->numberToDouble();
    double rnum = rhs()->toConstant()->numberToDouble();

    double result;
    if (isMax()) {
      result = js::math_max_impl(lnum, rnum);
    } else {
      result = js::math_min_impl(lnum, rnum);
    }

    // The folded MConstant should maintain the same MIRType with
    // the original MMinMax.
    if (type() == MIRType::Int32) {
      int32_t cast;
      if (mozilla::NumberEqualsInt32(result, &cast)) {
        return MConstant::New(alloc, Int32Value(cast));
      }
    } else if (type() == MIRType::Float32) {
      return MConstant::NewFloat32(alloc, result);
    } else {
      MOZ_ASSERT(type() == MIRType::Double);
      return MConstant::New(alloc, DoubleValue(result));
    }
  }

  MDefinition* operand = lhs()->isConstant() ? rhs() : lhs();
  MConstant* constant =
      lhs()->isConstant() ? lhs()->toConstant() : rhs()->toConstant();

  if (operand->isToDouble() &&
      operand->getOperand(0)->type() == MIRType::Int32) {
    // min(int32, cte >= INT32_MAX) = int32
    if (!isMax() && constant->isTypeRepresentableAsDouble() &&
        constant->numberToDouble() >= INT32_MAX) {
      MLimitedTruncate* limit = MLimitedTruncate::New(
          alloc, operand->getOperand(0), TruncateKind::NoTruncate);
      block()->insertBefore(this, limit);
      MToDouble* toDouble = MToDouble::New(alloc, limit);
      return toDouble;
    }

    // max(int32, cte <= INT32_MIN) = int32
    if (isMax() && constant->isTypeRepresentableAsDouble() &&
        constant->numberToDouble() <= INT32_MIN) {
      MLimitedTruncate* limit = MLimitedTruncate::New(
          alloc, operand->getOperand(0), TruncateKind::NoTruncate);
      block()->insertBefore(this, limit);
      MToDouble* toDouble = MToDouble::New(alloc, limit);
      return toDouble;
    }
  }

  if ((operand->isArrayLength() || operand->isArrayBufferViewLength()) &&
      constant->type() == MIRType::Int32) {
    MOZ_ASSERT(operand->type() == MIRType::Int32);

    // (Typed)ArrayLength is always >= 0.
    // max(array.length, cte <= 0) = array.length
    // min(array.length, cte <= 0) = cte
    if (constant->toInt32() <= 0) {
      return isMax() ? operand : constant;
    }
  }

  return this;
}

MDefinition* MPow::foldsConstant(TempAllocator& alloc) {
  // Both `x` and `p` in `x^p` must be constants in order to precompute.
  if (!input()->isConstant() || !power()->isConstant()) {
    return nullptr;
  }
  if (!power()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }
  if (!input()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }

  double x = input()->toConstant()->numberToDouble();
  double p = power()->toConstant()->numberToDouble();
  double result = js::ecmaPow(x, p);
  if (type() == MIRType::Int32) {
    int32_t cast;
    if (!mozilla::NumberIsInt32(result, &cast)) {
      // Reject folding if the result isn't an int32, because we'll bail anyway.
      return nullptr;
    }
    return MConstant::New(alloc, Int32Value(cast));
  }
  return MConstant::New(alloc, DoubleValue(result));
}

MDefinition* MPow::foldsConstantPower(TempAllocator& alloc) {
  // If `p` in `x^p` isn't constant, we can't apply these folds.
  if (!power()->isConstant()) {
    return nullptr;
  }
  if (!power()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }

  MOZ_ASSERT(type() == MIRType::Double || type() == MIRType::Int32);

  double pow = power()->toConstant()->numberToDouble();

  // Math.pow(x, 0.5) is a sqrt with edge-case detection.
  if (pow == 0.5) {
    MOZ_ASSERT(type() == MIRType::Double);
    return MPowHalf::New(alloc, input());
  }

  // Math.pow(x, -0.5) == 1 / Math.pow(x, 0.5), even for edge cases.
  if (pow == -0.5) {
    MOZ_ASSERT(type() == MIRType::Double);
    MPowHalf* half = MPowHalf::New(alloc, input());
    block()->insertBefore(this, half);
    MConstant* one = MConstant::New(alloc, DoubleValue(1.0));
    block()->insertBefore(this, one);
    return MDiv::New(alloc, one, half, MIRType::Double);
  }

  // Math.pow(x, 1) == x.
  if (pow == 1.0) {
    return input();
  }

  auto multiply = [this, &alloc](MDefinition* lhs, MDefinition* rhs) {
    MMul* mul = MMul::New(alloc, lhs, rhs, type());
    mul->setBailoutKind(bailoutKind());

    // Multiplying the same number can't yield negative zero.
    mul->setCanBeNegativeZero(lhs != rhs && canBeNegativeZero());
    return mul;
  };

  // Math.pow(x, 2) == x*x.
  if (pow == 2.0) {
    return multiply(input(), input());
  }

  // Math.pow(x, 3) == x*x*x.
  if (pow == 3.0) {
    MMul* mul1 = multiply(input(), input());
    block()->insertBefore(this, mul1);
    return multiply(input(), mul1);
  }

  // Math.pow(x, 4) == y*y, where y = x*x.
  if (pow == 4.0) {
    MMul* y = multiply(input(), input());
    block()->insertBefore(this, y);
    return multiply(y, y);
  }

  // No optimization
  return nullptr;
}

MDefinition* MPow::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsConstant(alloc)) {
    return def;
  }
  if (MDefinition* def = foldsConstantPower(alloc)) {
    return def;
  }
  return this;
}

MDefinition* MInt32ToIntPtr::foldsTo(TempAllocator& alloc) {
  MDefinition* def = input();
  if (def->isConstant()) {
    int32_t i = def->toConstant()->toInt32();
    return MConstant::NewIntPtr(alloc, intptr_t(i));
  }

  if (def->isNonNegativeIntPtrToInt32()) {
    return def->toNonNegativeIntPtrToInt32()->input();
  }

  return this;
}

bool MAbs::fallible() const {
  return !implicitTruncate_ && (!range() || !range()->hasInt32Bounds());
}

void MAbs::trySpecializeFloat32(TempAllocator& alloc) {
  // Do not use Float32 if we can use int32.
  if (input()->type() == MIRType::Int32) {
    return;
  }

  if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
    if (input()->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, input(), this);
    }
    return;
  }

  setResultType(MIRType::Float32);
}

MDefinition* MDiv::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
    return this;
  }

  if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
    return folded;
  }

  if (MDefinition* folded = EvaluateExactReciprocal(alloc, this)) {
    return folded;
  }

  return this;
}

void MDiv::analyzeEdgeCasesForward() {
  // This is only meaningful when doing integer division.
  if (type() != MIRType::Int32) {
    return;
  }

  MOZ_ASSERT(lhs()->type() == MIRType::Int32);
  MOZ_ASSERT(rhs()->type() == MIRType::Int32);

  // Try removing divide by zero check
  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0)) {
    canBeDivideByZero_ = false;
  }

  // If lhs is a constant int != INT32_MIN, then
  // negative overflow check can be skipped.
  if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(INT32_MIN)) {
    canBeNegativeOverflow_ = false;
  }

  // If rhs is a constant int != -1, likewise.
  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(-1)) {
    canBeNegativeOverflow_ = false;
  }

  // If lhs is != 0, then negative zero check can be skipped.
  if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(0)) {
    setCanBeNegativeZero(false);
  }

  // If rhs is >= 0, likewise.
  if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
    if (rhs()->toConstant()->toInt32() >= 0) {
      setCanBeNegativeZero(false);
    }
  }
}

void MDiv::analyzeEdgeCasesBackward() {
  if (canBeNegativeZero() && !NeedNegativeZeroCheck(this)) {
    setCanBeNegativeZero(false);
  }
}

bool MDiv::fallible() const { return !isTruncated(); }

MDefinition* MMod::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
  } else {
    if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
      return folded;
    }
  }
  return this;
}

void MMod::analyzeEdgeCasesForward() {
  // These optimizations make sense only for integer division
  if (type() != MIRType::Int32) {
    return;
  }

  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0)) {
    canBeDivideByZero_ = false;
  }

  if (rhs()->isConstant()) {
    int32_t n = rhs()->toConstant()->toInt32();
    if (n > 0 && !IsPowerOfTwo(uint32_t(n))) {
      canBePowerOfTwoDivisor_ = false;
    }
  }
}

bool MMod::fallible() const {
  return !isTruncated() &&
         (isUnsigned() || canBeDivideByZero() || canBeNegativeDividend());
}

void MMathFunction::trySpecializeFloat32(TempAllocator& alloc) {
  if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
    if (input()->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, input(), this);
    }
    return;
  }

  setResultType(MIRType::Float32);
  specialization_ = MIRType::Float32;
}

bool MMathFunction::isFloat32Commutative() const {
  switch (function_) {
    case UnaryMathFunction::Floor:
    case UnaryMathFunction::Ceil:
    case UnaryMathFunction::Round:
    case UnaryMathFunction::Trunc:
      return true;
    default:
      return false;
  }
}

bool MMathFunction::canRecoverOnBailout() const {
  switch (function_) {
    case UnaryMathFunction::Sin:
    case UnaryMathFunction::Log:
    case UnaryMathFunction::Ceil:
    case UnaryMathFunction::Floor:
    case UnaryMathFunction::Round:
    case UnaryMathFunction::Trunc:
      return true;
    default:
      return false;
  }
}

MHypot* MHypot::New(TempAllocator& alloc, const MDefinitionVector& vector) {
  uint32_t length = vector.length();
  MHypot* hypot = new (alloc) MHypot;
  if (!hypot->init(alloc, length)) {
    return nullptr;
  }

  for (uint32_t i = 0; i < length; ++i) {
    hypot->initOperand(i, vector[i]);
  }
  return hypot;
}

bool MAdd::fallible() const {
  // the add is fallible if range analysis does not say that it is finite, AND
  // either the truncation analysis shows that there are non-truncated uses.
  if (truncateKind() >= TruncateKind::IndirectTruncate) {
    return false;
  }
  if (range() && range()->hasInt32Bounds()) {
    return false;
  }
  return true;
}

bool MSub::fallible() const {
  // see comment in MAdd::fallible()
  if (truncateKind() >= TruncateKind::IndirectTruncate) {
    return false;
  }
  if (range() && range()->hasInt32Bounds()) {
    return false;
  }
  return true;
}

MDefinition* MSub::foldsTo(TempAllocator& alloc) {
  MDefinition* out = MBinaryArithInstruction::foldsTo(alloc);
  if (out != this) {
    return out;
  }

  if (type() != MIRType::Int32) {
    return this;
  }

  // Optimize X - X to 0. This optimization is only valid for Int32
  // values. Subtracting a floating point value from itself returns
  // NaN when the operand is either Infinity or NaN.
  if (lhs() == rhs()) {
    // Ensure that any bailouts that we depend on to guarantee that X
    // is Int32 are not removed.
    lhs()->setGuardRangeBailoutsUnchecked();
    return MConstant::New(alloc, Int32Value(0));
  }

  return this;
}

MDefinition* MMul::foldsTo(TempAllocator& alloc) {
  MDefinition* out = MBinaryArithInstruction::foldsTo(alloc);
  if (out != this) {
    return out;
  }

  if (type() != MIRType::Int32) {
    return this;
  }

  if (lhs() == rhs()) {
    setCanBeNegativeZero(false);
  }

  return this;
}

void MMul::analyzeEdgeCasesForward() {
  // Try to remove the check for negative zero
  // This only makes sense when using the integer multiplication
  if (type() != MIRType::Int32) {
    return;
  }

  // If lhs is > 0, no need for negative zero check.
  if (lhs()->isConstant() && lhs()->type() == MIRType::Int32) {
    if (lhs()->toConstant()->toInt32() > 0) {
      setCanBeNegativeZero(false);
    }
  }

  // If rhs is > 0, likewise.
  if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
    if (rhs()->toConstant()->toInt32() > 0) {
      setCanBeNegativeZero(false);
    }
  }
}

void MMul::analyzeEdgeCasesBackward() {
  if (canBeNegativeZero() && !NeedNegativeZeroCheck(this)) {
    setCanBeNegativeZero(false);
  }
}

bool MMul::updateForReplacement(MDefinition* ins_) {
  MMul* ins = ins_->toMul();
  bool negativeZero = canBeNegativeZero() || ins->canBeNegativeZero();
  setCanBeNegativeZero(negativeZero);
  // Remove the imul annotation when merging imul and normal multiplication.
  if (mode_ == Integer && ins->mode() != Integer) {
    mode_ = Normal;
  }
  return true;
}

bool MMul::canOverflow() const {
  if (isTruncated()) {
    return false;
  }
  return !range() || !range()->hasInt32Bounds();
}

bool MUrsh::fallible() const {
  if (bailoutsDisabled()) {
    return false;
  }
  return !range() || !range()->hasInt32Bounds();
}

MIRType MCompare::inputType() {
  switch (compareType_) {
    case Compare_Undefined:
      return MIRType::Undefined;
    case Compare_Null:
      return MIRType::Null;
    case Compare_UInt32:
    case Compare_Int32:
      return MIRType::Int32;
    case Compare_UIntPtr:
      return MIRType::IntPtr;
    case Compare_Double:
      return MIRType::Double;
    case Compare_Float32:
      return MIRType::Float32;
    case Compare_String:
      return MIRType::String;
    case Compare_Symbol:
      return MIRType::Symbol;
    case Compare_Object:
      return MIRType::Object;
    case Compare_BigInt:
    case Compare_BigInt_Int32:
    case Compare_BigInt_Double:
    case Compare_BigInt_String:
      return MIRType::BigInt;
    default:
      MOZ_CRASH("No known conversion");
  }
}

static inline bool MustBeUInt32(MDefinition* def, MDefinition** pwrapped) {
  if (def->isUrsh()) {
    *pwrapped = def->toUrsh()->lhs();
    MDefinition* rhs = def->toUrsh()->rhs();
    return def->toUrsh()->bailoutsDisabled() && rhs->maybeConstantValue() &&
           rhs->maybeConstantValue()->isInt32(0);
  }

  if (MConstant* defConst = def->maybeConstantValue()) {
    *pwrapped = defConst;
    return defConst->type() == MIRType::Int32 && defConst->toInt32() >= 0;
  }

  *pwrapped = nullptr;  // silence GCC warning
  return false;
}

/* static */
bool MBinaryInstruction::unsignedOperands(MDefinition* left,
                                          MDefinition* right) {
  MDefinition* replace;
  if (!MustBeUInt32(left, &replace)) {
    return false;
  }
  if (replace->type() != MIRType::Int32) {
    return false;
  }
  if (!MustBeUInt32(right, &replace)) {
    return false;
  }
  if (replace->type() != MIRType::Int32) {
    return false;
  }
  return true;
}

bool MBinaryInstruction::unsignedOperands() {
  return unsignedOperands(getOperand(0), getOperand(1));
}

void MBinaryInstruction::replaceWithUnsignedOperands() {
  MOZ_ASSERT(unsignedOperands());

  for (size_t i = 0; i < numOperands(); i++) {
    MDefinition* replace;
    MustBeUInt32(getOperand(i), &replace);
    if (replace == getOperand(i)) {
      continue;
    }

    getOperand(i)->setImplicitlyUsedUnchecked();
    replaceOperand(i, replace);
  }
}

MDefinition* MBitNot::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);

  MDefinition* input = getOperand(0);

  if (input->isConstant()) {
    js::Value v = Int32Value(~(input->toConstant()->toInt32()));
    return MConstant::New(alloc, v);
  }

  if (input->isBitNot()) {
    MOZ_ASSERT(input->toBitNot()->type() == MIRType::Int32);
    MOZ_ASSERT(input->toBitNot()->getOperand(0)->type() == MIRType::Int32);
    return MTruncateToInt32::New(alloc,
                                 input->toBitNot()->input());  // ~~x => x | 0
  }

  return this;
}

static void AssertKnownClass(TempAllocator& alloc, MInstruction* ins,
                             MDefinition* obj) {
#ifdef DEBUG
  const JSClass* clasp = GetObjectKnownJSClass(obj);
  MOZ_ASSERT(clasp);

  auto* assert = MAssertClass::New(alloc, obj, clasp);
  ins->block()->insertBefore(ins, assert);
#endif
}

MDefinition* MBoxNonStrictThis::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (in->isBox()) {
    in = in->toBox()->input();
  }

  if (in->type() == MIRType::Object) {
    return in;
  }

  return this;
}

AliasSet MLoadArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MArgumentsObjectLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MGuardArgumentsObjectFlags::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardArgumentsObjectFlags() ||
      ins->toGuardArgumentsObjectFlags()->flags() != flags()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardArgumentsObjectFlags::getAliasSet() const {
  // The flags are packed with the length in a fixed private slot.
  return AliasSet::Load(AliasSet::FixedSlot);
}

MDefinition* MReturnFromCtor::foldsTo(TempAllocator& alloc) {
  MDefinition* rval = value();
  if (rval->isBox()) {
    rval = rval->toBox()->input();
  }

  if (rval->type() == MIRType::Object) {
    return rval;
  }

  if (rval->type() != MIRType::Value) {
    return object();
  }

  return this;
}

MDefinition* MTypeOf::foldsTo(TempAllocator& alloc) {
  MDefinition* unboxed = input();
  if (unboxed->isBox()) {
    unboxed = unboxed->toBox()->input();
  }

  JSType type;
  switch (unboxed->type()) {
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
    case MIRType::BigInt:
      type = JSTYPE_BIGINT;
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
    case MIRType::Object: {
      KnownClass known = GetObjectKnownClass(unboxed);
      if (known != KnownClass::None) {
        if (known == KnownClass::Function) {
          type = JSTYPE_FUNCTION;
        } else {
          type = JSTYPE_OBJECT;
        }

        AssertKnownClass(alloc, this, unboxed);
        break;
      }
      [[fallthrough]];
    }
    default:
      return this;
  }

  return MConstant::New(
      alloc, StringValue(TypeName(type, GetJitContext()->runtime->names())));
}

MUrsh* MUrsh::NewWasm(TempAllocator& alloc, MDefinition* left,
                      MDefinition* right, MIRType type) {
  MUrsh* ins = new (alloc) MUrsh(left, right, type);

  // Since Ion has no UInt32 type, we use Int32 and we have a special
  // exception to the type rules: we can return values in
  // (INT32_MIN,UINT32_MAX] and still claim that we have an Int32 type
  // without bailing out. This is necessary because Ion has no UInt32
  // type and we can't have bailouts in wasm code.
  ins->bailoutsDisabled_ = true;

  return ins;
}

MResumePoint* MResumePoint::New(TempAllocator& alloc, MBasicBlock* block,
                                jsbytecode* pc, Mode mode) {
  MResumePoint* resume = new (alloc) MResumePoint(block, pc, mode);
  if (!resume->init(alloc)) {
    block->discardPreAllocatedResumePoint(resume);
    return nullptr;
  }
  resume->inherit(block);
  return resume;
}

MResumePoint::MResumePoint(MBasicBlock* block, jsbytecode* pc, Mode mode)
    : MNode(block, Kind::ResumePoint),
      pc_(pc),
      instruction_(nullptr),
      mode_(mode) {
  block->addResumePoint(this);
}

bool MResumePoint::init(TempAllocator& alloc) {
  return operands_.init(alloc, block()->stackDepth());
}

MResumePoint* MResumePoint::caller() const {
  return block()->callerResumePoint();
}

void MResumePoint::inherit(MBasicBlock* block) {
  // FixedList doesn't initialize its elements, so do unchecked inits.
  for (size_t i = 0; i < stackDepth(); i++) {
    initOperand(i, block->getSlot(i));
  }
}

void MResumePoint::addStore(TempAllocator& alloc, MDefinition* store,
                            const MResumePoint* cache) {
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

  MStoreToRecover* top = new (alloc) MStoreToRecover(store);
  stores_.push(top);
}

#ifdef JS_JITSPEW
void MResumePoint::dump(GenericPrinter& out) const {
  out.printf("resumepoint mode=");

  switch (mode()) {
    case MResumePoint::ResumeAt:
      if (instruction_) {
        out.printf("At(%u)", instruction_->id());
      } else {
        out.printf("At");
      }
      break;
    case MResumePoint::ResumeAfter:
      out.printf("After");
      break;
    case MResumePoint::Outer:
      out.printf("Outer");
      break;
  }

  if (MResumePoint* c = caller()) {
    out.printf(" (caller in block%u)", c->block()->id());
  }

  for (size_t i = 0; i < numOperands(); i++) {
    out.printf(" ");
    if (operands_[i].hasProducer()) {
      getOperand(i)->printName(out);
    } else {
      out.printf("(null)");
    }
  }
  out.printf("\n");
}

void MResumePoint::dump() const {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}
#endif

bool MResumePoint::isObservableOperand(MUse* u) const {
  return isObservableOperand(indexOf(u));
}

bool MResumePoint::isObservableOperand(size_t index) const {
  return block()->info().isObservableSlot(index);
}

bool MResumePoint::isRecoverableOperand(MUse* u) const {
  return block()->info().isRecoverableOperand(indexOf(u));
}

MDefinition* MTruncateBigIntToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);

  if (input->isBox()) {
    input = input->getOperand(0);
  }

  // If the operand converts an I64 to BigInt, drop both conversions.
  if (input->isInt64ToBigInt()) {
    return input->getOperand(0);
  }

  // Fold this operation if the input operand is constant.
  if (input->isConstant()) {
    return MConstant::NewInt64(
        alloc, BigInt::toInt64(input->toConstant()->toBigInt()));
  }

  return this;
}

MDefinition* MToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);

  if (input->isBox()) {
    input = input->getOperand(0);
  }

  // Unwrap MInt64ToBigInt: MToInt64(MInt64ToBigInt(int64)) = int64.
  if (input->isInt64ToBigInt()) {
    return input->getOperand(0);
  }

  // When the input is an Int64 already, just return it.
  if (input->type() == MIRType::Int64) {
    return input;
  }

  // Fold this operation if the input operand is constant.
  if (input->isConstant()) {
    switch (input->type()) {
      case MIRType::Boolean:
        return MConstant::NewInt64(alloc, input->toConstant()->toBoolean());
      default:
        break;
    }
  }

  return this;
}

MDefinition* MToNumberInt32::foldsTo(TempAllocator& alloc) {
  // Fold this operation if the input operand is constant.
  if (MConstant* cst = input()->maybeConstantValue()) {
    switch (cst->type()) {
      case MIRType::Null:
        if (conversion() == IntConversionInputKind::Any) {
          return MConstant::New(alloc, Int32Value(0));
        }
        break;
      case MIRType::Boolean:
        if (conversion() == IntConversionInputKind::Any ||
            conversion() == IntConversionInputKind::NumbersOrBoolsOnly) {
          return MConstant::New(alloc, Int32Value(cst->toBoolean()));
        }
        break;
      case MIRType::Int32:
        return MConstant::New(alloc, Int32Value(cst->toInt32()));
      case MIRType::Float32:
      case MIRType::Double:
        int32_t ival;
        // Only the value within the range of Int32 can be substituted as
        // constant.
        if (mozilla::NumberIsInt32(cst->numberToDouble(), &ival)) {
          return MConstant::New(alloc, Int32Value(ival));
        }
        break;
      default:
        break;
    }
  }

  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->toBox()->input();
  }

  // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
  // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
  // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
  // is folded to a MTruncateToInt32 node, which will result in this MIR:
  // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
  // the MUrsh node's type is int32 (since uint32 is not implemented), and
  // that would fold the MTruncateToInt32 node. This will make the modulo
  // unsigned, while is should have been signed.
  if (input->type() == MIRType::Int32 && !IsUint32Type(input)) {
    return input;
  }

  return this;
}

MDefinition* MToIntegerInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);

  // Fold this operation if the input operand is constant.
  if (input->isConstant()) {
    switch (input->type()) {
      case MIRType::Undefined:
      case MIRType::Null:
        return MConstant::New(alloc, Int32Value(0));
      case MIRType::Boolean:
        return MConstant::New(alloc,
                              Int32Value(input->toConstant()->toBoolean()));
      case MIRType::Int32:
        return MConstant::New(alloc,
                              Int32Value(input->toConstant()->toInt32()));
      case MIRType::Float32:
      case MIRType::Double: {
        double result = JS::ToInteger(input->toConstant()->numberToDouble());
        int32_t ival;
        // Only the value within the range of Int32 can be substituted as
        // constant.
        if (mozilla::NumberEqualsInt32(result, &ival)) {
          return MConstant::New(alloc, Int32Value(ival));
        }
        break;
      }
      default:
        break;
    }
  }

  // See the comment in |MToNumberInt32::foldsTo|.
  if (input->type() == MIRType::Int32 && !IsUint32Type(input)) {
    return input;
  }

  return this;
}

void MToNumberInt32::analyzeEdgeCasesBackward() {
  if (!NeedNegativeZeroCheck(this)) {
    setNeedsNegativeZeroCheck(false);
  }
}

MDefinition* MTruncateToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
  // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
  // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
  // is folded to a MTruncateToInt32 node, which will result in this MIR:
  // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
  // the MUrsh node's type is int32 (since uint32 is not implemented), and
  // that would fold the MTruncateToInt32 node. This will make the modulo
  // unsigned, while is should have been signed.
  if (input->type() == MIRType::Int32 && !IsUint32Type(input)) {
    return input;
  }

  if (input->type() == MIRType::Double && input->isConstant()) {
    int32_t ret = ToInt32(input->toConstant()->toDouble());
    return MConstant::New(alloc, Int32Value(ret));
  }

  return this;
}

MDefinition* MWasmTruncateToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->type() == MIRType::Int32) {
    return input;
  }

  if (input->type() == MIRType::Double && input->isConstant()) {
    double d = input->toConstant()->toDouble();
    if (IsNaN(d)) {
      return this;
    }

    if (!isUnsigned() && d <= double(INT32_MAX) && d >= double(INT32_MIN)) {
      return MConstant::New(alloc, Int32Value(ToInt32(d)));
    }

    if (isUnsigned() && d <= double(UINT32_MAX) && d >= 0) {
      return MConstant::New(alloc, Int32Value(ToInt32(d)));
    }
  }

  if (input->type() == MIRType::Float32 && input->isConstant()) {
    double f = double(input->toConstant()->toFloat32());
    if (IsNaN(f)) {
      return this;
    }

    if (!isUnsigned() && f <= double(INT32_MAX) && f >= double(INT32_MIN)) {
      return MConstant::New(alloc, Int32Value(ToInt32(f)));
    }

    if (isUnsigned() && f <= double(UINT32_MAX) && f >= 0) {
      return MConstant::New(alloc, Int32Value(ToInt32(f)));
    }
  }

  return this;
}

MDefinition* MWrapInt64ToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    uint64_t c = input->toConstant()->toInt64();
    int32_t output = bottomHalf() ? int32_t(c) : int32_t(c >> 32);
    return MConstant::New(alloc, Int32Value(output));
  }

  return this;
}

MDefinition* MExtendInt32ToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int32_t c = input->toConstant()->toInt32();
    int64_t res = isUnsigned() ? int64_t(uint32_t(c)) : int64_t(c);
    return MConstant::NewInt64(alloc, res);
  }

  return this;
}

MDefinition* MSignExtendInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int32_t c = input->toConstant()->toInt32();
    int32_t res;
    switch (mode_) {
      case Byte:
        res = int32_t(int8_t(c & 0xFF));
        break;
      case Half:
        res = int32_t(int16_t(c & 0xFFFF));
        break;
    }
    return MConstant::New(alloc, Int32Value(res));
  }

  return this;
}

MDefinition* MSignExtendInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int64_t c = input->toConstant()->toInt64();
    int64_t res;
    switch (mode_) {
      case Byte:
        res = int64_t(int8_t(c & 0xFF));
        break;
      case Half:
        res = int64_t(int16_t(c & 0xFFFF));
        break;
      case Word:
        res = int64_t(int32_t(c & 0xFFFFFFFFU));
        break;
    }
    return MConstant::NewInt64(alloc, res);
  }

  return this;
}

MDefinition* MToDouble::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  if (input->type() == MIRType::Double) {
    return input;
  }

  if (input->isConstant() &&
      input->toConstant()->isTypeRepresentableAsDouble()) {
    return MConstant::New(alloc,
                          DoubleValue(input->toConstant()->numberToDouble()));
  }

  return this;
}

MDefinition* MToFloat32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  if (input->type() == MIRType::Float32) {
    return input;
  }

  // If x is a Float32, Float32(Double(x)) == x
  if (!mustPreserveNaN_ && input->isToDouble() &&
      input->toToDouble()->input()->type() == MIRType::Float32) {
    return input->toToDouble()->input();
  }

  if (input->isConstant() &&
      input->toConstant()->isTypeRepresentableAsDouble()) {
    return MConstant::NewFloat32(alloc,
                                 float(input->toConstant()->numberToDouble()));
  }

  // Fold ToFloat32(ToDouble(int32)) to ToFloat32(int32).
  if (input->isToDouble() &&
      input->toToDouble()->input()->type() == MIRType::Int32) {
    return MToFloat32::New(alloc, input->toToDouble()->input());
  }

  return this;
}

MDefinition* MToString::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (in->isBox()) {
    in = in->getOperand(0);
  }

  if (in->type() == MIRType::String) {
    return in;
  }
  return this;
}

MDefinition* MClampToUint8::foldsTo(TempAllocator& alloc) {
  if (MConstant* inputConst = input()->maybeConstantValue()) {
    if (inputConst->isTypeRepresentableAsDouble()) {
      int32_t clamped = ClampDoubleToUint8(inputConst->numberToDouble());
      return MConstant::New(alloc, Int32Value(clamped));
    }
  }
  return this;
}

bool MCompare::tryFoldEqualOperands(bool* result) {
  if (lhs() != rhs()) {
    return false;
  }

  // Intuitively somebody would think that if lhs === rhs,
  // then we can just return true. (Or false for !==)
  // However NaN !== NaN is true! So we spend some time trying
  // to eliminate this case.

  if (!IsStrictEqualityOp(jsop())) {
    return false;
  }

  MOZ_ASSERT(
      compareType_ == Compare_Undefined || compareType_ == Compare_Null ||
      compareType_ == Compare_Int32 || compareType_ == Compare_UInt32 ||
      compareType_ == Compare_Double || compareType_ == Compare_Float32 ||
      compareType_ == Compare_UIntPtr || compareType_ == Compare_String ||
      compareType_ == Compare_Object || compareType_ == Compare_Symbol ||
      compareType_ == Compare_BigInt || compareType_ == Compare_BigInt_Int32 ||
      compareType_ == Compare_BigInt_Double ||
      compareType_ == Compare_BigInt_String);

  if (isDoubleComparison() || isFloat32Comparison()) {
    if (!operandsAreNeverNaN()) {
      return false;
    }
  }

  lhs()->setGuardRangeBailoutsUnchecked();

  *result = (jsop() == JSOp::StrictEq);
  return true;
}

bool MCompare::tryFoldTypeOf(bool* result) {
  if (!lhs()->isTypeOf() && !rhs()->isTypeOf()) {
    return false;
  }
  if (!lhs()->isConstant() && !rhs()->isConstant()) {
    return false;
  }

  MTypeOf* typeOf = lhs()->isTypeOf() ? lhs()->toTypeOf() : rhs()->toTypeOf();
  MConstant* constant =
      lhs()->isConstant() ? lhs()->toConstant() : rhs()->toConstant();

  if (constant->type() != MIRType::String) {
    return false;
  }

  if (!IsEqualityOp(jsop())) {
    return false;
  }

  const JSAtomState& names = GetJitContext()->runtime->names();
  if (constant->toString() == TypeName(JSTYPE_BOOLEAN, names)) {
    if (!typeOf->input()->mightBeType(MIRType::Boolean)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  } else if (constant->toString() == TypeName(JSTYPE_NUMBER, names)) {
    if (!typeOf->input()->mightBeType(MIRType::Int32) &&
        !typeOf->input()->mightBeType(MIRType::Float32) &&
        !typeOf->input()->mightBeType(MIRType::Double)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  } else if (constant->toString() == TypeName(JSTYPE_STRING, names)) {
    if (!typeOf->input()->mightBeType(MIRType::String)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  } else if (constant->toString() == TypeName(JSTYPE_SYMBOL, names)) {
    if (!typeOf->input()->mightBeType(MIRType::Symbol)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  } else if (constant->toString() == TypeName(JSTYPE_BIGINT, names)) {
    if (!typeOf->input()->mightBeType(MIRType::BigInt)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  } else if (constant->toString() == TypeName(JSTYPE_OBJECT, names)) {
    if (!typeOf->input()->mightBeType(MIRType::Object) &&
        !typeOf->input()->mightBeType(MIRType::Null)) {
      *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
      return true;
    }
  }

  return false;
}

bool MCompare::tryFold(bool* result) {
  JSOp op = jsop();

  if (tryFoldEqualOperands(result)) {
    return true;
  }

  if (tryFoldTypeOf(result)) {
    return true;
  }

  if (compareType_ == Compare_Null || compareType_ == Compare_Undefined) {
    // The LHS is the value we want to test against null or undefined.
    if (IsStrictEqualityOp(op)) {
      if (lhs()->type() == inputType()) {
        *result = (op == JSOp::StrictEq);
        return true;
      }
      if (!lhs()->mightBeType(inputType())) {
        *result = (op == JSOp::StrictNe);
        return true;
      }
    } else {
      MOZ_ASSERT(IsLooseEqualityOp(op));
      if (IsNullOrUndefined(lhs()->type())) {
        *result = (op == JSOp::Eq);
        return true;
      }
      if (!lhs()->mightBeType(MIRType::Null) &&
          !lhs()->mightBeType(MIRType::Undefined) &&
          !lhs()->mightBeType(MIRType::Object)) {
        *result = (op == JSOp::Ne);
        return true;
      }
    }
    return false;
  }

  return false;
}

template <typename T>
static bool FoldComparison(JSOp op, T left, T right) {
  switch (op) {
    case JSOp::Lt:
      return left < right;
    case JSOp::Le:
      return left <= right;
    case JSOp::Gt:
      return left > right;
    case JSOp::Ge:
      return left >= right;
    case JSOp::StrictEq:
    case JSOp::Eq:
      return left == right;
    case JSOp::StrictNe:
    case JSOp::Ne:
      return left != right;
    default:
      MOZ_CRASH("Unexpected op.");
  }
}

bool MCompare::evaluateConstantOperands(TempAllocator& alloc, bool* result) {
  if (type() != MIRType::Boolean && type() != MIRType::Int32) {
    return false;
  }

  MDefinition* left = getOperand(0);
  MDefinition* right = getOperand(1);

  if (compareType() == Compare_Double) {
    // Optimize "MCompare MConstant (MToDouble SomethingInInt32Range).
    // In most cases the MToDouble was added, because the constant is
    // a double.
    // e.g. v < 9007199254740991, where v is an int32 is always true.
    if (!lhs()->isConstant() && !rhs()->isConstant()) {
      return false;
    }

    MDefinition* operand = left->isConstant() ? right : left;
    MConstant* constant =
        left->isConstant() ? left->toConstant() : right->toConstant();
    MOZ_ASSERT(constant->type() == MIRType::Double);
    double cte = constant->toDouble();

    if (operand->isToDouble() &&
        operand->getOperand(0)->type() == MIRType::Int32) {
      bool replaced = false;
      switch (jsop_) {
        case JSOp::Lt:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = !((constant == lhs()) ^ (cte < INT32_MIN));
            replaced = true;
          }
          break;
        case JSOp::Le:
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
        case JSOp::Gt:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = !((constant == rhs()) ^ (cte < INT32_MIN));
            replaced = true;
          }
          break;
        case JSOp::Ge:
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
        case JSOp::StrictEq:  // Fall through.
        case JSOp::Eq:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = false;
            replaced = true;
          }
          break;
        case JSOp::StrictNe:  // Fall through.
        case JSOp::Ne:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = true;
            replaced = true;
          }
          break;
        default:
          MOZ_CRASH("Unexpected op.");
      }
      if (replaced) {
        MLimitedTruncate* limit = MLimitedTruncate::New(
            alloc, operand->getOperand(0), TruncateKind::NoTruncate);
        limit->setGuardUnchecked();
        block()->insertBefore(this, limit);
        return true;
      }
    }
  }

  if (!left->isConstant() || !right->isConstant()) {
    return false;
  }

  MConstant* lhs = left->toConstant();
  MConstant* rhs = right->toConstant();

  // Fold away some String equality comparisons.
  if (lhs->type() == MIRType::String && rhs->type() == MIRType::String) {
    int32_t comp = 0;  // Default to equal.
    if (left != right) {
      comp =
          CompareAtoms(&lhs->toString()->asAtom(), &rhs->toString()->asAtom());
    }
    *result = FoldComparison(jsop_, comp, 0);
    return true;
  }

  if (compareType_ == Compare_UInt32) {
    *result = FoldComparison(jsop_, uint32_t(lhs->toInt32()),
                             uint32_t(rhs->toInt32()));
    return true;
  }

  if (compareType_ == Compare_Int64) {
    *result = FoldComparison(jsop_, lhs->toInt64(), rhs->toInt64());
    return true;
  }

  if (compareType_ == Compare_UInt64) {
    *result = FoldComparison(jsop_, uint64_t(lhs->toInt64()),
                             uint64_t(rhs->toInt64()));
    return true;
  }

  if (lhs->isTypeRepresentableAsDouble() &&
      rhs->isTypeRepresentableAsDouble()) {
    *result =
        FoldComparison(jsop_, lhs->numberToDouble(), rhs->numberToDouble());
    return true;
  }

  return false;
}

MDefinition* MCompare::tryFoldCharCompare(TempAllocator& alloc) {
  if (compareType() != Compare_String) {
    return this;
  }

  MDefinition* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::String);

  MDefinition* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::String);

  // |str[i]| is compiled as |MFromCharCode(MCharCodeAt(str, i))|.
  auto isCharAccess = [](MDefinition* ins) {
    return ins->isFromCharCode() &&
           ins->toFromCharCode()->input()->isCharCodeAt();
  };

  if (left->isConstant() || right->isConstant()) {
    // Try to optimize |MConstant(string) <compare> (MFromCharCode MCharCodeAt)|
    // as |MConstant(charcode) <compare> MCharCodeAt|.
    MConstant* constant;
    MDefinition* operand;
    if (left->isConstant()) {
      constant = left->toConstant();
      operand = right;
    } else {
      constant = right->toConstant();
      operand = left;
    }

    if (constant->toString()->length() != 1 || !isCharAccess(operand)) {
      return this;
    }

    char16_t charCode = constant->toString()->asAtom().latin1OrTwoByteChar(0);
    MConstant* charCodeConst = MConstant::New(alloc, Int32Value(charCode));
    block()->insertBefore(this, charCodeConst);

    MDefinition* charCodeAt = operand->toFromCharCode()->input();

    if (left->isConstant()) {
      left = charCodeConst;
      right = charCodeAt;
    } else {
      left = charCodeAt;
      right = charCodeConst;
    }
  } else if (isCharAccess(left) && isCharAccess(right)) {
    // Try to optimize |(MFromCharCode MCharCodeAt) <compare> (MFromCharCode
    // MCharCodeAt)| as |MCharCodeAt <compare> MCharCodeAt|.

    left = left->toFromCharCode()->input();
    right = right->toFromCharCode()->input();
  } else {
    return this;
  }

  return MCompare::New(alloc, left, right, jsop(), MCompare::Compare_Int32);
}

MDefinition* MCompare::foldsTo(TempAllocator& alloc) {
  bool result;

  if (tryFold(&result) || evaluateConstantOperands(alloc, &result)) {
    if (type() == MIRType::Int32) {
      return MConstant::New(alloc, Int32Value(result));
    }

    MOZ_ASSERT(type() == MIRType::Boolean);
    return MConstant::New(alloc, BooleanValue(result));
  }

  if (MDefinition* folded = tryFoldCharCompare(alloc); folded != this) {
    return folded;
  }

  return this;
}

void MCompare::trySpecializeFloat32(TempAllocator& alloc) {
  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  if (lhs->canProduceFloat32() && rhs->canProduceFloat32() &&
      compareType_ == Compare_Double) {
    compareType_ = Compare_Float32;
  } else {
    if (lhs->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, lhs, this);
    }
    if (rhs->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<1>(alloc, rhs, this);
    }
  }
}

MDefinition* MNot::foldsTo(TempAllocator& alloc) {
  // Fold if the input is constant
  if (MConstant* inputConst = input()->maybeConstantValue()) {
    bool b;
    if (inputConst->valueToBoolean(&b)) {
      if (type() == MIRType::Int32 || type() == MIRType::Int64) {
        return MConstant::New(alloc, Int32Value(!b));
      }
      return MConstant::New(alloc, BooleanValue(!b));
    }
  }

  // If the operand of the Not is itself a Not, they cancel out. But we can't
  // always convert Not(Not(x)) to x because that may loose the conversion to
  // boolean. We can simplify Not(Not(Not(x))) to Not(x) though.
  MDefinition* op = getOperand(0);
  if (op->isNot()) {
    MDefinition* opop = op->getOperand(0);
    if (opop->isNot()) {
      return opop;
    }
  }

  // Not of an undefined or null value is always true
  if (input()->type() == MIRType::Undefined ||
      input()->type() == MIRType::Null) {
    return MConstant::New(alloc, BooleanValue(true));
  }

  // Not of a symbol is always false.
  if (input()->type() == MIRType::Symbol) {
    return MConstant::New(alloc, BooleanValue(false));
  }

  return this;
}

void MNot::trySpecializeFloat32(TempAllocator& alloc) {
  MDefinition* in = input();
  if (!in->canProduceFloat32() && in->type() == MIRType::Float32) {
    ConvertDefinitionToDouble<0>(alloc, in, this);
  }
}

#ifdef JS_JITSPEW
void MBeta::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);

  out.printf(" ");
  comparison_->dump(out);
}
#endif

bool MCreateThisWithTemplate::canRecoverOnBailout() const {
  MOZ_ASSERT(templateObject()->is<PlainObject>());
  return true;
}

AliasSet MCreateThis::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

bool MGetArgumentsObjectArg::congruentTo(const MDefinition* ins) const {
  if (!ins->isGetArgumentsObjectArg()) {
    return false;
  }
  if (ins->toGetArgumentsObjectArg()->argno() != argno()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGetArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MSetArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Store(AliasSet::Any);
}

MObjectState::MObjectState(MObjectState* state)
    : MVariadicInstruction(classOpcode),
      numSlots_(state->numSlots_),
      numFixedSlots_(state->numFixedSlots_) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();
}

MObjectState::MObjectState(JSObject* templateObject)
    : MObjectState(templateObject->as<NativeObject>().shape()) {}

MObjectState::MObjectState(const Shape* shape)
    : MVariadicInstruction(classOpcode) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();

  numSlots_ = shape->slotSpan();
  numFixedSlots_ = shape->numFixedSlots();
}

/* static */
JSObject* MObjectState::templateObjectOf(MDefinition* obj) {
  // MNewPlainObject uses a shape constant, not an object.
  MOZ_ASSERT(!obj->isNewPlainObject());

  if (obj->isNewObject()) {
    return obj->toNewObject()->templateObject();
  } else if (obj->isCreateThisWithTemplate()) {
    return obj->toCreateThisWithTemplate()->templateObject();
  } else if (obj->isNewCallObject()) {
    return obj->toNewCallObject()->templateObject();
  } else if (obj->isNewIterator()) {
    return obj->toNewIterator()->templateObject();
  }

  MOZ_CRASH("unreachable");
}

bool MObjectState::init(TempAllocator& alloc, MDefinition* obj) {
  if (!MVariadicInstruction::init(alloc, numSlots() + 1)) {
    return false;
  }
  // +1, for the Object.
  initOperand(0, obj);
  return true;
}

bool MObjectState::initFromTemplateObject(TempAllocator& alloc,
                                          MDefinition* undefinedVal) {
  if (object()->isNewPlainObject()) {
    MOZ_ASSERT(object()->toNewPlainObject()->shape()->slotSpan() == numSlots());
    for (size_t i = 0; i < numSlots(); i++) {
      initSlot(i, undefinedVal);
    }

    return true;
  }

  JSObject* templateObject = templateObjectOf(object());

  // Initialize all the slots of the object state with the value contained in
  // the template object. This is needed to account values which are baked in
  // the template objects and not visible in IonMonkey, such as the
  // uninitialized-lexical magic value of call objects.

  MOZ_ASSERT(templateObject->is<NativeObject>());
  NativeObject& nativeObject = templateObject->as<NativeObject>();
  MOZ_ASSERT(nativeObject.slotSpan() == numSlots());

  for (size_t i = 0; i < numSlots(); i++) {
    Value val = nativeObject.getSlot(i);
    MDefinition* def = undefinedVal;
    if (!val.isUndefined()) {
      MConstant* ins = MConstant::New(alloc, val);
      block()->insertBefore(this, ins);
      def = ins;
    }
    initSlot(i, def);
  }

  return true;
}

MObjectState* MObjectState::New(TempAllocator& alloc, MDefinition* obj) {
  MObjectState* res;
  if (obj->isNewPlainObject()) {
    const Shape* shape = obj->toNewPlainObject()->shape();
    res = new (alloc) MObjectState(shape);
  } else {
    JSObject* templateObject = templateObjectOf(obj);
    MOZ_ASSERT(templateObject, "Unexpected object creation.");
    res = new (alloc) MObjectState(templateObject);
  }

  if (!res || !res->init(alloc, obj)) {
    return nullptr;
  }
  return res;
}

MObjectState* MObjectState::Copy(TempAllocator& alloc, MObjectState* state) {
  MObjectState* res = new (alloc) MObjectState(state);
  if (!res || !res->init(alloc, state->object())) {
    return nullptr;
  }
  for (size_t i = 0; i < res->numSlots(); i++) {
    res->initSlot(i, state->getSlot(i));
  }
  return res;
}

MArrayState::MArrayState(MDefinition* arr) : MVariadicInstruction(classOpcode) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();
  if (arr->isNewArrayObject()) {
    numElements_ = arr->toNewArrayObject()->length();
  } else {
    numElements_ = arr->toNewArray()->length();
  }
}

bool MArrayState::init(TempAllocator& alloc, MDefinition* obj,
                       MDefinition* len) {
  if (!MVariadicInstruction::init(alloc, numElements() + 2)) {
    return false;
  }
  // +1, for the Array object.
  initOperand(0, obj);
  // +1, for the length value of the array.
  initOperand(1, len);
  return true;
}

bool MArrayState::initFromTemplateObject(TempAllocator& alloc,
                                         MDefinition* undefinedVal) {
  for (size_t i = 0; i < numElements(); i++) {
    initElement(i, undefinedVal);
  }
  return true;
}

MArrayState* MArrayState::New(TempAllocator& alloc, MDefinition* arr,
                              MDefinition* initLength) {
  MArrayState* res = new (alloc) MArrayState(arr);
  if (!res || !res->init(alloc, arr, initLength)) {
    return nullptr;
  }
  return res;
}

MArrayState* MArrayState::Copy(TempAllocator& alloc, MArrayState* state) {
  MDefinition* arr = state->array();
  MDefinition* len = state->initializedLength();
  MArrayState* res = new (alloc) MArrayState(arr);
  if (!res || !res->init(alloc, arr, len)) {
    return nullptr;
  }
  for (size_t i = 0; i < res->numElements(); i++) {
    res->initElement(i, state->getElement(i));
  }
  return res;
}

MNewArray::MNewArray(uint32_t length, MConstant* templateConst,
                     gc::InitialHeap initialHeap, bool vmCall)
    : MUnaryInstruction(classOpcode, templateConst),
      length_(length),
      initialHeap_(initialHeap),
      vmCall_(vmCall) {
  setResultType(MIRType::Object);
}

MDefinition::AliasType MLoadFixedSlot::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreFixedSlot()) {
    const MStoreFixedSlot* store = def->toStoreFixedSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }
    if (store->object() != object()) {
      return AliasType::MayAlias;
    }
    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadFixedSlot::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

MDefinition::AliasType MLoadFixedSlotAndUnbox::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreFixedSlot()) {
    const MStoreFixedSlot* store = def->toStoreFixedSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }
    if (store->object() != object()) {
      return AliasType::MayAlias;
    }
    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadFixedSlotAndUnbox::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

MDefinition* MWasmAddOffset::foldsTo(TempAllocator& alloc) {
  MDefinition* baseArg = base();
  if (!baseArg->isConstant()) {
    return this;
  }

  MOZ_ASSERT(baseArg->type() == MIRType::Int32);
  CheckedInt<uint32_t> ptr = baseArg->toConstant()->toInt32();

  ptr += offset();

  if (!ptr.isValid()) {
    return this;
  }

  return MConstant::New(alloc, Int32Value(ptr.value()));
}

bool MWasmAlignmentCheck::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmAlignmentCheck()) {
    return false;
  }
  const MWasmAlignmentCheck* check = ins->toWasmAlignmentCheck();
  return byteSize_ == check->byteSize() && congruentIfOperandsEqual(check);
}

MDefinition::AliasType MAsmJSLoadHeap::mightAlias(
    const MDefinition* def) const {
  if (def->isAsmJSStoreHeap()) {
    const MAsmJSStoreHeap* store = def->toAsmJSStoreHeap();
    if (store->accessType() != accessType()) {
      return AliasType::MayAlias;
    }
    if (!base()->isConstant() || !store->base()->isConstant()) {
      return AliasType::MayAlias;
    }
    const MConstant* otherBase = store->base()->toConstant();
    if (base()->toConstant()->equals(otherBase)) {
      return AliasType::MayAlias;
    }
    return AliasType::NoAlias;
  }
  return AliasType::MayAlias;
}

bool MAsmJSLoadHeap::congruentTo(const MDefinition* ins) const {
  if (!ins->isAsmJSLoadHeap()) {
    return false;
  }
  const MAsmJSLoadHeap* load = ins->toAsmJSLoadHeap();
  return load->accessType() == accessType() && congruentIfOperandsEqual(load);
}

MDefinition::AliasType MWasmLoadGlobalVar::mightAlias(
    const MDefinition* def) const {
  if (def->isWasmStoreGlobalVar()) {
    const MWasmStoreGlobalVar* store = def->toWasmStoreGlobalVar();
    return store->globalDataOffset() == globalDataOffset_ ? AliasType::MayAlias
                                                          : AliasType::NoAlias;
  }

  return AliasType::MayAlias;
}

MDefinition::AliasType MWasmLoadGlobalCell::mightAlias(
    const MDefinition* def) const {
  if (def->isWasmStoreGlobalCell()) {
    // No globals of different type can alias.  See bug 1467415 comment 3.
    if (type() != def->toWasmStoreGlobalCell()->value()->type()) {
      return AliasType::NoAlias;
    }

    // We could do better here.  We're dealing with two indirect globals.
    // If at at least one of them is created in this module, then they
    // can't alias -- in other words they can only alias if they are both
    // imported.  That would require having a flag on globals to indicate
    // which are imported.  See bug 1467415 comment 3, 4th rule.
  }

  return AliasType::MayAlias;
}

HashNumber MWasmLoadGlobalVar::valueHash() const {
  // Same comment as in MWasmLoadGlobalVar::congruentTo() applies here.
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, globalDataOffset_);
  return hash;
}

bool MWasmLoadGlobalVar::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmLoadGlobalVar()) {
    return false;
  }

  const MWasmLoadGlobalVar* other = ins->toWasmLoadGlobalVar();

  // We don't need to consider the isConstant_ markings here, because
  // equivalence of offsets implies equivalence of constness.
  bool sameOffsets = globalDataOffset_ == other->globalDataOffset_;
  MOZ_ASSERT_IF(sameOffsets, isConstant_ == other->isConstant_);

  // We omit checking congruence of the operands.  There is only one
  // operand, the TLS pointer, and it only ever has one value within the
  // domain of optimization.  If that should ever change then operand
  // congruence checking should be reinstated.
  return sameOffsets /* && congruentIfOperandsEqual(other) */;
}

MDefinition* MWasmLoadGlobalVar::foldsTo(TempAllocator& alloc) {
  if (!dependency() || !dependency()->isWasmStoreGlobalVar()) {
    return this;
  }

  MWasmStoreGlobalVar* store = dependency()->toWasmStoreGlobalVar();
  if (!store->block()->dominates(block())) {
    return this;
  }

  if (store->globalDataOffset() != globalDataOffset()) {
    return this;
  }

  if (store->value()->type() != type()) {
    return this;
  }

  return store->value();
}

bool MWasmLoadGlobalCell::congruentTo(const MDefinition* ins) const {
  if (!ins->isWasmLoadGlobalCell()) {
    return false;
  }
  const MWasmLoadGlobalCell* other = ins->toWasmLoadGlobalCell();
  return congruentIfOperandsEqual(other);
}

#ifdef ENABLE_WASM_SIMD
MDefinition* MWasmBitselectSimd128::foldsTo(TempAllocator& alloc) {
  if (control()->op() == MDefinition::Opcode::WasmFloatConstant) {
    int8_t shuffle[16];
    if (specializeConstantMaskAsShuffle(shuffle)) {
      return MWasmShuffleSimd128::New(alloc, lhs(), rhs(),
                                      SimdConstant::CreateX16(shuffle));
    }
  }
  return this;
}

MDefinition* MWasmBinarySimd128::foldsTo(TempAllocator& alloc) {
  if (simdOp() == wasm::SimdOp::V8x16Swizzle && rhs()->isWasmFloatConstant()) {
    // Specialize swizzle(v, constant) as shuffle(mask, v, zero) to trigger all
    // our shuffle optimizations.  We don't report this rewriting as the report
    // will be overwritten by the subsequent shuffle analysis.
    int8_t shuffleMask[16];
    memcpy(shuffleMask, rhs()->toWasmFloatConstant()->toSimd128().bytes(), 16);
    for (int i = 0; i < 16; i++) {
      // Out-of-bounds lanes reference the zero vector; in many cases, the zero
      // vector is removed by subsequent optimizations.
      if (shuffleMask[i] < 0 || shuffleMask[i] > 15) {
        shuffleMask[i] = 16;
      }
    }
    MWasmFloatConstant* zero =
        MWasmFloatConstant::NewSimd128(alloc, SimdConstant::SplatX4(0));
    if (!zero) {
      return nullptr;
    }
    block()->insertBefore(this, zero);
    return MWasmShuffleSimd128::New(alloc, lhs(), zero,
                                    SimdConstant::CreateX16(shuffleMask));
  }

  // Specialize var OP const / const OP var when possible.
  //
  // As the LIR layer can't directly handle v128 constants as part of its normal
  // machinery we specialize some nodes here if they have single-use v128
  // constant arguments.  The purpose is to generate code that inlines the
  // constant in the instruction stream, using either a rip-relative load+op or
  // quickly-synthesized constant in a scratch on x64.  There is a general
  // assumption here that that is better than generating the constant into an
  // allocatable register, since that register value could not be reused. (This
  // ignores the possibility that the constant load could be hoisted).

  if (lhs()->isWasmFloatConstant() != rhs()->isWasmFloatConstant() &&
      specializeForConstantRhs()) {
    if (isCommutative() && lhs()->isWasmFloatConstant() && lhs()->hasOneUse()) {
      return MWasmBinarySimd128WithConstant::New(
          alloc, rhs(), lhs()->toWasmFloatConstant()->toSimd128(), simdOp());
    }

    if (rhs()->isWasmFloatConstant() && rhs()->hasOneUse()) {
      return MWasmBinarySimd128WithConstant::New(
          alloc, lhs(), rhs()->toWasmFloatConstant()->toSimd128(), simdOp());
    }
  }

  return this;
}

MDefinition* MWasmScalarToSimd128::foldsTo(TempAllocator& alloc) {
#  ifdef DEBUG
  auto logging = mozilla::MakeScopeExit([&] {
    js::wasm::ReportSimdAnalysis("scalar-to-simd128 -> constant folded");
  });
#  endif
  if (input()->isConstant()) {
    MConstant* c = input()->toConstant();
    switch (simdOp()) {
      case wasm::SimdOp::I8x16Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX16(c->toInt32()));
      case wasm::SimdOp::I16x8Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX8(c->toInt32()));
      case wasm::SimdOp::I32x4Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX4(c->toInt32()));
      case wasm::SimdOp::I64x2Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX2(c->toInt64()));
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
  }
  if (input()->isWasmFloatConstant()) {
    MWasmFloatConstant* c = input()->toWasmFloatConstant();
    switch (simdOp()) {
      case wasm::SimdOp::F32x4Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX4(c->toFloat32()));
      case wasm::SimdOp::F64x2Splat:
        return MWasmFloatConstant::NewSimd128(
            alloc, SimdConstant::SplatX2(c->toDouble()));
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
  }
#  ifdef DEBUG
  logging.release();
#  endif
  return this;
}

template <typename T>
static bool AllTrue(const T& v) {
  constexpr size_t count = sizeof(T) / sizeof(*v);
  static_assert(count == 16 || count == 8 || count == 4 || count == 2);
  bool result = true;
  for (unsigned i = 0; i < count; i++) {
    result = result && v[i] != 0;
  }
  return result;
}

template <typename T>
static int32_t Bitmask(const T& v) {
  constexpr size_t count = sizeof(T) / sizeof(*v);
  constexpr size_t shift = 8 * sizeof(*v) - 1;
  static_assert(shift == 7 || shift == 15 || shift == 31 || shift == 63);
  int32_t result = 0;
  for (unsigned i = 0; i < count; i++) {
    result = result | int32_t(((v[i] >> shift) & 1) << i);
  }
  return result;
}

MDefinition* MWasmReduceSimd128::foldsTo(TempAllocator& alloc) {
#  ifdef DEBUG
  auto logging = mozilla::MakeScopeExit([&] {
    js::wasm::ReportSimdAnalysis("simd128-to-scalar -> constant folded");
  });
#  endif
  if (input()->isWasmFloatConstant()) {
    SimdConstant c = input()->toWasmFloatConstant()->toSimd128();
    int32_t i32Result = 0;
    switch (simdOp()) {
      case wasm::SimdOp::V128AnyTrue:
        i32Result = !c.isZeroBits();
        break;
      case wasm::SimdOp::I8x16AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16());
        break;
      case wasm::SimdOp::I8x16Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16());
        break;
      case wasm::SimdOp::I16x8AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8());
        break;
      case wasm::SimdOp::I16x8Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8());
        break;
      case wasm::SimdOp::I32x4AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4());
        break;
      case wasm::SimdOp::I32x4Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4());
        break;
      case wasm::SimdOp::I64x2AllTrue:
        i32Result = AllTrue(
            SimdConstant::CreateSimd128((int64_t*)c.bytes()).asInt64x2());
        break;
      case wasm::SimdOp::I64x2Bitmask:
        i32Result = Bitmask(
            SimdConstant::CreateSimd128((int64_t*)c.bytes()).asInt64x2());
        break;
      case wasm::SimdOp::I8x16ExtractLaneS:
        i32Result =
            SimdConstant::CreateSimd128((int8_t*)c.bytes()).asInt8x16()[imm()];
        break;
      case wasm::SimdOp::I8x16ExtractLaneU:
        i32Result = int32_t(SimdConstant::CreateSimd128((int8_t*)c.bytes())
                                .asInt8x16()[imm()]) &
                    0xFF;
        break;
      case wasm::SimdOp::I16x8ExtractLaneS:
        i32Result =
            SimdConstant::CreateSimd128((int16_t*)c.bytes()).asInt16x8()[imm()];
        break;
      case wasm::SimdOp::I16x8ExtractLaneU:
        i32Result = int32_t(SimdConstant::CreateSimd128((int16_t*)c.bytes())
                                .asInt16x8()[imm()]) &
                    0xFFFF;
        break;
      case wasm::SimdOp::I32x4ExtractLane:
        i32Result =
            SimdConstant::CreateSimd128((int32_t*)c.bytes()).asInt32x4()[imm()];
        break;
      case wasm::SimdOp::I64x2ExtractLane:
        return MConstant::NewInt64(
            alloc, SimdConstant::CreateSimd128((int64_t*)c.bytes())
                       .asInt64x2()[imm()]);
      case wasm::SimdOp::F32x4ExtractLane:
        return MWasmFloatConstant::NewFloat32(
            alloc, SimdConstant::CreateSimd128((float*)c.bytes())
                       .asFloat32x4()[imm()]);
      case wasm::SimdOp::F64x2ExtractLane:
        return MWasmFloatConstant::NewDouble(
            alloc, SimdConstant::CreateSimd128((double*)c.bytes())
                       .asFloat64x2()[imm()]);
      default:
#  ifdef DEBUG
        logging.release();
#  endif
        return this;
    }
    return MConstant::New(alloc, Int32Value(i32Result), MIRType::Int32);
  }
#  ifdef DEBUG
  logging.release();
#  endif
  return this;
}
#endif  // ENABLE_WASM_SIMD

MDefinition::AliasType MLoadDynamicSlot::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreDynamicSlot()) {
    const MStoreDynamicSlot* store = def->toStoreDynamicSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }

    if (store->slots() != slots()) {
      return AliasType::MayAlias;
    }

    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

HashNumber MLoadDynamicSlot::valueHash() const {
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, slot_);
  return hash;
}

MDefinition* MLoadDynamicSlot::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

#ifdef JS_JITSPEW
void MLoadDynamicSlot::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %u", slot());
}

void MStoreDynamicSlot::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf(" ");
  getOperand(0)->printName(out);
  out.printf(" %u ", slot());
  getOperand(1)->printName(out);
}
#endif

MDefinition* MGuardFunctionScript::foldsTo(TempAllocator& alloc) {
  if (input()->isLambda() &&
      input()->toLambda()->info().baseScript == expected()) {
    return input();
  }
  if (input()->isLambdaArrow() &&
      input()->toLambdaArrow()->info().baseScript == expected()) {
    return input();
  }
  return this;
}

MDefinition* MFunctionEnvironment::foldsTo(TempAllocator& alloc) {
  if (input()->isLambda()) {
    return input()->toLambda()->environmentChain();
  }
  if (input()->isLambdaArrow()) {
    return input()->toLambdaArrow()->environmentChain();
  }
  if (input()->isFunctionWithProto()) {
    return input()->toFunctionWithProto()->environmentChain();
  }
  return this;
}

static bool AddIsANonZeroAdditionOf(MAdd* add, MDefinition* ins) {
  if (add->lhs() != ins && add->rhs() != ins) {
    return false;
  }
  MDefinition* other = (add->lhs() == ins) ? add->rhs() : add->lhs();
  if (!IsNumberType(other->type())) {
    return false;
  }
  if (!other->isConstant()) {
    return false;
  }
  if (other->toConstant()->numberToDouble() == 0) {
    return false;
  }
  return true;
}

// Skip over instructions that usually appear between the actual index
// value being used and the MLoadElement.
// They don't modify the index value in a meaningful way.
static MDefinition* SkipUninterestingInstructions(MDefinition* ins) {
  // Drop the MToNumberInt32 added by the TypePolicy for double and float
  // values.
  if (ins->isToNumberInt32()) {
    return SkipUninterestingInstructions(ins->toToNumberInt32()->input());
  }

  // Ignore the bounds check, which don't modify the index.
  if (ins->isBoundsCheck()) {
    return SkipUninterestingInstructions(ins->toBoundsCheck()->index());
  }

  // Masking the index for Spectre-mitigation is not observable.
  if (ins->isSpectreMaskIndex()) {
    return SkipUninterestingInstructions(ins->toSpectreMaskIndex()->index());
  }

  return ins;
}

static bool DefinitelyDifferentValue(MDefinition* ins1, MDefinition* ins2) {
  ins1 = SkipUninterestingInstructions(ins1);
  ins2 = SkipUninterestingInstructions(ins2);

  if (ins1 == ins2) {
    return false;
  }

  // For constants check they are not equal.
  if (ins1->isConstant() && ins2->isConstant()) {
    MConstant* cst1 = ins1->toConstant();
    MConstant* cst2 = ins2->toConstant();

    if (!cst1->isTypeRepresentableAsDouble() ||
        !cst2->isTypeRepresentableAsDouble()) {
      return false;
    }

    // Be conservative and only allow values that fit into int32.
    int32_t n1, n2;
    if (!mozilla::NumberIsInt32(cst1->numberToDouble(), &n1) ||
        !mozilla::NumberIsInt32(cst2->numberToDouble(), &n2)) {
      return false;
    }

    return n1 != n2;
  }

  // Check if "ins1 = ins2 + cte", which would make both instructions
  // have different values.
  if (ins1->isAdd()) {
    if (AddIsANonZeroAdditionOf(ins1->toAdd(), ins2)) {
      return true;
    }
  }
  if (ins2->isAdd()) {
    if (AddIsANonZeroAdditionOf(ins2->toAdd(), ins1)) {
      return true;
    }
  }

  return false;
}

MDefinition::AliasType MLoadElement::mightAlias(const MDefinition* def) const {
  if (def->isStoreElement()) {
    const MStoreElement* store = def->toStoreElement();
    if (store->index() != index()) {
      if (DefinitelyDifferentValue(store->index(), index())) {
        return AliasType::NoAlias;
      }
      return AliasType::MayAlias;
    }

    if (store->elements() != elements()) {
      return AliasType::MayAlias;
    }

    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadElement::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

MDefinition* MWasmUnsignedToDouble::foldsTo(TempAllocator& alloc) {
  if (input()->isConstant()) {
    return MConstant::New(
        alloc, DoubleValue(uint32_t(input()->toConstant()->toInt32())));
  }

  return this;
}

MDefinition* MWasmUnsignedToFloat32::foldsTo(TempAllocator& alloc) {
  if (input()->isConstant()) {
    double dval = double(uint32_t(input()->toConstant()->toInt32()));
    if (IsFloat32Representable(dval)) {
      return MConstant::NewFloat32(alloc, float(dval));
    }
  }

  return this;
}

MWasmCall* MWasmCall::New(TempAllocator& alloc, const wasm::CallSiteDesc& desc,
                          const wasm::CalleeDesc& callee, const Args& args,
                          uint32_t stackArgAreaSizeUnaligned,
                          MDefinition* tableIndex) {
  MWasmCall* call =
      new (alloc) MWasmCall(desc, callee, stackArgAreaSizeUnaligned);

  if (!call->argRegs_.init(alloc, args.length())) {
    return nullptr;
  }
  for (size_t i = 0; i < call->argRegs_.length(); i++) {
    call->argRegs_[i] = args[i].reg;
  }

  if (!call->init(alloc,
                  call->argRegs_.length() + (callee.isTable() ? 1 : 0))) {
    return nullptr;
  }
  // FixedList doesn't initialize its elements, so do an unchecked init.
  for (size_t i = 0; i < call->argRegs_.length(); i++) {
    call->initOperand(i, args[i].def);
  }
  if (callee.isTable()) {
    call->initOperand(call->argRegs_.length(), tableIndex);
  }

  return call;
}

MWasmCall* MWasmCall::NewBuiltinInstanceMethodCall(
    TempAllocator& alloc, const wasm::CallSiteDesc& desc,
    const wasm::SymbolicAddress builtin, wasm::FailureMode failureMode,
    const ABIArg& instanceArg, const Args& args,
    uint32_t stackArgAreaSizeUnaligned) {
  auto callee = wasm::CalleeDesc::builtinInstanceMethod(builtin);
  MWasmCall* call = MWasmCall::New(alloc, desc, callee, args,
                                   stackArgAreaSizeUnaligned, nullptr);
  if (!call) {
    return nullptr;
  }

  MOZ_ASSERT(instanceArg != ABIArg());
  call->instanceArg_ = instanceArg;
  call->builtinMethodFailureMode_ = failureMode;
  return call;
}

void MSqrt::trySpecializeFloat32(TempAllocator& alloc) {
  if (!input()->canProduceFloat32() || !CheckUsesAreFloat32Consumers(this)) {
    if (input()->type() == MIRType::Float32) {
      ConvertDefinitionToDouble<0>(alloc, input(), this);
    }
    return;
  }

  setResultType(MIRType::Float32);
  specialization_ = MIRType::Float32;
}

MDefinition* MClz::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant()) {
    MConstant* c = num()->toConstant();
    if (type() == MIRType::Int32) {
      int32_t n = c->toInt32();
      if (n == 0) {
        return MConstant::New(alloc, Int32Value(32));
      }
      return MConstant::New(alloc,
                            Int32Value(mozilla::CountLeadingZeroes32(n)));
    }
    int64_t n = c->toInt64();
    if (n == 0) {
      return MConstant::NewInt64(alloc, int64_t(64));
    }
    return MConstant::NewInt64(alloc,
                               int64_t(mozilla::CountLeadingZeroes64(n)));
  }

  return this;
}

MDefinition* MCtz::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant()) {
    MConstant* c = num()->toConstant();
    if (type() == MIRType::Int32) {
      int32_t n = num()->toConstant()->toInt32();
      if (n == 0) {
        return MConstant::New(alloc, Int32Value(32));
      }
      return MConstant::New(alloc,
                            Int32Value(mozilla::CountTrailingZeroes32(n)));
    }
    int64_t n = c->toInt64();
    if (n == 0) {
      return MConstant::NewInt64(alloc, int64_t(64));
    }
    return MConstant::NewInt64(alloc,
                               int64_t(mozilla::CountTrailingZeroes64(n)));
  }

  return this;
}

MDefinition* MPopcnt::foldsTo(TempAllocator& alloc) {
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

MDefinition* MBoundsCheck::foldsTo(TempAllocator& alloc) {
  if (type() == MIRType::Int32 && index()->isConstant() &&
      length()->isConstant()) {
    uint32_t len = length()->toConstant()->toInt32();
    uint32_t idx = index()->toConstant()->toInt32();
    if (idx + uint32_t(minimum()) < len && idx + uint32_t(maximum()) < len) {
      return index();
    }
  }

  return this;
}

MDefinition* MTableSwitch::foldsTo(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  // If we only have one successor, convert to a plain goto to the only
  // successor. TableSwitch indices are numeric; other types will always go to
  // the only successor.
  if (numSuccessors() == 1 ||
      (op->type() != MIRType::Value && !IsNumberType(op->type()))) {
    return MGoto::New(alloc, getDefault());
  }

  if (MConstant* opConst = op->maybeConstantValue()) {
    if (op->type() == MIRType::Int32) {
      int32_t i = opConst->toInt32() - low_;
      MBasicBlock* target;
      if (size_t(i) < numCases()) {
        target = getCase(size_t(i));
      } else {
        target = getDefault();
      }
      MOZ_ASSERT(target);
      return MGoto::New(alloc, target);
    }
  }

  return this;
}

MDefinition* MArrayJoin::foldsTo(TempAllocator& alloc) {
  MDefinition* arr = array();

  if (!arr->isStringSplit()) {
    return this;
  }

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

  MStringReplace* substr =
      MStringReplace::New(alloc, string, pattern, replacement);
  substr->setFlatReplacement();
  return substr;
}

MDefinition* MGetFirstDollarIndex::foldsTo(TempAllocator& alloc) {
  MDefinition* strArg = str();
  if (!strArg->isConstant()) {
    return this;
  }

  JSAtom* atom = &strArg->toConstant()->toString()->asAtom();
  int32_t index = GetFirstDollarIndexRawFlat(atom);
  return MConstant::New(alloc, Int32Value(index));
}

AliasSet MThrowRuntimeLexicalError::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MSlots::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MInitializedLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSetInitializedLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MArrayLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSetArrayLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MFunctionLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MFunctionName::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MArrayBufferByteLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::FixedSlot);
}

AliasSet MArrayBufferViewLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset);
}

AliasSet MArrayBufferViewByteOffset::getAliasSet() const {
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset);
}

AliasSet MArrayBufferViewElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardHasAttachedArrayBuffer::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot);
}

AliasSet MArrayPush::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::Element);
}

MDefinition* MGuardNumberToIntPtrIndex::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();

  if (input->isToDouble() && input->getOperand(0)->type() == MIRType::Int32) {
    return MInt32ToIntPtr::New(alloc, input->getOperand(0));
  }

  if (!input->isConstant()) {
    return this;
  }

  // Fold constant double representable as intptr to intptr.
  int64_t ival;
  if (!mozilla::NumberEqualsInt64(input->toConstant()->toDouble(), &ival)) {
    // If not representable as an int64, this access is equal to an OOB access.
    // So replace it with a known int64/intptr value which also produces an OOB
    // access. If we don't support OOB accesses we have to bail out.
    if (!supportOOB()) {
      return this;
    }
    ival = -1;
  }

  if (ival < INTPTR_MIN || ival > INTPTR_MAX) {
    return this;
  }

  return MConstant::NewIntPtr(alloc, intptr_t(ival));
}

MDefinition* MIsObject::foldsTo(TempAllocator& alloc) {
  if (!object()->isBox()) {
    return this;
  }

  MDefinition* unboxed = object()->getOperand(0);
  if (unboxed->type() == MIRType::Object) {
    return MConstant::New(alloc, BooleanValue(true));
  }

  return this;
}

MDefinition* MIsNullOrUndefined::foldsTo(TempAllocator& alloc) {
  MDefinition* input = value();
  if (input->isBox()) {
    input = input->toBox()->input();
  }

  if (input->definitelyType({MIRType::Null, MIRType::Undefined})) {
    return MConstant::New(alloc, BooleanValue(true));
  }

  if (!input->mightBeType(MIRType::Null) &&
      !input->mightBeType(MIRType::Undefined)) {
    return MConstant::New(alloc, BooleanValue(false));
  }

  return this;
}

AliasSet MHomeObjectSuperBase::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

static MDefinition* SkipObjectGuards(MDefinition* ins) {
  // These instructions don't modify the object and just guard specific
  // properties.
  while (true) {
    if (ins->isGuardShape()) {
      ins = ins->toGuardShape()->object();
      continue;
    }
    if (ins->isGuardNullProto()) {
      ins = ins->toGuardNullProto()->object();
      continue;
    }
    if (ins->isGuardProto()) {
      ins = ins->toGuardProto()->object();
      continue;
    }

    break;
  }

  return ins;
}

MDefinition* MGuardShape::foldsTo(TempAllocator& alloc) {
  MDefinition* ins = dependency();
  if (!ins) {
    return this;
  }

  if (!ins->block()->dominates(block())) {
    return this;
  }

  auto AssertShape = [](TempAllocator& alloc, MGuardShape* ins) {
#ifdef DEBUG
    auto* assert = MAssertShape::New(alloc, ins->object(),
                                     const_cast<Shape*>(ins->shape()));
    ins->block()->insertBefore(ins, assert);
#endif
  };

  if (ins->isAddAndStoreSlot()) {
    auto* add = ins->toAddAndStoreSlot();

    if (SkipObjectGuards(add->object()) != SkipObjectGuards(object()) ||
        add->shape() != shape()) {
      return this;
    }

    AssertShape(alloc, this);
    return object();
  }

  if (ins->isAllocateAndStoreSlot()) {
    auto* allocate = ins->toAllocateAndStoreSlot();

    if (SkipObjectGuards(allocate->object()) != SkipObjectGuards(object()) ||
        allocate->shape() != shape()) {
      return this;
    }

    AssertShape(alloc, this);
    return object();
  }

  if (ins->isStart()) {
    // The guard doesn't depend on any other instruction that is modifying
    // the object operand, so check it directly.
    auto* obj = SkipObjectGuards(object());
    if (!obj->isNewObject()) {
      return this;
    }

    JSObject* templateObject = obj->toNewObject()->templateObject();
    if (!templateObject) {
      return this;
    }

    if (templateObject->shape() != shape()) {
      return this;
    }

    AssertShape(alloc, this);
    return object();
  }

  return this;
}

MDefinition* MGuardValue::foldsTo(TempAllocator& alloc) {
  if (MConstant* cst = value()->maybeConstantValue()) {
    if (cst->toJSValue() == expected()) {
      return value();
    }
  }

  return this;
}

MDefinition* MGuardNullOrUndefined::foldsTo(TempAllocator& alloc) {
  MDefinition* input = value();
  if (input->isBox()) {
    input = input->toBox()->input();
  }

  if (input->definitelyType({MIRType::Null, MIRType::Undefined})) {
    return value();
  }

  return this;
}

MDefinition* MGuardObjectIdentity::foldsTo(TempAllocator& alloc) {
  if (object()->isConstant() && expected()->isConstant()) {
    JSObject* obj = &object()->toConstant()->toObject();
    JSObject* other = &expected()->toConstant()->toObject();
    if (!bailOnEquality()) {
      if (obj == other) {
        return object();
      }
    } else {
      if (obj != other) {
        return object();
      }
    }
  }

  if (!bailOnEquality() && object()->isNurseryObject() &&
      expected()->isNurseryObject()) {
    uint32_t objIndex = object()->toNurseryObject()->nurseryIndex();
    uint32_t otherIndex = expected()->toNurseryObject()->nurseryIndex();
    if (objIndex == otherIndex) {
      return object();
    }
  }

  return this;
}

MDefinition* MGuardSpecificFunction::foldsTo(TempAllocator& alloc) {
  if (function()->isConstant() && expected()->isConstant()) {
    JSObject* fun = &function()->toConstant()->toObject();
    JSObject* other = &expected()->toConstant()->toObject();
    if (fun == other) {
      return function();
    }
  }

  if (function()->isNurseryObject() && expected()->isNurseryObject()) {
    uint32_t funIndex = function()->toNurseryObject()->nurseryIndex();
    uint32_t otherIndex = expected()->toNurseryObject()->nurseryIndex();
    if (funIndex == otherIndex) {
      return function();
    }
  }

  return this;
}

MDefinition* MGuardSpecificAtom::foldsTo(TempAllocator& alloc) {
  if (str()->isConstant()) {
    JSAtom* cstAtom = &str()->toConstant()->toString()->asAtom();
    if (cstAtom == atom()) {
      return str();
    }
  }

  return this;
}

MDefinition* MGuardSpecificSymbol::foldsTo(TempAllocator& alloc) {
  if (symbol()->isConstant()) {
    if (symbol()->toConstant()->toSymbol() == expected()) {
      return symbol();
    }
  }

  return this;
}

bool MCallBindVar::congruentTo(const MDefinition* ins) const {
  if (!ins->isCallBindVar()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

bool MGuardShape::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardShape()) {
    return false;
  }
  if (shape() != ins->toGuardShape()->shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardShape::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition* MGuardIsNotProxy::foldsTo(TempAllocator& alloc) {
  KnownClass known = GetObjectKnownClass(object());
  if (known == KnownClass::None) {
    return this;
  }

  MOZ_ASSERT(!GetObjectKnownJSClass(object())->isProxyObject());
  AssertKnownClass(alloc, this, object());
  return object();
}

AliasSet MMegamorphicLoadSlotByValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MMegamorphicLoadSlot::congruentTo(const MDefinition* ins) const {
  if (!ins->isMegamorphicLoadSlot()) {
    return false;
  }
  if (ins->toMegamorphicLoadSlot()->name() != name()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MMegamorphicLoadSlot::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MMegamorphicStoreSlot::congruentTo(const MDefinition* ins) const {
  if (!ins->isMegamorphicStoreSlot()) {
    return false;
  }
  if (ins->toMegamorphicStoreSlot()->name() != name()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MMegamorphicStoreSlot::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::FixedSlot |
                         AliasSet::DynamicSlot);
}

bool MMegamorphicHasProp::congruentTo(const MDefinition* ins) const {
  if (!ins->isMegamorphicHasProp()) {
    return false;
  }
  if (ins->toMegamorphicHasProp()->hasOwn() != hasOwn()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MMegamorphicHasProp::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MNurseryObject::congruentTo(const MDefinition* ins) const {
  if (!ins->isNurseryObject()) {
    return false;
  }
  return nurseryIndex() == ins->toNurseryObject()->nurseryIndex();
}

AliasSet MGuardFunctionIsNonBuiltinCtor::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardFunctionKind::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardFunctionKind()) {
    return false;
  }
  if (expected() != ins->toGuardFunctionKind()->expected()) {
    return false;
  }
  if (bailOnEquality() != ins->toGuardFunctionKind()->bailOnEquality()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardFunctionKind::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardFunctionScript::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardFunctionScript()) {
    return false;
  }
  if (expected() != ins->toGuardFunctionScript()->expected()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardFunctionScript::getAliasSet() const {
  // A JSFunction's BaseScript pointer is immutable. Relazification of
  // self-hosted functions is an exception to this, but we don't use this
  // guard for self-hosted functions.
  MOZ_ASSERT(!flags_.isSelfHostedOrIntrinsic());
  return AliasSet::None();
}

bool MGuardSpecificAtom::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardSpecificAtom()) {
    return false;
  }
  if (atom() != ins->toGuardSpecificAtom()->atom()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

MDefinition* MGuardStringToIndex::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSAtom* atom = &string()->toConstant()->toString()->asAtom();

  int32_t index = GetIndexFromString(atom);
  if (index < 0) {
    return this;
  }

  return MConstant::New(alloc, Int32Value(index));
}

MDefinition* MGuardStringToInt32::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSAtom* atom = &string()->toConstant()->toString()->asAtom();
  double number;
  if (!js::MaybeStringToNumber(atom, &number)) {
    return this;
  }

  int32_t n;
  if (!mozilla::NumberIsInt32(number, &n)) {
    return this;
  }

  return MConstant::New(alloc, Int32Value(n));
}

MDefinition* MGuardStringToDouble::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSAtom* atom = &string()->toConstant()->toString()->asAtom();
  double number;
  if (!js::MaybeStringToNumber(atom, &number)) {
    return this;
  }

  return MConstant::New(alloc, DoubleValue(number));
}

AliasSet MGuardNoDenseElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MCopyLexicalEnvironmentObject::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MAllocateAndStoreSlot::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::DynamicSlot);
}

AliasSet MLoadDOMExpandoValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::DOMProxyExpando);
}

AliasSet MLoadDOMExpandoValueIgnoreGeneration::getAliasSet() const {
  return AliasSet::Load(AliasSet::DOMProxyExpando);
}

bool MGuardDOMExpandoMissingOrGuardShape::congruentTo(
    const MDefinition* ins) const {
  if (!ins->isGuardDOMExpandoMissingOrGuardShape()) {
    return false;
  }
  if (shape() != ins->toGuardDOMExpandoMissingOrGuardShape()->shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardDOMExpandoMissingOrGuardShape::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition* MGuardToClass::foldsTo(TempAllocator& alloc) {
  const JSClass* clasp = GetObjectKnownJSClass(object());
  if (!clasp || getClass() != clasp) {
    return this;
  }

  AssertKnownClass(alloc, this, object());
  return object();
}

MDefinition* MHasClass::foldsTo(TempAllocator& alloc) {
  const JSClass* clasp = GetObjectKnownJSClass(object());
  if (!clasp) {
    return this;
  }

  AssertKnownClass(alloc, this, object());
  return MConstant::New(alloc, BooleanValue(getClass() == clasp));
}

MDefinition* MIsCallable::foldsTo(TempAllocator& alloc) {
  if (input()->type() != MIRType::Object) {
    return this;
  }

  KnownClass known = GetObjectKnownClass(input());
  if (known == KnownClass::None) {
    return this;
  }

  AssertKnownClass(alloc, this, input());
  return MConstant::New(alloc, BooleanValue(known == KnownClass::Function));
}

MDefinition* MIsArray::foldsTo(TempAllocator& alloc) {
  if (input()->type() != MIRType::Object) {
    return this;
  }

  KnownClass known = GetObjectKnownClass(input());
  if (known == KnownClass::None) {
    return this;
  }

  AssertKnownClass(alloc, this, input());
  return MConstant::New(alloc, BooleanValue(known == KnownClass::Array));
}

AliasSet MObjectClassToString::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

MDefinition* MGuardIsNotArrayBufferMaybeShared::foldsTo(TempAllocator& alloc) {
  switch (GetObjectKnownClass(object())) {
    case KnownClass::PlainObject:
    case KnownClass::Array:
    case KnownClass::Function:
    case KnownClass::RegExp:
    case KnownClass::ArrayIterator:
    case KnownClass::StringIterator:
    case KnownClass::RegExpStringIterator: {
      AssertKnownClass(alloc, this, object());
      return object();
    }
    case KnownClass::None:
      break;
  }

  return this;
}

MDefinition* MCheckIsObj::foldsTo(TempAllocator& alloc) {
  if (!input()->isBox()) {
    return this;
  }

  MDefinition* unboxed = input()->getOperand(0);
  if (unboxed->type() == MIRType::Object) {
    return unboxed;
  }

  return this;
}

MDefinition* MCheckThis::foldsTo(TempAllocator& alloc) {
  MDefinition* input = thisValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->getOperand(0);
  if (unboxed->mightBeMagicType()) {
    return this;
  }

  return input;
}

MDefinition* MCheckThisReinit::foldsTo(TempAllocator& alloc) {
  MDefinition* input = thisValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->getOperand(0);
  if (unboxed->type() != MIRType::MagicUninitializedLexical) {
    return this;
  }

  return input;
}

MDefinition* MCheckObjCoercible::foldsTo(TempAllocator& alloc) {
  MDefinition* input = checkValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->getOperand(0);
  if (unboxed->mightBeType(MIRType::Null) ||
      unboxed->mightBeType(MIRType::Undefined)) {
    return this;
  }

  return input;
}

AliasSet MCheckObjCoercible::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MIsPackedArray::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardArrayIsPacked::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSuperFunction::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MInitHomeObject::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MLoadWrapperTarget::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MGuardHasGetterSetter::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardHasGetterSetter::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardHasGetterSetter()) {
    return false;
  }
  if (ins->toGuardHasGetterSetter()->propId() != propId()) {
    return false;
  }
  if (ins->toGuardHasGetterSetter()->getterSetter() != getterSetter()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardIsExtensible::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardIndexGreaterThanDenseInitLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardIndexIsValidUpdateOrAdd::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MCallObjectHasSparseElement::getAliasSet() const {
  return AliasSet::Load(AliasSet::Element | AliasSet::ObjectFields |
                        AliasSet::FixedSlot | AliasSet::DynamicSlot);
}

MDefinition* MGuardInt32IsNonNegative::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(index()->type() == MIRType::Int32);

  MDefinition* input = index();
  if (!input->isConstant() || input->toConstant()->toInt32() < 0) {
    return this;
  }
  return input;
}

MIonToWasmCall* MIonToWasmCall::New(TempAllocator& alloc,
                                    WasmInstanceObject* instanceObj,
                                    const wasm::FuncExport& funcExport) {
  const wasm::ValTypeVector& results = funcExport.funcType().results();
  MIRType resultType = MIRType::Value;
  // At the JS boundary some wasm types must be represented as a Value, and in
  // addition a void return requires an Undefined value.
  if (results.length() > 0 && !results[0].isEncodedAsJSValueOnEscape()) {
    MOZ_ASSERT(results.length() == 1,
               "multiple returns not implemented for inlined Wasm calls");
    resultType = ToMIRType(results[0]);
  }

  auto* ins = new (alloc) MIonToWasmCall(instanceObj, resultType, funcExport);
  if (!ins->init(alloc, funcExport.funcType().args().length())) {
    return nullptr;
  }
  return ins;
}

#ifdef DEBUG
bool MIonToWasmCall::isConsistentFloat32Use(MUse* use) const {
  return funcExport_.funcType().args()[use->index()].kind() ==
         wasm::ValType::F32;
}
#endif

MCreateInlinedArgumentsObject* MCreateInlinedArgumentsObject::New(
    TempAllocator& alloc, MDefinition* callObj, MDefinition* callee,
    MDefinitionVector& args) {
  MCreateInlinedArgumentsObject* ins =
      new (alloc) MCreateInlinedArgumentsObject();

  uint32_t argc = args.length();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, callObj);
  ins->initOperand(1, callee);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args[i]);
  }

  return ins;
}

MGetInlinedArgument* MGetInlinedArgument::New(
    TempAllocator& alloc, MDefinition* index,
    MCreateInlinedArgumentsObject* args) {
  MGetInlinedArgument* ins = new (alloc) MGetInlinedArgument();

  uint32_t argc = args->numActuals();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, index);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args->getArg(i));
  }

  return ins;
}

MDefinition* MGetInlinedArgument::foldsTo(TempAllocator& alloc) {
  MDefinition* indexDef = SkipUninterestingInstructions(index());
  if (!indexDef->isConstant() || indexDef->type() != MIRType::Int32) {
    return this;
  }

  int32_t indexConst = indexDef->toConstant()->toInt32();
  if (indexConst < 0 || uint32_t(indexConst) >= numActuals()) {
    return this;
  }

  MDefinition* arg = getArg(indexConst);
  if (arg->type() != MIRType::Value) {
    arg = MBox::New(alloc, arg);
  }

  return arg;
}

bool MWasmShiftSimd128::congruentTo(const MDefinition* ins) const {
  return ins->toWasmShiftSimd128()->simdOp() == simdOp_ &&
         congruentIfOperandsEqual(ins);
}

bool MWasmShuffleSimd128::congruentTo(const MDefinition* ins) const {
  return ins->toWasmShuffleSimd128()->control().bitwiseEqual(control_) &&
         congruentIfOperandsEqual(ins);
}

bool MWasmUnarySimd128::congruentTo(const MDefinition* ins) const {
  return ins->toWasmUnarySimd128()->simdOp() == simdOp_ &&
         congruentIfOperandsEqual(ins);
}
