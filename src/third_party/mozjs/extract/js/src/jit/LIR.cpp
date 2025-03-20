/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/LIR.h"

#include "mozilla/ScopeExit.h"

#include <type_traits>

#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "js/Printf.h"
#include "util/Unicode.h"

using namespace js;
using namespace js::jit;

const char* const js::jit::LIROpNames[] = {
#define OPNAME(op, ...) #op,
    LIR_OPCODE_LIST(OPNAME)
#undef OPNAME
};

LIRGraph::LIRGraph(MIRGraph* mir)
    : constantPool_(mir->alloc()),
      constantPoolMap_(mir->alloc()),
      safepoints_(mir->alloc()),
      nonCallSafepoints_(mir->alloc()),
      numVirtualRegisters_(0),
      numInstructions_(1),  // First id is 1.
      localSlotsSize_(0),
      argumentSlotCount_(0),
      mir_(*mir) {}

bool LIRGraph::addConstantToPool(const Value& v, uint32_t* index) {
  ConstantPoolMap::AddPtr p = constantPoolMap_.lookupForAdd(v);
  if (p) {
    *index = p->value();
    return true;
  }
  *index = constantPool_.length();
  return constantPool_.append(v) && constantPoolMap_.add(p, v, *index);
}

bool LIRGraph::noteNeedsSafepoint(LInstruction* ins) {
  // Instructions with safepoints must be in linear order.
  MOZ_ASSERT_IF(!safepoints_.empty(), safepoints_.back()->id() < ins->id());
  if (!ins->isCall() && !nonCallSafepoints_.append(ins)) {
    return false;
  }
  return safepoints_.append(ins);
}

#ifdef JS_JITSPEW
void LIRGraph::dump(GenericPrinter& out) {
  for (size_t i = 0; i < numBlocks(); i++) {
    getBlock(i)->dump(out);
    out.printf("\n");
  }
}

void LIRGraph::dump() {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}
#endif

LBlock::LBlock(MBasicBlock* from)
    : block_(from), entryMoveGroup_(nullptr), exitMoveGroup_(nullptr) {
  from->assignLir(this);
}

bool LBlock::init(TempAllocator& alloc) {
  // Count the number of LPhis we'll need.
  size_t numLPhis = 0;
  for (MPhiIterator i(block_->phisBegin()), e(block_->phisEnd()); i != e; ++i) {
    MPhi* phi = *i;
    switch (phi->type()) {
      case MIRType::Value:
        numLPhis += BOX_PIECES;
        break;
      case MIRType::Int64:
        numLPhis += INT64_PIECES;
        break;
      default:
        numLPhis += 1;
        break;
    }
  }

  // Allocate space for the LPhis.
  if (!phis_.init(alloc, numLPhis)) {
    return false;
  }

  // For each MIR phi, set up LIR phis as appropriate. We'll fill in their
  // operands on each incoming edge, and set their definitions at the start of
  // their defining block.
  size_t phiIndex = 0;
  size_t numPreds = block_->numPredecessors();
  for (MPhiIterator i(block_->phisBegin()), e(block_->phisEnd()); i != e; ++i) {
    MPhi* phi = *i;
    MOZ_ASSERT(phi->numOperands() == numPreds);

    int numPhis;
    switch (phi->type()) {
      case MIRType::Value:
        numPhis = BOX_PIECES;
        break;
      case MIRType::Int64:
        numPhis = INT64_PIECES;
        break;
      default:
        numPhis = 1;
        break;
    }
    for (int i = 0; i < numPhis; i++) {
      LAllocation* inputs = alloc.allocateArray<LAllocation>(numPreds);
      if (!inputs) {
        return false;
      }

      void* addr = &phis_[phiIndex++];
      LPhi* lphi = new (addr) LPhi(phi, inputs);
      lphi->setBlock(this);
    }
  }
  return true;
}

const LInstruction* LBlock::firstInstructionWithId() const {
  for (LInstructionIterator i(instructions_.begin()); i != instructions_.end();
       ++i) {
    if (i->id()) {
      return *i;
    }
  }
  return 0;
}

LMoveGroup* LBlock::getEntryMoveGroup(TempAllocator& alloc) {
  if (entryMoveGroup_) {
    return entryMoveGroup_;
  }
  entryMoveGroup_ = LMoveGroup::New(alloc);
  insertBefore(*begin(), entryMoveGroup_);
  return entryMoveGroup_;
}

LMoveGroup* LBlock::getExitMoveGroup(TempAllocator& alloc) {
  if (exitMoveGroup_) {
    return exitMoveGroup_;
  }
  exitMoveGroup_ = LMoveGroup::New(alloc);
  insertBefore(*rbegin(), exitMoveGroup_);
  return exitMoveGroup_;
}

#ifdef JS_JITSPEW
void LBlock::dump(GenericPrinter& out) {
  out.printf("block%u:\n", mir()->id());
  for (size_t i = 0; i < numPhis(); ++i) {
    getPhi(i)->dump(out);
    out.printf("\n");
  }
  for (LInstructionIterator iter = begin(); iter != end(); iter++) {
    iter->dump(out);
    if (iter->safepoint()) {
      out.printf(" SAFEPOINT(0x%p) ", iter->safepoint());
    }
    out.printf("\n");
  }
}

void LBlock::dump() {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}
#endif

static size_t TotalOperandCount(LRecoverInfo* recoverInfo) {
  size_t accum = 0;
  for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
    if (!it->isRecoveredOnBailout()) {
      accum++;
    }
  }
  return accum;
}

LRecoverInfo::LRecoverInfo(TempAllocator& alloc)
    : instructions_(alloc), recoverOffset_(INVALID_RECOVER_OFFSET) {}

LRecoverInfo* LRecoverInfo::New(MIRGenerator* gen, MResumePoint* mir) {
  LRecoverInfo* recoverInfo = new (gen->alloc()) LRecoverInfo(gen->alloc());
  if (!recoverInfo || !recoverInfo->init(mir)) {
    return nullptr;
  }

  JitSpew(JitSpew_IonSnapshots, "Generating LIR recover info %p from MIR (%p)",
          (void*)recoverInfo, (void*)mir);

  return recoverInfo;
}

// de-virtualise MResumePoint::getOperand calls.
template <typename Node>
bool LRecoverInfo::appendOperands(Node* ins) {
  for (size_t i = 0, end = ins->numOperands(); i < end; i++) {
    MDefinition* def = ins->getOperand(i);

    // As there is no cycle in the data-flow (without MPhi), checking for
    // isInWorkList implies that the definition is already in the
    // instruction vector, and not processed by a caller of the current
    // function.
    if (def->isRecoveredOnBailout() && !def->isInWorklist()) {
      if (!appendDefinition(def)) {
        return false;
      }
    }
  }

  return true;
}

bool LRecoverInfo::appendDefinition(MDefinition* def) {
  MOZ_ASSERT(def->isRecoveredOnBailout());
  def->setInWorklist();
  auto clearWorklistFlagOnFailure =
      mozilla::MakeScopeExit([&] { def->setNotInWorklist(); });

  if (!appendOperands(def)) {
    return false;
  }

  if (!instructions_.append(def)) {
    return false;
  }

  clearWorklistFlagOnFailure.release();
  return true;
}

bool LRecoverInfo::appendResumePoint(MResumePoint* rp) {
  // Stores should be recovered first.
  if (!rp->storesEmpty()) {
    hasSideEffects_ = true;
  }
  for (auto iter(rp->storesBegin()), end(rp->storesEnd()); iter != end;
       ++iter) {
    if (!appendDefinition(iter->operand)) {
      return false;
    }
  }

  if (rp->caller() && !appendResumePoint(rp->caller())) {
    return false;
  }

  if (!appendOperands(rp)) {
    return false;
  }

  return instructions_.append(rp);
}

bool LRecoverInfo::init(MResumePoint* rp) {
  // Before exiting this function, remove temporary flags from all definitions
  // added in the vector.
  auto clearWorklistFlags = mozilla::MakeScopeExit([&] {
    for (MNode** it = begin(); it != end(); it++) {
      if (!(*it)->isDefinition()) {
        continue;
      }
      (*it)->toDefinition()->setNotInWorklist();
    }
  });

  // Sort operations in the order in which we need to restore the stack. This
  // implies that outer frames, as well as operations needed to recover the
  // current frame, are located before the current frame. The inner-most
  // resume point should be the last element in the list.
  if (!appendResumePoint(rp)) {
    return false;
  }

  MOZ_ASSERT(mir() == rp);
  return true;
}

LSnapshot::LSnapshot(LRecoverInfo* recoverInfo, BailoutKind kind)
    : slots_(nullptr),
      recoverInfo_(recoverInfo),
      snapshotOffset_(INVALID_SNAPSHOT_OFFSET),
      numSlots_(TotalOperandCount(recoverInfo) * BOX_PIECES),
      bailoutKind_(kind) {}

bool LSnapshot::init(MIRGenerator* gen) {
  slots_ = gen->allocate<LAllocation>(numSlots_);
  return !!slots_;
}

LSnapshot* LSnapshot::New(MIRGenerator* gen, LRecoverInfo* recover,
                          BailoutKind kind) {
  LSnapshot* snapshot = new (gen->alloc()) LSnapshot(recover, kind);
  if (!snapshot || !snapshot->init(gen)) {
    return nullptr;
  }

  JitSpew(JitSpew_IonSnapshots, "Generating LIR snapshot %p from recover (%p)",
          (void*)snapshot, (void*)recover);

  return snapshot;
}

void LSnapshot::rewriteRecoveredInput(LUse input) {
  // Mark any operands to this snapshot with the same value as input as being
  // equal to the instruction's result.
  for (size_t i = 0; i < numEntries(); i++) {
    if (getEntry(i)->isUse() &&
        getEntry(i)->toUse()->virtualRegister() == input.virtualRegister()) {
      setEntry(i, LUse(input.virtualRegister(), LUse::RECOVERED_INPUT));
    }
  }
}

#ifdef JS_JITSPEW
void LNode::printName(GenericPrinter& out, Opcode op) {
  static const char* const names[] = {
#  define LIROP(x) #x,
      LIR_OPCODE_LIST(LIROP)
#  undef LIROP
  };
  const char* name = names[uint32_t(op)];
  size_t len = strlen(name);
  for (size_t i = 0; i < len; i++) {
    out.printf("%c", unicode::ToLowerCase(name[i]));
  }
}

void LNode::printName(GenericPrinter& out) { printName(out, op()); }
#endif

bool LAllocation::aliases(const LAllocation& other) const {
  if (isFloatReg() && other.isFloatReg()) {
    return toFloatReg()->reg().aliases(other.toFloatReg()->reg());
  }
  return *this == other;
}

#ifdef JS_JITSPEW
static const char* DefTypeName(LDefinition::Type type) {
  switch (type) {
    case LDefinition::GENERAL:
      return "g";
    case LDefinition::INT32:
      return "i";
    case LDefinition::OBJECT:
      return "o";
    case LDefinition::SLOTS:
      return "s";
    case LDefinition::WASM_ANYREF:
      return "wr";
    case LDefinition::FLOAT32:
      return "f";
    case LDefinition::DOUBLE:
      return "d";
    case LDefinition::SIMD128:
      return "simd128";
    case LDefinition::STACKRESULTS:
      return "stackresults";
#  ifdef JS_NUNBOX32
    case LDefinition::TYPE:
      return "t";
    case LDefinition::PAYLOAD:
      return "p";
#  else
    case LDefinition::BOX:
      return "x";
#  endif
  }
  MOZ_CRASH("Invalid type");
}

UniqueChars LDefinition::toString() const {
  AutoEnterOOMUnsafeRegion oomUnsafe;

  UniqueChars buf;
  if (isBogusTemp()) {
    buf = JS_smprintf("bogus");
  } else {
    buf = JS_smprintf("v%u<%s>", virtualRegister(), DefTypeName(type()));
    if (buf) {
      if (policy() == LDefinition::FIXED) {
        buf = JS_sprintf_append(std::move(buf), ":%s",
                                output()->toString().get());
      } else if (policy() == LDefinition::MUST_REUSE_INPUT) {
        buf = JS_sprintf_append(std::move(buf), ":tied(%u)", getReusedInput());
      }
    }
  }

  if (!buf) {
    oomUnsafe.crash("LDefinition::toString()");
  }

  return buf;
}

static UniqueChars PrintUse(const LUse* use) {
  switch (use->policy()) {
    case LUse::REGISTER:
      return JS_smprintf("v%u:R", use->virtualRegister());
    case LUse::FIXED:
      return JS_smprintf("v%u:F:%s", use->virtualRegister(),
                         AnyRegister::FromCode(use->registerCode()).name());
    case LUse::ANY:
      return JS_smprintf("v%u:A", use->virtualRegister());
    case LUse::KEEPALIVE:
      return JS_smprintf("v%u:KA", use->virtualRegister());
    case LUse::STACK:
      return JS_smprintf("v%u:S", use->virtualRegister());
    case LUse::RECOVERED_INPUT:
      return JS_smprintf("v%u:RI", use->virtualRegister());
    default:
      MOZ_CRASH("invalid use policy");
  }
}

UniqueChars LAllocation::toString() const {
  AutoEnterOOMUnsafeRegion oomUnsafe;

  UniqueChars buf;
  if (isBogus()) {
    buf = JS_smprintf("bogus");
  } else {
    switch (kind()) {
      case LAllocation::CONSTANT_VALUE:
      case LAllocation::CONSTANT_INDEX: {
        const MConstant* c = toConstant();
        switch (c->type()) {
          case MIRType::Int32:
            buf = JS_smprintf("%d", c->toInt32());
            break;
          case MIRType::Int64:
            buf = JS_smprintf("%" PRId64, c->toInt64());
            break;
          case MIRType::IntPtr:
            buf = JS_smprintf("%" PRIxPTR, c->toIntPtr());
            break;
          case MIRType::String:
            // If a JSContext is a available, output the actual string
            if (JSContext* cx = TlsContext.get()) {
              Sprinter spr(cx);
              if (!spr.init()) {
                oomUnsafe.crash("LAllocation::toString()");
              }
              spr.putString(cx, c->toString());
              buf = spr.release();
            } else {
              buf = JS_smprintf("string");
            }
            break;
          case MIRType::Symbol:
            buf = JS_smprintf("sym");
            break;
          case MIRType::Object:
          case MIRType::Null:
            buf = JS_smprintf("obj %p", c->toObjectOrNull());
            break;
          case MIRType::Shape:
            buf = JS_smprintf("shape");
            break;
          default:
            if (c->isTypeRepresentableAsDouble()) {
              buf = JS_smprintf("%g", c->numberToDouble());
            } else {
              buf = JS_smprintf("const");
            }
        }
      } break;
      case LAllocation::GPR:
        buf = JS_smprintf("%s", toGeneralReg()->reg().name());
        break;
      case LAllocation::FPU:
        buf = JS_smprintf("%s", toFloatReg()->reg().name());
        break;
      case LAllocation::STACK_SLOT:
        buf = JS_smprintf("stack:%u", toStackSlot()->slot());
        break;
      case LAllocation::ARGUMENT_SLOT:
        buf = JS_smprintf("arg:%u", toArgument()->index());
        break;
      case LAllocation::STACK_AREA:
        buf = JS_smprintf("stackarea:%u+%u", toStackArea()->base(),
                          toStackArea()->size());
        break;
      case LAllocation::USE:
        buf = PrintUse(toUse());
        break;
      default:
        MOZ_CRASH("what?");
    }
  }

  if (!buf) {
    oomUnsafe.crash("LAllocation::toString()");
  }

  return buf;
}

void LAllocation::dump() const { fprintf(stderr, "%s\n", toString().get()); }

void LDefinition::dump() const { fprintf(stderr, "%s\n", toString().get()); }

template <typename T>
static void PrintOperands(GenericPrinter& out, T* node) {
  size_t numOperands = node->numOperands();

  for (size_t i = 0; i < numOperands; i++) {
    out.printf(" (%s)", node->getOperand(i)->toString().get());
    if (i != numOperands - 1) {
      out.printf(",");
    }
  }
}

void LNode::printOperands(GenericPrinter& out) {
  if (isMoveGroup()) {
    toMoveGroup()->printOperands(out);
    return;
  }
  if (isInteger()) {
    out.printf(" (%d)", toInteger()->i32());
    return;
  }
  if (isInteger64()) {
    out.printf(" (%" PRId64 ")", toInteger64()->i64());
    return;
  }

  if (isPhi()) {
    PrintOperands(out, toPhi());
  } else {
    PrintOperands(out, toInstruction());
  }
}
#endif

void LInstruction::assignSnapshot(LSnapshot* snapshot) {
  MOZ_ASSERT(!snapshot_);
  snapshot_ = snapshot;

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_IonSnapshots)) {
    JitSpewHeader(JitSpew_IonSnapshots);
    Fprinter& out = JitSpewPrinter();
    out.printf("Assigning snapshot %p to instruction %p (", (void*)snapshot,
               (void*)this);
    printName(out);
    out.printf(")\n");
  }
#endif
}

#ifdef JS_JITSPEW
static size_t NumSuccessorsHelper(const LNode* ins) { return 0; }

template <size_t Succs, size_t Operands, size_t Temps>
static size_t NumSuccessorsHelper(
    const LControlInstructionHelper<Succs, Operands, Temps>* ins) {
  return Succs;
}

static size_t NumSuccessors(const LInstruction* ins) {
  switch (ins->op()) {
    default:
      MOZ_CRASH("Unexpected LIR op");
#  define LIROP(x)         \
    case LNode::Opcode::x: \
      return NumSuccessorsHelper(ins->to##x());
      LIR_OPCODE_LIST(LIROP)
#  undef LIROP
  }
}

static MBasicBlock* GetSuccessorHelper(const LNode* ins, size_t i) {
  MOZ_CRASH("Unexpected instruction with successors");
}

template <size_t Succs, size_t Operands, size_t Temps>
static MBasicBlock* GetSuccessorHelper(
    const LControlInstructionHelper<Succs, Operands, Temps>* ins, size_t i) {
  return ins->getSuccessor(i);
}

static MBasicBlock* GetSuccessor(const LInstruction* ins, size_t i) {
  MOZ_ASSERT(i < NumSuccessors(ins));

  switch (ins->op()) {
    default:
      MOZ_CRASH("Unexpected LIR op");
#  define LIROP(x)         \
    case LNode::Opcode::x: \
      return GetSuccessorHelper(ins->to##x(), i);
      LIR_OPCODE_LIST(LIROP)
#  undef LIROP
  }
}
#endif

#ifdef JS_JITSPEW
void LNode::dump(GenericPrinter& out) {
  if (numDefs() != 0) {
    out.printf("{");
    for (size_t i = 0; i < numDefs(); i++) {
      const LDefinition* def =
          isPhi() ? toPhi()->getDef(i) : toInstruction()->getDef(i);
      out.printf("%s", def->toString().get());
      if (i != numDefs() - 1) {
        out.printf(", ");
      }
    }
    out.printf("} <- ");
  }

  printName(out);
  printOperands(out);

  if (isInstruction()) {
    LInstruction* ins = toInstruction();
    size_t numTemps = ins->numTemps();
    if (numTemps > 0) {
      out.printf(" t=(");
      for (size_t i = 0; i < numTemps; i++) {
        out.printf("%s", ins->getTemp(i)->toString().get());
        if (i != numTemps - 1) {
          out.printf(", ");
        }
      }
      out.printf(")");
    }

    size_t numSuccessors = NumSuccessors(ins);
    if (numSuccessors > 0) {
      out.printf(" s=(");
      for (size_t i = 0; i < numSuccessors; i++) {
        MBasicBlock* succ = GetSuccessor(ins, i);
        out.printf("block%u", succ->id());
        if (i != numSuccessors - 1) {
          out.printf(", ");
        }
      }
      out.printf(")");
    }
  }
}

void LNode::dump() {
  Fprinter out(stderr);
  dump(out);
  out.printf("\n");
  out.finish();
}

const char* LNode::getExtraName() const {
  switch (op()) {
    default:
      MOZ_CRASH("Unexpected LIR op");
#  define LIROP(x)         \
    case LNode::Opcode::x: \
      return to##x()->extraName();
      LIR_OPCODE_LIST(LIROP)
#  undef LIROP
  }
}
#endif

void LInstruction::initSafepoint(TempAllocator& alloc) {
  MOZ_ASSERT(!safepoint_);
  safepoint_ = new (alloc) LSafepoint(alloc);
  MOZ_ASSERT(safepoint_);
}

bool LMoveGroup::add(LAllocation from, LAllocation to, LDefinition::Type type) {
#ifdef DEBUG
  MOZ_ASSERT(from != to);
  for (size_t i = 0; i < moves_.length(); i++) {
    MOZ_ASSERT(to != moves_[i].to());
  }

  // Check that SIMD moves are aligned according to ABI requirements.
  // clang-format off
# ifdef ENABLE_WASM_SIMD
    // Alignment is not currently required for SIMD on x86/x64/arm64.  See also
    // CodeGeneratorShared::CodeGeneratorShared and in general everywhere
    // SimdMemoryAignment is used.  Likely, alignment requirements will return.
#   if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || \
       defined(JS_CODEGEN_ARM64)
      // No need for any check on x86/x64/arm64.
#   else
#     error "Need to consider SIMD alignment on this target."
      // The following code may be of use if we need alignment checks on
      // some future target.
      //if (LDefinition(type).type() == LDefinition::SIMD128) {
      //  MOZ_ASSERT(from.isMemory() || from.isFloatReg());
      //  if (from.isMemory()) {
      //    if (from.isArgument()) {
      //      MOZ_ASSERT(from.toArgument()->index() % SimdMemoryAlignment == 0);
      //    } else {
      //      MOZ_ASSERT(from.toStackSlot()->slot() % SimdMemoryAlignment == 0);
      //    }
      //  }
      //  MOZ_ASSERT(to.isMemory() || to.isFloatReg());
      //  if (to.isMemory()) {
      //    if (to.isArgument()) {
      //      MOZ_ASSERT(to.toArgument()->index() % SimdMemoryAlignment == 0);
      //    } else {
      //      MOZ_ASSERT(to.toStackSlot()->slot() % SimdMemoryAlignment == 0);
      //    }
      //  }
      //}
#   endif
# endif
  // clang-format on

#endif
  return moves_.append(LMove(from, to, type));
}

bool LMoveGroup::addAfter(LAllocation from, LAllocation to,
                          LDefinition::Type type) {
  // Transform the operands to this move so that performing the result
  // simultaneously with existing moves in the group will have the same
  // effect as if the original move took place after the existing moves.

  for (size_t i = 0; i < moves_.length(); i++) {
    if (moves_[i].to() == from) {
      from = moves_[i].from();
      break;
    }
  }

  if (from == to) {
    return true;
  }

  for (size_t i = 0; i < moves_.length(); i++) {
    if (to == moves_[i].to()) {
      moves_[i] = LMove(from, to, type);
      return true;
    }
  }

  return add(from, to, type);
}

#ifdef JS_JITSPEW
void LMoveGroup::printOperands(GenericPrinter& out) {
  for (size_t i = 0; i < numMoves(); i++) {
    const LMove& move = getMove(i);
    out.printf(" [%s -> %s", move.from().toString().get(),
               move.to().toString().get());
    out.printf(", %s", DefTypeName(move.type()));
    out.printf("]");
    if (i != numMoves() - 1) {
      out.printf(",");
    }
  }
}
#endif

#define LIROP(x)                              \
  static_assert(!std::is_polymorphic_v<L##x>, \
                "LIR instructions should not have virtual methods");
LIR_OPCODE_LIST(LIROP)
#undef LIROP
