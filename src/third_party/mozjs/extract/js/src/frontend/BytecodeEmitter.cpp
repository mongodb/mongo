/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS bytecode generation.
 */

#include "frontend/BytecodeEmitter.h"

#include "mozilla/Casting.h"    // mozilla::AssertedCast
#include "mozilla/DebugOnly.h"  // mozilla::DebugOnly
#include "mozilla/FloatingPoint.h"  // mozilla::NumberEqualsInt32, mozilla::NumberIsInt32
#include "mozilla/HashTable.h"      // mozilla::HashSet
#include "mozilla/Maybe.h"          // mozilla::{Maybe,Nothing,Some}
#include "mozilla/PodOperations.h"  // mozilla::PodCopy
#include "mozilla/Saturate.h"
#include "mozilla/Variant.h"  // mozilla::AsVariant

#include <algorithm>
#include <iterator>
#include <string.h>

#include "jstypes.h"  // JS_BIT

#include "frontend/AbstractScopePtr.h"           // ScopeIndex
#include "frontend/BytecodeControlStructures.h"  // NestableControl, BreakableControl, LabelControl, LoopControl, TryFinallyControl
#include "frontend/CallOrNewEmitter.h"           // CallOrNewEmitter
#include "frontend/CForEmitter.h"                // CForEmitter
#include "frontend/DecoratorEmitter.h"           // DecoratorEmitter
#include "frontend/DefaultEmitter.h"             // DefaultEmitter
#include "frontend/DoWhileEmitter.h"             // DoWhileEmitter
#include "frontend/ElemOpEmitter.h"              // ElemOpEmitter
#include "frontend/EmitterScope.h"               // EmitterScope
#include "frontend/ExpressionStatementEmitter.h"  // ExpressionStatementEmitter
#include "frontend/ForInEmitter.h"                // ForInEmitter
#include "frontend/ForOfEmitter.h"                // ForOfEmitter
#include "frontend/FunctionEmitter.h"  // FunctionEmitter, FunctionScriptEmitter, FunctionParamsEmitter
#include "frontend/IfEmitter.h"     // IfEmitter, InternalIfEmitter, CondEmitter
#include "frontend/LabelEmitter.h"  // LabelEmitter
#include "frontend/LexicalScopeEmitter.h"  // LexicalScopeEmitter
#include "frontend/ModuleSharedContext.h"  // ModuleSharedContext
#include "frontend/NameAnalysisTypes.h"    // PrivateNameKind
#include "frontend/NameFunctions.h"        // NameFunctions
#include "frontend/NameOpEmitter.h"        // NameOpEmitter
#include "frontend/ObjectEmitter.h"  // PropertyEmitter, ObjectEmitter, ClassEmitter
#include "frontend/OptionalEmitter.h"  // OptionalEmitter
#include "frontend/ParseNode.h"   // ParseNodeKind, ParseNode and subclasses
#include "frontend/Parser.h"      // Parser
#include "frontend/ParserAtom.h"  // ParserAtomsTable, ParserAtom
#include "frontend/PrivateOpEmitter.h"  // PrivateOpEmitter
#include "frontend/PropOpEmitter.h"     // PropOpEmitter
#include "frontend/SourceNotes.h"       // SrcNote, SrcNoteType, SrcNoteWriter
#include "frontend/SwitchEmitter.h"     // SwitchEmitter
#include "frontend/TaggedParserAtomIndexHasher.h"  // TaggedParserAtomIndexHasher
#include "frontend/TDZCheckCache.h"                // TDZCheckCache
#include "frontend/TryEmitter.h"                   // TryEmitter
#include "frontend/WhileEmitter.h"                 // WhileEmitter
#include "js/friend/ErrorMessages.h"               // JSMSG_*
#include "js/friend/StackLimits.h"                 // AutoCheckRecursionLimit
#include "util/StringBuffer.h"                     // StringBuffer
#include "vm/BytecodeUtil.h"  // JOF_*, IsArgOp, IsLocalOp, SET_UINT24, SET_ICINDEX, BytecodeFallsThrough, BytecodeIsJumpTarget
#include "vm/CompletionKind.h"      // CompletionKind
#include "vm/FunctionPrefixKind.h"  // FunctionPrefixKind
#include "vm/GeneratorObject.h"     // AbstractGeneratorObject
#include "vm/Opcodes.h"             // JSOp, JSOpLength_*
#include "vm/PropMap.h"             // SharedPropMap::MaxPropsForNonDictionary
#include "vm/Scope.h"               // GetScopeDataTrailingNames
#include "vm/SharedStencil.h"       // ScopeNote
#include "vm/ThrowMsgKind.h"        // ThrowMsgKind
#include "vm/WellKnownAtom.h"       // js_*_str

using namespace js;
using namespace js::frontend;

using mozilla::AssertedCast;
using mozilla::AsVariant;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::NumberEqualsInt32;
using mozilla::NumberIsInt32;
using mozilla::PodCopy;
using mozilla::Some;

static bool ParseNodeRequiresSpecialLineNumberNotes(ParseNode* pn) {
  // The few node types listed below are exceptions to the usual
  // location-source-note-emitting code in BytecodeEmitter::emitTree().
  // Single-line `while` loops and C-style `for` loops require careful
  // handling to avoid strange stepping behavior.
  // Functions usually shouldn't have location information (bug 1431202).

  ParseNodeKind kind = pn->getKind();
  return kind == ParseNodeKind::WhileStmt || kind == ParseNodeKind::ForStmt ||
         kind == ParseNodeKind::Function;
}

static bool NeedsFieldInitializer(ParseNode* member, bool inStaticContext) {
  // For the purposes of bytecode emission, StaticClassBlocks are treated as if
  // they were static initializers.
  return (member->is<StaticClassBlock>() && inStaticContext) ||
         (member->is<ClassField>() &&
          member->as<ClassField>().isStatic() == inStaticContext);
}

static bool NeedsAccessorInitializer(ParseNode* member, bool isStatic) {
  if (isStatic) {
    return false;
  }
  return member->is<ClassMethod>() &&
         member->as<ClassMethod>().name().isKind(ParseNodeKind::PrivateName) &&
         !member->as<ClassMethod>().isStatic() &&
         member->as<ClassMethod>().accessorType() != AccessorType::None;
}

static bool ShouldSuppressBreakpointsAndSourceNotes(
    SharedContext* sc, BytecodeEmitter::EmitterMode emitterMode) {
  // Suppress for all self-hosting code.
  if (emitterMode == BytecodeEmitter::EmitterMode::SelfHosting) {
    return true;
  }

  // Suppress for synthesized class constructors.
  if (sc->isFunctionBox()) {
    FunctionBox* funbox = sc->asFunctionBox();
    return funbox->isSyntheticFunction() && funbox->isClassConstructor();
  }

  return false;
}

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent, FrontendContext* fc,
                                 SharedContext* sc,
                                 const ErrorReporter& errorReporter,
                                 CompilationState& compilationState,
                                 EmitterMode emitterMode)
    : sc(sc),
      fc(fc),
      parent(parent),
      bytecodeSection_(fc, sc->extent().lineno, sc->extent().column),
      perScriptData_(fc, compilationState),
      errorReporter_(errorReporter),
      compilationState(compilationState),
      suppressBreakpointsAndSourceNotes(
          ShouldSuppressBreakpointsAndSourceNotes(sc, emitterMode)),
      emitterMode(emitterMode) {
  MOZ_ASSERT_IF(parent, fc == parent->fc);
}

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent, SharedContext* sc)
    : BytecodeEmitter(parent, parent->fc, sc, parent->errorReporter_,
                      parent->compilationState, parent->emitterMode) {}

BytecodeEmitter::BytecodeEmitter(FrontendContext* fc,
                                 const EitherParser& parser, SharedContext* sc,
                                 CompilationState& compilationState,
                                 EmitterMode emitterMode)
    : BytecodeEmitter(nullptr, fc, sc, parser.errorReporter(), compilationState,
                      emitterMode) {
  ep_.emplace(parser);
}

void BytecodeEmitter::initFromBodyPosition(TokenPos bodyPosition) {
  setScriptStartOffsetIfUnset(bodyPosition.begin);
  setFunctionBodyEndPos(bodyPosition.end);
}

bool BytecodeEmitter::init() {
  if (!parent) {
    if (!compilationState.prepareSharedDataStorage(fc)) {
      return false;
    }
  }
  return perScriptData_.init(fc);
}

bool BytecodeEmitter::init(TokenPos bodyPosition) {
  initFromBodyPosition(bodyPosition);
  return init();
}

template <typename T>
T* BytecodeEmitter::findInnermostNestableControl() const {
  return NestableControl::findNearest<T>(innermostNestableControl);
}

template <typename T, typename Predicate /* (T*) -> bool */>
T* BytecodeEmitter::findInnermostNestableControl(Predicate predicate) const {
  return NestableControl::findNearest<T>(innermostNestableControl, predicate);
}

NameLocation BytecodeEmitter::lookupName(TaggedParserAtomIndex name) {
  return innermostEmitterScope()->lookup(this, name);
}

void BytecodeEmitter::lookupPrivate(TaggedParserAtomIndex name,
                                    NameLocation& loc,
                                    Maybe<NameLocation>& brandLoc) {
  innermostEmitterScope()->lookupPrivate(this, name, loc, brandLoc);
}

Maybe<NameLocation> BytecodeEmitter::locationOfNameBoundInScope(
    TaggedParserAtomIndex name, EmitterScope* target) {
  return innermostEmitterScope()->locationBoundInScope(name, target);
}

template <typename T>
Maybe<NameLocation> BytecodeEmitter::locationOfNameBoundInScopeType(
    TaggedParserAtomIndex name, EmitterScope* source) {
  EmitterScope* aScope = source;
  while (!aScope->scope(this).is<T>()) {
    aScope = aScope->enclosingInFrame();
  }
  return source->locationBoundInScope(name, aScope);
}

bool BytecodeEmitter::markStepBreakpoint() {
  if (skipBreakpointSrcNotes()) {
    return true;
  }

  if (!newSrcNote(SrcNoteType::StepSep)) {
    return false;
  }

  if (!newSrcNote(SrcNoteType::Breakpoint)) {
    return false;
  }

  // We track the location of the most recent separator for use in
  // markSimpleBreakpoint. Note that this means that the position must already
  // be set before markStepBreakpoint is called.
  bytecodeSection().updateSeparatorPosition();

  return true;
}

bool BytecodeEmitter::markSimpleBreakpoint() {
  if (skipBreakpointSrcNotes()) {
    return true;
  }

  // If a breakable call ends up being the same location as the most recent
  // expression start, we need to skip marking it breakable in order to avoid
  // having two breakpoints with the same line/column position.
  // Note: This assumes that the position for the call has already been set.
  if (!bytecodeSection().isDuplicateLocation()) {
    if (!newSrcNote(SrcNoteType::Breakpoint)) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitCheck(JSOp op, ptrdiff_t delta,
                                BytecodeOffset* offset) {
  size_t oldLength = bytecodeSection().code().length();
  *offset = BytecodeOffset(oldLength);

  size_t newLength = oldLength + size_t(delta);
  if (MOZ_UNLIKELY(newLength > MaxBytecodeLength)) {
    ReportAllocationOverflow(fc);
    return false;
  }

  if (!bytecodeSection().code().growByUninitialized(delta)) {
    return false;
  }

  if (BytecodeOpHasIC(op)) {
    // Even if every bytecode op is a JOF_IC op and the function has ARGC_LIMIT
    // arguments, numICEntries cannot overflow.
    static_assert(MaxBytecodeLength + 1 /* this */ + ARGC_LIMIT <= UINT32_MAX,
                  "numICEntries must not overflow");
    bytecodeSection().incrementNumICEntries();
  }

  return true;
}

#ifdef DEBUG
bool BytecodeEmitter::checkStrictOrSloppy(JSOp op) {
  if (IsCheckStrictOp(op) && !sc->strict()) {
    return false;
  }
  if (IsCheckSloppyOp(op) && sc->strict()) {
    return false;
  }
  return true;
}
#endif

bool BytecodeEmitter::emit1(JSOp op) {
  MOZ_ASSERT(checkStrictOrSloppy(op));

  BytecodeOffset offset;
  if (!emitCheck(op, 1, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(op);
  bytecodeSection().updateDepth(op, offset);
  return true;
}

bool BytecodeEmitter::emit2(JSOp op, uint8_t op1) {
  MOZ_ASSERT(checkStrictOrSloppy(op));

  BytecodeOffset offset;
  if (!emitCheck(op, 2, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(op);
  code[1] = jsbytecode(op1);
  bytecodeSection().updateDepth(op, offset);
  return true;
}

bool BytecodeEmitter::emit3(JSOp op, jsbytecode op1, jsbytecode op2) {
  MOZ_ASSERT(checkStrictOrSloppy(op));

  /* These should filter through emitVarOp. */
  MOZ_ASSERT(!IsArgOp(op));
  MOZ_ASSERT(!IsLocalOp(op));

  BytecodeOffset offset;
  if (!emitCheck(op, 3, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(op);
  code[1] = op1;
  code[2] = op2;
  bytecodeSection().updateDepth(op, offset);
  return true;
}

bool BytecodeEmitter::emitN(JSOp op, size_t extra, BytecodeOffset* offset) {
  MOZ_ASSERT(checkStrictOrSloppy(op));
  ptrdiff_t length = 1 + ptrdiff_t(extra);

  BytecodeOffset off;
  if (!emitCheck(op, length, &off)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(off);
  code[0] = jsbytecode(op);
  /* The remaining |extra| bytes are set by the caller */

  /*
   * Don't updateDepth if op's use-count comes from the immediate
   * operand yet to be stored in the extra bytes after op.
   */
  if (CodeSpec(op).nuses >= 0) {
    bytecodeSection().updateDepth(op, off);
  }

  if (offset) {
    *offset = off;
  }
  return true;
}

bool BytecodeEmitter::emitJumpTargetOp(JSOp op, BytecodeOffset* off) {
  MOZ_ASSERT(BytecodeIsJumpTarget(op));

  // Record the current IC-entry index at start of this op.
  uint32_t numEntries = bytecodeSection().numICEntries();

  size_t n = GetOpLength(op) - 1;
  MOZ_ASSERT(GetOpLength(op) >= 1 + ICINDEX_LEN);

  if (!emitN(op, n, off)) {
    return false;
  }

  SET_ICINDEX(bytecodeSection().code(*off), numEntries);
  return true;
}

bool BytecodeEmitter::emitJumpTarget(JumpTarget* target) {
  BytecodeOffset off = bytecodeSection().offset();

  // Alias consecutive jump targets.
  if (bytecodeSection().lastTargetOffset().valid() &&
      off == bytecodeSection().lastTargetOffset() +
                 BytecodeOffsetDiff(JSOpLength_JumpTarget)) {
    target->offset = bytecodeSection().lastTargetOffset();
    return true;
  }

  target->offset = off;
  bytecodeSection().setLastTargetOffset(off);

  BytecodeOffset opOff;
  return emitJumpTargetOp(JSOp::JumpTarget, &opOff);
}

bool BytecodeEmitter::emitJumpNoFallthrough(JSOp op, JumpList* jump) {
  BytecodeOffset offset;
  if (!emitCheck(op, 5, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(op);
  MOZ_ASSERT(!jump->offset.valid() ||
             (0 <= jump->offset.value() && jump->offset < offset));
  jump->push(bytecodeSection().code(BytecodeOffset(0)), offset);
  bytecodeSection().updateDepth(op, offset);
  return true;
}

bool BytecodeEmitter::emitJump(JSOp op, JumpList* jump) {
  if (!emitJumpNoFallthrough(op, jump)) {
    return false;
  }
  if (BytecodeFallsThrough(op)) {
    JumpTarget fallthrough;
    if (!emitJumpTarget(&fallthrough)) {
      return false;
    }
  }
  return true;
}

void BytecodeEmitter::patchJumpsToTarget(JumpList jump, JumpTarget target) {
  MOZ_ASSERT(
      !jump.offset.valid() ||
      (0 <= jump.offset.value() && jump.offset <= bytecodeSection().offset()));
  MOZ_ASSERT(0 <= target.offset.value() &&
             target.offset <= bytecodeSection().offset());
  MOZ_ASSERT_IF(
      jump.offset.valid() &&
          target.offset + BytecodeOffsetDiff(4) <= bytecodeSection().offset(),
      BytecodeIsJumpTarget(JSOp(*bytecodeSection().code(target.offset))));
  jump.patchAll(bytecodeSection().code(BytecodeOffset(0)), target);
}

bool BytecodeEmitter::emitJumpTargetAndPatch(JumpList jump) {
  if (!jump.offset.valid()) {
    return true;
  }
  JumpTarget target;
  if (!emitJumpTarget(&target)) {
    return false;
  }
  patchJumpsToTarget(jump, target);
  return true;
}

bool BytecodeEmitter::emitCall(JSOp op, uint16_t argc,
                               const Maybe<uint32_t>& sourceCoordOffset) {
  if (sourceCoordOffset.isSome()) {
    if (!updateSourceCoordNotes(*sourceCoordOffset)) {
      return false;
    }
  }
  return emit3(op, ARGC_LO(argc), ARGC_HI(argc));
}

bool BytecodeEmitter::emitCall(JSOp op, uint16_t argc, ParseNode* pn) {
  return emitCall(op, argc, pn ? Some(pn->pn_pos.begin) : Nothing());
}

bool BytecodeEmitter::emitDupAt(unsigned slotFromTop, unsigned count) {
  MOZ_ASSERT(slotFromTop < unsigned(bytecodeSection().stackDepth()));
  MOZ_ASSERT(slotFromTop + 1 >= count);

  if (slotFromTop == 0 && count == 1) {
    return emit1(JSOp::Dup);
  }

  if (slotFromTop == 1 && count == 2) {
    return emit1(JSOp::Dup2);
  }

  if (slotFromTop >= Bit(24)) {
    reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
    return false;
  }

  for (unsigned i = 0; i < count; i++) {
    BytecodeOffset off;
    if (!emitN(JSOp::DupAt, 3, &off)) {
      return false;
    }

    jsbytecode* pc = bytecodeSection().code(off);
    SET_UINT24(pc, slotFromTop);
  }

  return true;
}

bool BytecodeEmitter::emitPopN(unsigned n) {
  MOZ_ASSERT(n != 0);

  if (n == 1) {
    return emit1(JSOp::Pop);
  }

  // 2 JSOp::Pop instructions (2 bytes) are shorter than JSOp::PopN (3 bytes).
  if (n == 2) {
    return emit1(JSOp::Pop) && emit1(JSOp::Pop);
  }

  return emitUint16Operand(JSOp::PopN, n);
}

bool BytecodeEmitter::emitPickN(uint8_t n) {
  MOZ_ASSERT(n != 0);

  if (n == 1) {
    return emit1(JSOp::Swap);
  }

  return emit2(JSOp::Pick, n);
}

bool BytecodeEmitter::emitUnpickN(uint8_t n) {
  MOZ_ASSERT(n != 0);

  if (n == 1) {
    return emit1(JSOp::Swap);
  }

  return emit2(JSOp::Unpick, n);
}

bool BytecodeEmitter::emitCheckIsObj(CheckIsObjectKind kind) {
  return emit2(JSOp::CheckIsObj, uint8_t(kind));
}

bool BytecodeEmitter::emitBuiltinObject(BuiltinObjectKind kind) {
  return emit2(JSOp::BuiltinObject, uint8_t(kind));
}

/* Updates line number notes, not column notes. */
bool BytecodeEmitter::updateLineNumberNotes(uint32_t offset) {
  if (skipLocationSrcNotes()) {
    return true;
  }

  const ErrorReporter& er = errorReporter();
  bool onThisLine;
  if (!er.isOnThisLine(offset, bytecodeSection().currentLine(), &onThisLine)) {
    er.errorNoOffset(JSMSG_OUT_OF_MEMORY);
    return false;
  }

  if (!onThisLine) {
    unsigned line = er.lineAt(offset);
    unsigned delta = line - bytecodeSection().currentLine();

    // If we use a `SetLine` note below, we want it to be relative to the
    // scripts initial line number for better chance of sharing.
    unsigned initialLine = sc->extent().lineno;
    MOZ_ASSERT(line >= initialLine);

    /*
     * Encode any change in the current source line number by using
     * either several SrcNoteType::NewLine notes or just one
     * SrcNoteType::SetLine note, whichever consumes less space.
     *
     * NB: We handle backward line number deltas (possible with for
     * loops where the update part is emitted after the body, but its
     * line number is <= any line number in the body) here by letting
     * unsigned delta_ wrap to a very large number, which triggers a
     * SrcNoteType::SetLine.
     */
    bytecodeSection().setCurrentLine(line, offset);
    if (delta >= SrcNote::SetLine::lengthFor(line, initialLine)) {
      if (!newSrcNote2(SrcNoteType::SetLine,
                       SrcNote::SetLine::toOperand(line, initialLine))) {
        return false;
      }
    } else {
      do {
        if (!newSrcNote(SrcNoteType::NewLine)) {
          return false;
        }
      } while (--delta != 0);
    }

    bytecodeSection().updateSeparatorPositionIfPresent();
  }
  return true;
}

/* Updates the line number and column number information in the source notes. */
bool BytecodeEmitter::updateSourceCoordNotes(uint32_t offset) {
  if (!updateLineNumberNotes(offset)) {
    return false;
  }

  if (skipLocationSrcNotes()) {
    return true;
  }

  uint32_t columnIndex = errorReporter().columnAt(offset);
  MOZ_ASSERT(columnIndex <= ColumnLimit);

  // Assert colspan is always representable.
  static_assert((0 - ptrdiff_t(ColumnLimit)) >= SrcNote::ColSpan::MinColSpan);
  static_assert((ptrdiff_t(ColumnLimit) - 0) <= SrcNote::ColSpan::MaxColSpan);

  ptrdiff_t colspan =
      ptrdiff_t(columnIndex) - ptrdiff_t(bytecodeSection().lastColumn());

  if (colspan != 0) {
    if (!newSrcNote2(SrcNoteType::ColSpan,
                     SrcNote::ColSpan::toOperand(colspan))) {
      return false;
    }
    bytecodeSection().setLastColumn(columnIndex, offset);
    bytecodeSection().updateSeparatorPositionIfPresent();
  }
  return true;
}

uint32_t BytecodeEmitter::getOffsetForLoop(ParseNode* nextpn) {
  // Try to give the JSOp::LoopHead the same line number as the next
  // instruction. nextpn is often a block, in which case the next instruction
  // typically comes from the first statement inside.
  if (nextpn->is<LexicalScopeNode>()) {
    nextpn = nextpn->as<LexicalScopeNode>().scopeBody();
  }
  if (nextpn->isKind(ParseNodeKind::StatementList)) {
    if (ParseNode* firstStatement = nextpn->as<ListNode>().head()) {
      nextpn = firstStatement;
    }
  }

  return nextpn->pn_pos.begin;
}

bool BytecodeEmitter::emitUint16Operand(JSOp op, uint32_t operand) {
  MOZ_ASSERT(operand <= UINT16_MAX);
  if (!emit3(op, UINT16_LO(operand), UINT16_HI(operand))) {
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitUint32Operand(JSOp op, uint32_t operand) {
  BytecodeOffset off;
  if (!emitN(op, 4, &off)) {
    return false;
  }
  SET_UINT32(bytecodeSection().code(off), operand);
  return true;
}

bool BytecodeEmitter::emitGoto(NestableControl* target, GotoKind kind) {
  NonLocalExitControl nle(this, kind == GotoKind::Continue
                                    ? NonLocalExitKind::Continue
                                    : NonLocalExitKind::Break);
  return nle.emitNonLocalJump(target);
}

AbstractScopePtr BytecodeEmitter::innermostScope() const {
  return innermostEmitterScope()->scope(this);
}

ScopeIndex BytecodeEmitter::innermostScopeIndex() const {
  return *innermostEmitterScope()->scopeIndex(this);
}

bool BytecodeEmitter::emitGCIndexOp(JSOp op, GCThingIndex index) {
  MOZ_ASSERT(checkStrictOrSloppy(op));

  constexpr size_t OpLength = 1 + GCTHING_INDEX_LEN;
  MOZ_ASSERT(GetOpLength(op) == OpLength);

  BytecodeOffset offset;
  if (!emitCheck(op, OpLength, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(op);
  SET_GCTHING_INDEX(code, index);
  bytecodeSection().updateDepth(op, offset);
  return true;
}

bool BytecodeEmitter::emitAtomOp(JSOp op, TaggedParserAtomIndex atom) {
  MOZ_ASSERT(atom);

  // .generator lookups should be emitted as JSOp::GetAliasedVar instead of
  // JSOp::GetName etc, to bypass |with| objects on the scope chain.
  // It's safe to emit .this lookups though because |with| objects skip
  // those.
  MOZ_ASSERT_IF(op == JSOp::GetName || op == JSOp::GetGName,
                atom != TaggedParserAtomIndex::WellKnown::dotGenerator());

  GCThingIndex index;
  if (!makeAtomIndex(atom, ParserAtom::Atomize::Yes, &index)) {
    return false;
  }

  return emitAtomOp(op, index);
}

bool BytecodeEmitter::emitAtomOp(JSOp op, GCThingIndex atomIndex) {
  MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
#ifdef DEBUG
  auto atom = perScriptData().gcThingList().getAtom(atomIndex);
  MOZ_ASSERT(compilationState.parserAtoms.isInstantiatedAsJSAtom(atom));
#endif
  return emitGCIndexOp(op, atomIndex);
}

bool BytecodeEmitter::emitStringOp(JSOp op, TaggedParserAtomIndex atom) {
  MOZ_ASSERT(atom);
  GCThingIndex index;
  if (!makeAtomIndex(atom, ParserAtom::Atomize::No, &index)) {
    return false;
  }

  return emitStringOp(op, index);
}

bool BytecodeEmitter::emitStringOp(JSOp op, GCThingIndex atomIndex) {
  MOZ_ASSERT(JOF_OPTYPE(op) == JOF_STRING);
  return emitGCIndexOp(op, atomIndex);
}

bool BytecodeEmitter::emitInternedScopeOp(GCThingIndex index, JSOp op) {
  MOZ_ASSERT(JOF_OPTYPE(op) == JOF_SCOPE);
  MOZ_ASSERT(index < perScriptData().gcThingList().length());
  return emitGCIndexOp(op, index);
}

bool BytecodeEmitter::emitInternedObjectOp(GCThingIndex index, JSOp op) {
  MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
  MOZ_ASSERT(index < perScriptData().gcThingList().length());
  return emitGCIndexOp(op, index);
}

bool BytecodeEmitter::emitRegExp(GCThingIndex index) {
  return emitGCIndexOp(JSOp::RegExp, index);
}

bool BytecodeEmitter::emitLocalOp(JSOp op, uint32_t slot) {
  MOZ_ASSERT(JOF_OPTYPE(op) != JOF_ENVCOORD);
  MOZ_ASSERT(IsLocalOp(op));

  BytecodeOffset off;
  if (!emitN(op, LOCALNO_LEN, &off)) {
    return false;
  }

  SET_LOCALNO(bytecodeSection().code(off), slot);
  return true;
}

bool BytecodeEmitter::emitArgOp(JSOp op, uint16_t slot) {
  MOZ_ASSERT(IsArgOp(op));
  BytecodeOffset off;
  if (!emitN(op, ARGNO_LEN, &off)) {
    return false;
  }

  SET_ARGNO(bytecodeSection().code(off), slot);
  return true;
}

bool BytecodeEmitter::emitEnvCoordOp(JSOp op, EnvironmentCoordinate ec) {
  MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ENVCOORD ||
             JOF_OPTYPE(op) == JOF_DEBUGCOORD);

  constexpr size_t N = ENVCOORD_HOPS_LEN + ENVCOORD_SLOT_LEN;
  MOZ_ASSERT(GetOpLength(op) == 1 + N);

  BytecodeOffset off;
  if (!emitN(op, N, &off)) {
    return false;
  }

  jsbytecode* pc = bytecodeSection().code(off);
  SET_ENVCOORD_HOPS(pc, ec.hops());
  pc += ENVCOORD_HOPS_LEN;
  SET_ENVCOORD_SLOT(pc, ec.slot());
  pc += ENVCOORD_SLOT_LEN;
  return true;
}

JSOp BytecodeEmitter::strictifySetNameOp(JSOp op) {
  switch (op) {
    case JSOp::SetName:
      if (sc->strict()) {
        op = JSOp::StrictSetName;
      }
      break;
    case JSOp::SetGName:
      if (sc->strict()) {
        op = JSOp::StrictSetGName;
      }
      break;
    default:;
  }
  return op;
}

bool BytecodeEmitter::checkSideEffects(ParseNode* pn, bool* answer) {
  AutoCheckRecursionLimit recursion(fc);
  if (!recursion.check(fc)) {
    return false;
  }

restart:

  switch (pn->getKind()) {
    // Trivial cases with no side effects.
    case ParseNodeKind::EmptyStmt:
    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
    case ParseNodeKind::Elision:
    case ParseNodeKind::Generator:
      MOZ_ASSERT(pn->is<NullaryNode>());
      *answer = false;
      return true;

    case ParseNodeKind::ObjectPropertyName:
    case ParseNodeKind::PrivateName:  // no side effects, unlike
                                      // ParseNodeKind::Name
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
      MOZ_ASSERT(pn->is<NameNode>());
      *answer = false;
      return true;

    case ParseNodeKind::RegExpExpr:
      MOZ_ASSERT(pn->is<RegExpLiteral>());
      *answer = false;
      return true;

    case ParseNodeKind::NumberExpr:
      MOZ_ASSERT(pn->is<NumericLiteral>());
      *answer = false;
      return true;

    case ParseNodeKind::BigIntExpr:
      MOZ_ASSERT(pn->is<BigIntLiteral>());
      *answer = false;
      return true;

    // |this| can throw in derived class constructors, including nested arrow
    // functions or eval.
    case ParseNodeKind::ThisExpr:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = sc->needsThisTDZChecks();
      return true;

    // |new.target| doesn't have any side-effects.
    case ParseNodeKind::NewTargetExpr: {
      MOZ_ASSERT(pn->is<NewTargetNode>());
      *answer = false;
      return true;
    }

    // Trivial binary nodes with more token pos holders.
    case ParseNodeKind::ImportMetaExpr: {
      MOZ_ASSERT(pn->as<BinaryNode>().left()->isKind(ParseNodeKind::PosHolder));
      MOZ_ASSERT(
          pn->as<BinaryNode>().right()->isKind(ParseNodeKind::PosHolder));
      *answer = false;
      return true;
    }

    case ParseNodeKind::BreakStmt:
      MOZ_ASSERT(pn->is<BreakStatement>());
      *answer = true;
      return true;

    case ParseNodeKind::ContinueStmt:
      MOZ_ASSERT(pn->is<ContinueStatement>());
      *answer = true;
      return true;

    case ParseNodeKind::DebuggerStmt:
      MOZ_ASSERT(pn->is<DebuggerStatement>());
      *answer = true;
      return true;

    // Watch out for getters!
    case ParseNodeKind::OptionalDotExpr:
    case ParseNodeKind::DotExpr:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Unary cases with side effects only if the child has them.
    case ParseNodeKind::TypeOfExpr:
    case ParseNodeKind::VoidExpr:
    case ParseNodeKind::NotExpr:
      return checkSideEffects(pn->as<UnaryNode>().kid(), answer);

    // Even if the name expression is effect-free, performing ToPropertyKey on
    // it might not be effect-free:
    //
    //   RegExp.prototype.toString = () => { throw 42; };
    //   ({ [/regex/]: 0 }); // ToPropertyKey(/regex/) throws 42
    //
    //   function Q() {
    //     ({ [new.target]: 0 });
    //   }
    //   Q.toString = () => { throw 17; };
    //   new Q; // new.target will be Q, ToPropertyKey(Q) throws 17
    case ParseNodeKind::ComputedName:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // Looking up or evaluating the associated name could throw.
    case ParseNodeKind::TypeOfNameExpr:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // This unary case has side effects on the enclosing object, sure.  But
    // that's not the question this function answers: it's whether the
    // operation may have a side effect on something *other* than the result
    // of the overall operation in which it's embedded.  The answer to that
    // is no, because an object literal having a mutated prototype only
    // produces a value, without affecting anything else.
    case ParseNodeKind::MutateProto:
      return checkSideEffects(pn->as<UnaryNode>().kid(), answer);

    // Unary cases with obvious side effects.
    case ParseNodeKind::PreIncrementExpr:
    case ParseNodeKind::PostIncrementExpr:
    case ParseNodeKind::PreDecrementExpr:
    case ParseNodeKind::PostDecrementExpr:
    case ParseNodeKind::ThrowStmt:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // These might invoke valueOf/toString, even with a subexpression without
    // side effects!  Consider |+{ valueOf: null, toString: null }|.
    case ParseNodeKind::BitNotExpr:
    case ParseNodeKind::PosExpr:
    case ParseNodeKind::NegExpr:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // This invokes the (user-controllable) iterator protocol.
    case ParseNodeKind::Spread:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::InitialYield:
    case ParseNodeKind::YieldStarExpr:
    case ParseNodeKind::YieldExpr:
    case ParseNodeKind::AwaitExpr:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // Deletion generally has side effects, even if isolated cases have none.
    case ParseNodeKind::DeleteNameExpr:
    case ParseNodeKind::DeletePropExpr:
    case ParseNodeKind::DeleteElemExpr:
    case ParseNodeKind::DeleteOptionalChainExpr:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // Deletion of a non-Reference expression has side effects only through
    // evaluating the expression.
    case ParseNodeKind::DeleteExpr: {
      ParseNode* expr = pn->as<UnaryNode>().kid();
      return checkSideEffects(expr, answer);
    }

    case ParseNodeKind::ExpressionStmt:
      return checkSideEffects(pn->as<UnaryNode>().kid(), answer);

    // Binary cases with obvious side effects.
    case ParseNodeKind::InitExpr:
      *answer = true;
      return true;

    case ParseNodeKind::AssignExpr:
    case ParseNodeKind::AddAssignExpr:
    case ParseNodeKind::SubAssignExpr:
    case ParseNodeKind::CoalesceAssignExpr:
    case ParseNodeKind::OrAssignExpr:
    case ParseNodeKind::AndAssignExpr:
    case ParseNodeKind::BitOrAssignExpr:
    case ParseNodeKind::BitXorAssignExpr:
    case ParseNodeKind::BitAndAssignExpr:
    case ParseNodeKind::LshAssignExpr:
    case ParseNodeKind::RshAssignExpr:
    case ParseNodeKind::UrshAssignExpr:
    case ParseNodeKind::MulAssignExpr:
    case ParseNodeKind::DivAssignExpr:
    case ParseNodeKind::ModAssignExpr:
    case ParseNodeKind::PowAssignExpr:
      MOZ_ASSERT(pn->is<AssignmentNode>());
      *answer = true;
      return true;

    case ParseNodeKind::SetThis:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::StatementList:
    // Strict equality operations and short circuit operators are well-behaved
    // and perform no conversions.
    case ParseNodeKind::CoalesceExpr:
    case ParseNodeKind::OrExpr:
    case ParseNodeKind::AndExpr:
    case ParseNodeKind::StrictEqExpr:
    case ParseNodeKind::StrictNeExpr:
    // Any subexpression of a comma expression could be effectful.
    case ParseNodeKind::CommaExpr:
      MOZ_ASSERT(!pn->as<ListNode>().empty());
      [[fallthrough]];
    // Subcomponents of a literal may be effectful.
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      for (ParseNode* item : pn->as<ListNode>().contents()) {
        if (!checkSideEffects(item, answer)) {
          return false;
        }
        if (*answer) {
          return true;
        }
      }
      return true;

#ifdef ENABLE_RECORD_TUPLE
    case ParseNodeKind::RecordExpr:
    case ParseNodeKind::TupleExpr:
      MOZ_CRASH("Record and Tuple are not supported yet");
#endif

#ifdef ENABLE_DECORATORS
    case ParseNodeKind::DecoratorList:
      MOZ_CRASH("Decorators are not supported yet");
#endif

    // Most other binary operations (parsed as lists in SpiderMonkey) may
    // perform conversions triggering side effects.  Math operations perform
    // ToNumber and may fail invoking invalid user-defined toString/valueOf:
    // |5 < { toString: null }|.  |instanceof| throws if provided a
    // non-object constructor: |null instanceof null|.  |in| throws if given
    // a non-object RHS: |5 in null|.
    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::NeExpr:
    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::InstanceOfExpr:
    case ParseNodeKind::InExpr:
    case ParseNodeKind::PrivateInExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
    case ParseNodeKind::MulExpr:
    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
    case ParseNodeKind::PowExpr:
      MOZ_ASSERT(pn->as<ListNode>().count() >= 2);
      *answer = true;
      return true;

    case ParseNodeKind::PropertyDefinition:
    case ParseNodeKind::Case: {
      BinaryNode* node = &pn->as<BinaryNode>();
      if (!checkSideEffects(node->left(), answer)) {
        return false;
      }
      if (*answer) {
        return true;
      }
      return checkSideEffects(node->right(), answer);
    }

    // More getters.
    case ParseNodeKind::ElemExpr:
    case ParseNodeKind::OptionalElemExpr:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Throws if the operand is not of the right class. Can also call a private
    // getter.
    case ParseNodeKind::PrivateMemberExpr:
    case ParseNodeKind::OptionalPrivateMemberExpr:
      *answer = true;
      return true;

    // These affect visible names in this code, or in other code.
    case ParseNodeKind::ImportDecl:
    case ParseNodeKind::ExportFromStmt:
    case ParseNodeKind::ExportDefaultStmt:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Likewise.
    case ParseNodeKind::ExportStmt:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::CallImportExpr:
    case ParseNodeKind::CallImportSpec:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Every part of a loop might be effect-free, but looping infinitely *is*
    // an effect.  (Language lawyer trivia: C++ says threads can be assumed
    // to exit or have side effects, C++14 [intro.multithread]p27, so a C++
    // implementation's equivalent of the below could set |*answer = false;|
    // if all loop sub-nodes set |*answer = false|!)
    case ParseNodeKind::DoWhileStmt:
    case ParseNodeKind::WhileStmt:
    case ParseNodeKind::ForStmt:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Declarations affect the name set of the relevant scope.
    case ParseNodeKind::VarStmt:
    case ParseNodeKind::ConstDecl:
    case ParseNodeKind::LetDecl:
      MOZ_ASSERT(pn->is<ListNode>());
      *answer = true;
      return true;

    case ParseNodeKind::IfStmt:
    case ParseNodeKind::ConditionalExpr: {
      TernaryNode* node = &pn->as<TernaryNode>();
      if (!checkSideEffects(node->kid1(), answer)) {
        return false;
      }
      if (*answer) {
        return true;
      }
      if (!checkSideEffects(node->kid2(), answer)) {
        return false;
      }
      if (*answer) {
        return true;
      }
      if ((pn = node->kid3())) {
        goto restart;
      }
      return true;
    }

    // Function calls can invoke non-local code.
    case ParseNodeKind::NewExpr:
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::OptionalCallExpr:
    case ParseNodeKind::TaggedTemplateExpr:
    case ParseNodeKind::SuperCallExpr:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    // Function arg lists can contain arbitrary expressions. Technically
    // this only causes side-effects if one of the arguments does, but since
    // the call being made will always trigger side-effects, it isn't needed.
    case ParseNodeKind::Arguments:
      MOZ_ASSERT(pn->is<ListNode>());
      *answer = true;
      return true;

    case ParseNodeKind::OptionalChain:
      MOZ_ASSERT(pn->is<UnaryNode>());
      *answer = true;
      return true;

    // Classes typically introduce names.  Even if no name is introduced,
    // the heritage and/or class body (through computed property names)
    // usually have effects.
    case ParseNodeKind::ClassDecl:
      MOZ_ASSERT(pn->is<ClassNode>());
      *answer = true;
      return true;

    // |with| calls |ToObject| on its expression and so throws if that value
    // is null/undefined.
    case ParseNodeKind::WithStmt:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::ReturnStmt:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::Name:
      MOZ_ASSERT(pn->is<NameNode>());
      *answer = true;
      return true;

    // Shorthands could trigger getters: the |x| in the object literal in
    // |with ({ get x() { throw 42; } }) ({ x });|, for example, triggers
    // one.  (Of course, it isn't necessary to use |with| for a shorthand to
    // trigger a getter.)
    case ParseNodeKind::Shorthand:
      MOZ_ASSERT(pn->is<BinaryNode>());
      *answer = true;
      return true;

    case ParseNodeKind::Function:
      MOZ_ASSERT(pn->is<FunctionNode>());
      /*
       * A named function, contrary to ES3, is no longer effectful, because
       * we bind its name lexically (using JSOp::Callee) instead of creating
       * an Object instance and binding a readonly, permanent property in it
       * (the object and binding can be detected and hijacked or captured).
       * This is a bug fix to ES3; it is fixed in ES3.1 drafts.
       */
      *answer = false;
      return true;

    case ParseNodeKind::Module:
      *answer = false;
      return true;

    case ParseNodeKind::TryStmt: {
      TryNode* tryNode = &pn->as<TryNode>();
      if (!checkSideEffects(tryNode->body(), answer)) {
        return false;
      }
      if (*answer) {
        return true;
      }
      if (LexicalScopeNode* catchScope = tryNode->catchScope()) {
        if (!checkSideEffects(catchScope, answer)) {
          return false;
        }
        if (*answer) {
          return true;
        }
      }
      if (ParseNode* finallyBlock = tryNode->finallyBlock()) {
        if (!checkSideEffects(finallyBlock, answer)) {
          return false;
        }
      }
      return true;
    }

    case ParseNodeKind::Catch: {
      BinaryNode* catchClause = &pn->as<BinaryNode>();
      if (ParseNode* name = catchClause->left()) {
        if (!checkSideEffects(name, answer)) {
          return false;
        }
        if (*answer) {
          return true;
        }
      }
      return checkSideEffects(catchClause->right(), answer);
    }

    case ParseNodeKind::SwitchStmt: {
      SwitchStatement* switchStmt = &pn->as<SwitchStatement>();
      if (!checkSideEffects(&switchStmt->discriminant(), answer)) {
        return false;
      }
      return *answer ||
             checkSideEffects(&switchStmt->lexicalForCaseList(), answer);
    }

    case ParseNodeKind::LabelStmt:
      return checkSideEffects(pn->as<LabeledStatement>().statement(), answer);

    case ParseNodeKind::LexicalScope:
      return checkSideEffects(pn->as<LexicalScopeNode>().scopeBody(), answer);

    // We could methodically check every interpolated expression, but it's
    // probably not worth the trouble.  Treat template strings as effect-free
    // only if they don't contain any substitutions.
    case ParseNodeKind::TemplateStringListExpr: {
      ListNode* list = &pn->as<ListNode>();
      MOZ_ASSERT(!list->empty());
      MOZ_ASSERT((list->count() % 2) == 1,
                 "template strings must alternate template and substitution "
                 "parts");
      *answer = list->count() > 1;
      return true;
    }

    // This should be unreachable but is left as-is for now.
    case ParseNodeKind::ParamsBody:
      *answer = true;
      return true;

    case ParseNodeKind::ForIn:                // by ParseNodeKind::For
    case ParseNodeKind::ForOf:                // by ParseNodeKind::For
    case ParseNodeKind::ForHead:              // by ParseNodeKind::For
    case ParseNodeKind::DefaultConstructor:   // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ClassBodyScope:       // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ClassMethod:          // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ClassField:           // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ClassNames:           // by ParseNodeKind::ClassDecl
    case ParseNodeKind::StaticClassBlock:     // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ClassMemberList:      // by ParseNodeKind::ClassDecl
    case ParseNodeKind::ImportSpecList:       // by ParseNodeKind::Import
    case ParseNodeKind::ImportSpec:           // by ParseNodeKind::Import
    case ParseNodeKind::ImportNamespaceSpec:  // by ParseNodeKind::Import
    case ParseNodeKind::ImportAssertion:      // by ParseNodeKind::Import
    case ParseNodeKind::ImportAssertionList:  // by ParseNodeKind::Import
    case ParseNodeKind::ImportModuleRequest:  // by ParseNodeKind::Import
    case ParseNodeKind::ExportBatchSpecStmt:  // by ParseNodeKind::Export
    case ParseNodeKind::ExportSpecList:       // by ParseNodeKind::Export
    case ParseNodeKind::ExportSpec:           // by ParseNodeKind::Export
    case ParseNodeKind::ExportNamespaceSpec:  // by ParseNodeKind::Export
    case ParseNodeKind::CallSiteObj:       // by ParseNodeKind::TaggedTemplate
    case ParseNodeKind::PosHolder:         // by ParseNodeKind::NewTarget
    case ParseNodeKind::SuperBase:         // by ParseNodeKind::Elem and others
    case ParseNodeKind::PropertyNameExpr:  // by ParseNodeKind::Dot
      MOZ_CRASH("handled by parent nodes");

    case ParseNodeKind::LastUnused:
    case ParseNodeKind::Limit:
      MOZ_CRASH("invalid node kind");
  }

  MOZ_CRASH(
      "invalid, unenumerated ParseNodeKind value encountered in "
      "BytecodeEmitter::checkSideEffects");
}

bool BytecodeEmitter::isInLoop() {
  return findInnermostNestableControl<LoopControl>();
}

bool BytecodeEmitter::checkSingletonContext() {
  MOZ_ASSERT_IF(sc->treatAsRunOnce(), sc->isTopLevelContext());
  return sc->treatAsRunOnce() && !isInLoop();
}

bool BytecodeEmitter::needsImplicitThis() {
  // Short-circuit if there is an enclosing 'with' scope.
  if (sc->inWith()) {
    return true;
  }

  // Otherwise see if the current point is under a 'with'.
  for (EmitterScope* es = innermostEmitterScope(); es;
       es = es->enclosingInFrame()) {
    if (es->scope(this).kind() == ScopeKind::With) {
      return true;
    }
  }

  return false;
}

size_t BytecodeEmitter::countThisEnvironmentHops() {
  unsigned numHops = 0;

  for (BytecodeEmitter* current = this; current; current = current->parent) {
    for (EmitterScope* es = current->innermostEmitterScope(); es;
         es = es->enclosingInFrame()) {
      if (es->scope(current).is<FunctionScope>()) {
        if (!es->scope(current).isArrow()) {
          // The Parser is responsible for marking the environment as either
          // closed-over or used-by-eval which ensure that is must exist.
          MOZ_ASSERT(es->scope(current).hasEnvironment());
          return numHops;
        }
      }
      if (es->scope(current).hasEnvironment()) {
        numHops++;
      }
    }
  }

  // The "this" environment exists outside of the compilation, but the
  // `ScopeContext` recorded the number of additional hops needed, so add
  // those in now.
  MOZ_ASSERT(sc->allowSuperProperty());
  numHops += compilationState.scopeContext.enclosingThisEnvironmentHops;
  return numHops;
}

bool BytecodeEmitter::emitThisEnvironmentCallee() {
  // Get the innermost enclosing function that has a |this| binding.

  // Directly load callee from the frame if possible.
  if (sc->isFunctionBox() && !sc->asFunctionBox()->isArrow()) {
    return emit1(JSOp::Callee);
  }

  // We have to load the callee from the environment chain.
  size_t numHops = countThisEnvironmentHops();

  static_assert(
      ENVCOORD_HOPS_LIMIT - 1 <= UINT8_MAX,
      "JSOp::EnvCallee operand size should match ENVCOORD_HOPS_LIMIT");

  MOZ_ASSERT(numHops < ENVCOORD_HOPS_LIMIT - 1);

  return emit2(JSOp::EnvCallee, numHops);
}

bool BytecodeEmitter::emitSuperBase() {
  if (!emitThisEnvironmentCallee()) {
    return false;
  }

  return emit1(JSOp::SuperBase);
}

void BytecodeEmitter::reportError(ParseNode* pn, unsigned errorNumber, ...) {
  uint32_t offset = pn ? pn->pn_pos.begin : *scriptStartOffset;

  va_list args;
  va_start(args, errorNumber);

  errorReporter().errorWithNotesAtVA(nullptr, AsVariant(offset), errorNumber,
                                     &args);

  va_end(args);
}

void BytecodeEmitter::reportError(uint32_t offset, unsigned errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);

  errorReporter().errorWithNotesAtVA(nullptr, AsVariant(offset), errorNumber,
                                     &args);

  va_end(args);
}

bool BytecodeEmitter::addObjLiteralData(ObjLiteralWriter& writer,
                                        GCThingIndex* outIndex) {
  if (!writer.checkForDuplicatedNames(fc)) {
    return false;
  }

  size_t len = writer.getCode().size();
  auto* code = compilationState.alloc.newArrayUninitialized<uint8_t>(len);
  if (!code) {
    js::ReportOutOfMemory(fc);
    return false;
  }
  memcpy(code, writer.getCode().data(), len);

  ObjLiteralIndex objIndex(compilationState.objLiteralData.length());
  if (uint32_t(objIndex) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc);
    return false;
  }
  if (!compilationState.objLiteralData.emplaceBack(code, len, writer.getKind(),
                                                   writer.getFlags(),
                                                   writer.getPropertyCount())) {
    js::ReportOutOfMemory(fc);
    return false;
  }

  return perScriptData().gcThingList().append(objIndex, outIndex);
}

bool BytecodeEmitter::emitPrepareIteratorResult() {
  constexpr JSOp op = JSOp::NewObject;

  ObjLiteralWriter writer;
  writer.beginShape(op);

  writer.setPropNameNoDuplicateCheck(parserAtoms(),
                                     TaggedParserAtomIndex::WellKnown::value());
  if (!writer.propWithUndefinedValue(fc)) {
    return false;
  }
  writer.setPropNameNoDuplicateCheck(parserAtoms(),
                                     TaggedParserAtomIndex::WellKnown::done());
  if (!writer.propWithUndefinedValue(fc)) {
    return false;
  }

  GCThingIndex shape;
  if (!addObjLiteralData(writer, &shape)) {
    return false;
  }

  return emitGCIndexOp(op, shape);
}

bool BytecodeEmitter::emitFinishIteratorResult(bool done) {
  if (!emitAtomOp(JSOp::InitProp, TaggedParserAtomIndex::WellKnown::value())) {
    return false;
  }
  if (!emit1(done ? JSOp::True : JSOp::False)) {
    return false;
  }
  if (!emitAtomOp(JSOp::InitProp, TaggedParserAtomIndex::WellKnown::done())) {
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitGetNameAtLocation(TaggedParserAtomIndex name,
                                            const NameLocation& loc) {
  NameOpEmitter noe(this, name, loc, NameOpEmitter::Kind::Get);
  if (!noe.emitGet()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitGetName(NameNode* name) {
  MOZ_ASSERT(name->isKind(ParseNodeKind::Name));

  return emitGetName(name->name());
}

bool BytecodeEmitter::emitGetPrivateName(NameNode* name) {
  MOZ_ASSERT(name->isKind(ParseNodeKind::PrivateName));
  return emitGetPrivateName(name->name());
}

bool BytecodeEmitter::emitGetPrivateName(TaggedParserAtomIndex nameAtom) {
  // The parser ensures the private name is present on the environment chain,
  // but its location can be Dynamic or Global when emitting debugger
  // eval-in-frame code.
  NameLocation location = lookupName(nameAtom);
  MOZ_ASSERT(location.kind() == NameLocation::Kind::FrameSlot ||
             location.kind() == NameLocation::Kind::EnvironmentCoordinate ||
             location.kind() == NameLocation::Kind::Dynamic ||
             location.kind() == NameLocation::Kind::Global);

  return emitGetNameAtLocation(nameAtom, location);
}

bool BytecodeEmitter::emitTDZCheckIfNeeded(TaggedParserAtomIndex name,
                                           const NameLocation& loc,
                                           ValueIsOnStack isOnStack) {
  // Dynamic accesses have TDZ checks built into their VM code and should
  // never emit explicit TDZ checks.
  MOZ_ASSERT(loc.hasKnownSlot());
  MOZ_ASSERT(loc.isLexical() || loc.isPrivateMethod() || loc.isSynthetic());

  // Private names are implemented as lexical bindings, but it's just an
  // implementation detail. Per spec there's no TDZ check when using them.
  if (parserAtoms().isPrivateName(name)) {
    return true;
  }

  Maybe<MaybeCheckTDZ> check =
      innermostTDZCheckCache->needsTDZCheck(this, name);
  if (!check) {
    return false;
  }

  // We've already emitted a check in this basic block.
  if (*check == DontCheckTDZ) {
    return true;
  }

  // If the value is not on the stack, we have to load it first.
  if (isOnStack == ValueIsOnStack::No) {
    if (loc.kind() == NameLocation::Kind::FrameSlot) {
      if (!emitLocalOp(JSOp::GetLocal, loc.frameSlot())) {
        return false;
      }
    } else {
      if (!emitEnvCoordOp(JSOp::GetAliasedVar, loc.environmentCoordinate())) {
        return false;
      }
    }
  }

  // Emit the lexical check.
  if (loc.kind() == NameLocation::Kind::FrameSlot) {
    if (!emitLocalOp(JSOp::CheckLexical, loc.frameSlot())) {
      return false;
    }
  } else {
    if (!emitEnvCoordOp(JSOp::CheckAliasedLexical,
                        loc.environmentCoordinate())) {
      return false;
    }
  }

  // Pop the value if needed.
  if (isOnStack == ValueIsOnStack::No) {
    if (!emit1(JSOp::Pop)) {
      return false;
    }
  }

  return innermostTDZCheckCache->noteTDZCheck(this, name, DontCheckTDZ);
}

bool BytecodeEmitter::emitPropLHS(PropertyAccess* prop) {
  MOZ_ASSERT(!prop->isSuper());

  ParseNode* expr = &prop->expression();

  if (!expr->is<PropertyAccess>() || expr->as<PropertyAccess>().isSuper()) {
    // The non-optimized case.
    return emitTree(expr);
  }

  // If the object operand is also a dotted property reference, reverse the
  // list linked via expression() temporarily so we can iterate over it from
  // the bottom up (reversing again as we go), to avoid excessive recursion.
  PropertyAccess* pndot = &expr->as<PropertyAccess>();
  ParseNode* pnup = nullptr;
  ParseNode* pndown;
  for (;;) {
    // Reverse pndot->expression() to point up, not down.
    pndown = &pndot->expression();
    pndot->setExpression(pnup);
    if (!pndown->is<PropertyAccess>() ||
        pndown->as<PropertyAccess>().isSuper()) {
      break;
    }
    pnup = pndot;
    pndot = &pndown->as<PropertyAccess>();
  }

  // pndown is a primary expression, not a dotted property reference.
  if (!emitTree(pndown)) {
    return false;
  }

  while (true) {
    // Walk back up the list, emitting annotated name ops.
    if (!emitAtomOp(JSOp::GetProp, pndot->key().atom())) {
      return false;
    }

    // Reverse the pndot->expression() link again.
    pnup = pndot->maybeExpression();
    pndot->setExpression(pndown);
    pndown = pndot;
    if (!pnup) {
      break;
    }
    pndot = &pnup->as<PropertyAccess>();
  }
  return true;
}

bool BytecodeEmitter::emitPropIncDec(UnaryNode* incDec, ValueUsage valueUsage) {
  PropertyAccess* prop = &incDec->kid()->as<PropertyAccess>();
  bool isSuper = prop->isSuper();
  ParseNodeKind kind = incDec->getKind();
  PropOpEmitter poe(
      this,
      kind == ParseNodeKind::PostIncrementExpr
          ? PropOpEmitter::Kind::PostIncrement
      : kind == ParseNodeKind::PreIncrementExpr
          ? PropOpEmitter::Kind::PreIncrement
      : kind == ParseNodeKind::PostDecrementExpr
          ? PropOpEmitter::Kind::PostDecrement
          : PropOpEmitter::Kind::PreDecrement,
      isSuper ? PropOpEmitter::ObjKind::Super : PropOpEmitter::ObjKind::Other);
  if (!poe.prepareForObj()) {
    return false;
  }
  if (isSuper) {
    UnaryNode* base = &prop->expression().as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] THIS
      return false;
    }
  } else {
    if (!emitPropLHS(prop)) {
      //            [stack] OBJ
      return false;
    }
  }
  if (!poe.emitIncDec(prop->key().atom(), valueUsage)) {
    //              [stack] RESULT
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitNameIncDec(UnaryNode* incDec, ValueUsage valueUsage) {
  MOZ_ASSERT(incDec->kid()->isKind(ParseNodeKind::Name));

  ParseNodeKind kind = incDec->getKind();
  NameNode* name = &incDec->kid()->as<NameNode>();
  NameOpEmitter noe(this, name->atom(),
                    kind == ParseNodeKind::PostIncrementExpr
                        ? NameOpEmitter::Kind::PostIncrement
                    : kind == ParseNodeKind::PreIncrementExpr
                        ? NameOpEmitter::Kind::PreIncrement
                    : kind == ParseNodeKind::PostDecrementExpr
                        ? NameOpEmitter::Kind::PostDecrement
                        : NameOpEmitter::Kind::PreDecrement);
  if (!noe.emitIncDec(valueUsage)) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitObjAndKey(ParseNode* exprOrSuper, ParseNode* key,
                                    ElemOpEmitter& eoe) {
  if (exprOrSuper->isKind(ParseNodeKind::SuperBase)) {
    if (!eoe.prepareForObj()) {
      //            [stack]
      return false;
    }
    UnaryNode* base = &exprOrSuper->as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] THIS
      return false;
    }
    if (!eoe.prepareForKey()) {
      //            [stack] THIS
      return false;
    }
    if (!emitTree(key)) {
      //            [stack] THIS KEY
      return false;
    }

    return true;
  }

  if (!eoe.prepareForObj()) {
    //              [stack]
    return false;
  }
  if (!emitTree(exprOrSuper)) {
    //              [stack] OBJ
    return false;
  }
  if (!eoe.prepareForKey()) {
    //              [stack] OBJ? OBJ
    return false;
  }
  if (!emitTree(key)) {
    //              [stack] OBJ? OBJ KEY
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitElemOpBase(JSOp op) {
  if (!emit1(op)) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitElemObjAndKey(PropertyByValue* elem, bool isSuper,
                                        ElemOpEmitter& eoe) {
  MOZ_ASSERT(isSuper == elem->expression().isKind(ParseNodeKind::SuperBase));
  return emitObjAndKey(&elem->expression(), &elem->key(), eoe);
}

static ElemOpEmitter::Kind ConvertIncDecKind(ParseNodeKind kind) {
  switch (kind) {
    case ParseNodeKind::PostIncrementExpr:
      return ElemOpEmitter::Kind::PostIncrement;
    case ParseNodeKind::PreIncrementExpr:
      return ElemOpEmitter::Kind::PreIncrement;
    case ParseNodeKind::PostDecrementExpr:
      return ElemOpEmitter::Kind::PostDecrement;
    case ParseNodeKind::PreDecrementExpr:
      return ElemOpEmitter::Kind::PreDecrement;
    default:
      MOZ_CRASH("unexpected inc/dec node kind");
  }
}

static PrivateOpEmitter::Kind PrivateConvertIncDecKind(ParseNodeKind kind) {
  switch (kind) {
    case ParseNodeKind::PostIncrementExpr:
      return PrivateOpEmitter::Kind::PostIncrement;
    case ParseNodeKind::PreIncrementExpr:
      return PrivateOpEmitter::Kind::PreIncrement;
    case ParseNodeKind::PostDecrementExpr:
      return PrivateOpEmitter::Kind::PostDecrement;
    case ParseNodeKind::PreDecrementExpr:
      return PrivateOpEmitter::Kind::PreDecrement;
    default:
      MOZ_CRASH("unexpected inc/dec node kind");
  }
}

bool BytecodeEmitter::emitElemIncDec(UnaryNode* incDec, ValueUsage valueUsage) {
  PropertyByValue* elemExpr = &incDec->kid()->as<PropertyByValue>();
  bool isSuper = elemExpr->isSuper();
  MOZ_ASSERT(!elemExpr->key().isKind(ParseNodeKind::PrivateName));
  ParseNodeKind kind = incDec->getKind();
  ElemOpEmitter eoe(
      this, ConvertIncDecKind(kind),
      isSuper ? ElemOpEmitter::ObjKind::Super : ElemOpEmitter::ObjKind::Other);
  if (!emitElemObjAndKey(elemExpr, isSuper, eoe)) {
    //              [stack] # if Super
    //              [stack] THIS KEY
    //              [stack] # otherwise
    //              [stack] OBJ KEY
    return false;
  }
  if (!eoe.emitIncDec(valueUsage)) {
    //              [stack] RESULT
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitCallIncDec(UnaryNode* incDec) {
  MOZ_ASSERT(incDec->isKind(ParseNodeKind::PreIncrementExpr) ||
             incDec->isKind(ParseNodeKind::PostIncrementExpr) ||
             incDec->isKind(ParseNodeKind::PreDecrementExpr) ||
             incDec->isKind(ParseNodeKind::PostDecrementExpr));

  ParseNode* call = incDec->kid();
  MOZ_ASSERT(call->isKind(ParseNodeKind::CallExpr));
  if (!emitTree(call)) {
    //              [stack] CALLRESULT
    return false;
  }
  if (!emit1(JSOp::ToNumeric)) {
    //              [stack] N
    return false;
  }

  // The increment/decrement has no side effects, so proceed to throw for
  // invalid assignment target.
  return emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::AssignToCall));
}

bool BytecodeEmitter::emitPrivateIncDec(UnaryNode* incDec,
                                        ValueUsage valueUsage) {
  PrivateMemberAccess* privateExpr = &incDec->kid()->as<PrivateMemberAccess>();
  ParseNodeKind kind = incDec->getKind();
  PrivateOpEmitter xoe(this, PrivateConvertIncDecKind(kind),
                       privateExpr->privateName().name());
  if (!emitTree(&privateExpr->expression())) {
    //              [stack] OBJ
    return false;
  }
  if (!xoe.emitReference()) {
    //              [stack] OBJ NAME
    return false;
  }
  if (!xoe.emitIncDec(valueUsage)) {
    //              [stack] RESULT
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDouble(double d) {
  BytecodeOffset offset;
  if (!emitCheck(JSOp::Double, 9, &offset)) {
    return false;
  }

  jsbytecode* code = bytecodeSection().code(offset);
  code[0] = jsbytecode(JSOp::Double);
  SET_INLINE_VALUE(code, DoubleValue(d));
  bytecodeSection().updateDepth(JSOp::Double, offset);
  return true;
}

bool BytecodeEmitter::emitNumberOp(double dval) {
  int32_t ival;
  if (NumberIsInt32(dval, &ival)) {
    if (ival == 0) {
      return emit1(JSOp::Zero);
    }
    if (ival == 1) {
      return emit1(JSOp::One);
    }
    if ((int)(int8_t)ival == ival) {
      return emit2(JSOp::Int8, uint8_t(int8_t(ival)));
    }

    uint32_t u = uint32_t(ival);
    if (u < Bit(16)) {
      if (!emitUint16Operand(JSOp::Uint16, u)) {
        return false;
      }
    } else if (u < Bit(24)) {
      BytecodeOffset off;
      if (!emitN(JSOp::Uint24, 3, &off)) {
        return false;
      }
      SET_UINT24(bytecodeSection().code(off), u);
    } else {
      BytecodeOffset off;
      if (!emitN(JSOp::Int32, 4, &off)) {
        return false;
      }
      SET_INT32(bytecodeSection().code(off), ival);
    }
    return true;
  }

  return emitDouble(dval);
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047.
 * LLVM is deciding to inline this function which uses a lot of stack space
 * into emitTree which is recursive and uses relatively little stack space.
 */
MOZ_NEVER_INLINE bool BytecodeEmitter::emitSwitch(SwitchStatement* switchStmt) {
  LexicalScopeNode& lexical = switchStmt->lexicalForCaseList();
  MOZ_ASSERT(lexical.isKind(ParseNodeKind::LexicalScope));
  ListNode* cases = &lexical.scopeBody()->as<ListNode>();
  MOZ_ASSERT(cases->isKind(ParseNodeKind::StatementList));

  SwitchEmitter se(this);
  if (!se.emitDiscriminant(switchStmt->discriminant().pn_pos.begin)) {
    return false;
  }

  if (!markStepBreakpoint()) {
    return false;
  }
  if (!emitTree(&switchStmt->discriminant())) {
    return false;
  }

  // Enter the scope before pushing the switch BreakableControl since all
  // breaks are under this scope.

  if (!lexical.isEmptyScope()) {
    if (!se.emitLexical(lexical.scopeBindings())) {
      return false;
    }

    // A switch statement may contain hoisted functions inside its
    // cases. The PNX_FUNCDEFS flag is propagated from the STATEMENTLIST
    // bodies of the cases to the case list.
    if (cases->hasTopLevelFunctionDeclarations()) {
      for (ParseNode* item : cases->contents()) {
        CaseClause* caseClause = &item->as<CaseClause>();
        ListNode* statements = caseClause->statementList();
        if (statements->hasTopLevelFunctionDeclarations()) {
          if (!emitHoistedFunctionsInList(statements)) {
            return false;
          }
        }
      }
    }
  } else {
    MOZ_ASSERT(!cases->hasTopLevelFunctionDeclarations());
  }

  SwitchEmitter::TableGenerator tableGen(this);
  uint32_t caseCount = cases->count() - (switchStmt->hasDefault() ? 1 : 0);
  if (caseCount == 0) {
    tableGen.finish(0);
  } else {
    for (ParseNode* item : cases->contents()) {
      CaseClause* caseClause = &item->as<CaseClause>();
      if (caseClause->isDefault()) {
        continue;
      }

      ParseNode* caseValue = caseClause->caseExpression();

      if (caseValue->getKind() != ParseNodeKind::NumberExpr) {
        tableGen.setInvalid();
        break;
      }

      int32_t i;
      if (!NumberEqualsInt32(caseValue->as<NumericLiteral>().value(), &i)) {
        tableGen.setInvalid();
        break;
      }

      if (!tableGen.addNumber(i)) {
        return false;
      }
    }

    tableGen.finish(caseCount);
  }

  if (!se.validateCaseCount(caseCount)) {
    return false;
  }

  bool isTableSwitch = tableGen.isValid();
  if (isTableSwitch) {
    if (!se.emitTable(tableGen)) {
      return false;
    }
  } else {
    if (!se.emitCond()) {
      return false;
    }

    // Emit code for evaluating cases and jumping to case statements.
    for (ParseNode* item : cases->contents()) {
      CaseClause* caseClause = &item->as<CaseClause>();
      if (caseClause->isDefault()) {
        continue;
      }

      if (!se.prepareForCaseValue()) {
        return false;
      }

      ParseNode* caseValue = caseClause->caseExpression();
      // If the expression is a literal, suppress line number emission so
      // that debugging works more naturally.
      if (!emitTree(
              caseValue, ValueUsage::WantValue,
              caseValue->isLiteral() ? SUPPRESS_LINENOTE : EMIT_LINENOTE)) {
        return false;
      }

      if (!se.emitCaseJump()) {
        return false;
      }
    }
  }

  // Emit code for each case's statements.
  for (ParseNode* item : cases->contents()) {
    CaseClause* caseClause = &item->as<CaseClause>();
    if (caseClause->isDefault()) {
      if (!se.emitDefaultBody()) {
        return false;
      }
    } else {
      if (isTableSwitch) {
        ParseNode* caseValue = caseClause->caseExpression();
        MOZ_ASSERT(caseValue->isKind(ParseNodeKind::NumberExpr));

        NumericLiteral* literal = &caseValue->as<NumericLiteral>();
#ifdef DEBUG
        // Use NumberEqualsInt32 here because switches compare using
        // strict equality, which will equate -0 and +0.  In contrast
        // NumberIsInt32 would return false for -0.
        int32_t v;
        MOZ_ASSERT(mozilla::NumberEqualsInt32(literal->value(), &v));
#endif
        int32_t i = int32_t(literal->value());

        if (!se.emitCaseBody(i, tableGen)) {
          return false;
        }
      } else {
        if (!se.emitCaseBody()) {
          return false;
        }
      }
    }

    if (!emitTree(caseClause->statementList())) {
      return false;
    }
  }

  if (!se.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::allocateResumeIndex(BytecodeOffset offset,
                                          uint32_t* resumeIndex) {
  static constexpr uint32_t MaxResumeIndex = BitMask(24);

  static_assert(
      MaxResumeIndex < uint32_t(AbstractGeneratorObject::RESUME_INDEX_RUNNING),
      "resumeIndex should not include magic AbstractGeneratorObject "
      "resumeIndex values");
  static_assert(
      MaxResumeIndex <= INT32_MAX / sizeof(uintptr_t),
      "resumeIndex * sizeof(uintptr_t) must fit in an int32. JIT code relies "
      "on this when loading resume entries from BaselineScript");

  *resumeIndex = bytecodeSection().resumeOffsetList().length();
  if (*resumeIndex > MaxResumeIndex) {
    reportError(nullptr, JSMSG_TOO_MANY_RESUME_INDEXES);
    return false;
  }

  return bytecodeSection().resumeOffsetList().append(offset.value());
}

bool BytecodeEmitter::allocateResumeIndexRange(
    mozilla::Span<BytecodeOffset> offsets, uint32_t* firstResumeIndex) {
  *firstResumeIndex = 0;

  for (size_t i = 0, len = offsets.size(); i < len; i++) {
    uint32_t resumeIndex;
    if (!allocateResumeIndex(offsets[i], &resumeIndex)) {
      return false;
    }
    if (i == 0) {
      *firstResumeIndex = resumeIndex;
    }
  }

  return true;
}

bool BytecodeEmitter::emitYieldOp(JSOp op) {
  if (op == JSOp::FinalYieldRval) {
    return emit1(JSOp::FinalYieldRval);
  }

  MOZ_ASSERT(op == JSOp::InitialYield || op == JSOp::Yield ||
             op == JSOp::Await);

  BytecodeOffset off;
  if (!emitN(op, 3, &off)) {
    return false;
  }

  if (op == JSOp::InitialYield || op == JSOp::Yield) {
    bytecodeSection().addNumYields();
  }

  uint32_t resumeIndex;
  if (!allocateResumeIndex(bytecodeSection().offset(), &resumeIndex)) {
    return false;
  }

  SET_RESUMEINDEX(bytecodeSection().code(off), resumeIndex);

  BytecodeOffset unusedOffset;
  return emitJumpTargetOp(JSOp::AfterYield, &unusedOffset);
}

bool BytecodeEmitter::emitPushResumeKind(GeneratorResumeKind kind) {
  return emit2(JSOp::ResumeKind, uint8_t(kind));
}

bool BytecodeEmitter::emitSetThis(BinaryNode* setThisNode) {
  // ParseNodeKind::SetThis is used to update |this| after a super() call
  // in a derived class constructor.

  MOZ_ASSERT(setThisNode->isKind(ParseNodeKind::SetThis));
  MOZ_ASSERT(setThisNode->left()->isKind(ParseNodeKind::Name));

  auto name = setThisNode->left()->as<NameNode>().name();

  // The 'this' binding is not lexical, but due to super() semantics this
  // initialization needs to be treated as a lexical one.
  NameLocation loc = lookupName(name);
  NameLocation lexicalLoc;
  if (loc.kind() == NameLocation::Kind::FrameSlot) {
    lexicalLoc = NameLocation::FrameSlot(BindingKind::Let, loc.frameSlot());
  } else if (loc.kind() == NameLocation::Kind::EnvironmentCoordinate) {
    EnvironmentCoordinate coord = loc.environmentCoordinate();
    uint8_t hops = AssertedCast<uint8_t>(coord.hops());
    lexicalLoc = NameLocation::EnvironmentCoordinate(BindingKind::Let, hops,
                                                     coord.slot());
  } else {
    MOZ_ASSERT(loc.kind() == NameLocation::Kind::Dynamic);
    lexicalLoc = loc;
  }

  NameOpEmitter noe(this, name, lexicalLoc, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    //              [stack]
    return false;
  }

  // Emit the new |this| value.
  if (!emitTree(setThisNode->right())) {
    //              [stack] NEWTHIS
    return false;
  }

  // Get the original |this| and throw if we already initialized
  // it. Do *not* use the NameLocation argument, as that's the special
  // lexical location below to deal with super() semantics.
  if (!emitGetName(name)) {
    //              [stack] NEWTHIS THIS
    return false;
  }
  if (!emit1(JSOp::CheckThisReinit)) {
    //              [stack] NEWTHIS THIS
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack] NEWTHIS
    return false;
  }
  if (!noe.emitAssignment()) {
    //              [stack] NEWTHIS
    return false;
  }

  if (!emitInitializeInstanceMembers(true)) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::defineHoistedTopLevelFunctions(ParseNode* body) {
  MOZ_ASSERT(inPrologue());
  MOZ_ASSERT(sc->isGlobalContext() || (sc->isEvalContext() && !sc->strict()));
  MOZ_ASSERT(body->is<LexicalScopeNode>() || body->is<ListNode>());

  if (body->is<LexicalScopeNode>()) {
    body = body->as<LexicalScopeNode>().scopeBody();
    MOZ_ASSERT(body->is<ListNode>());
  }

  if (!body->as<ListNode>().hasTopLevelFunctionDeclarations()) {
    return true;
  }

  return emitHoistedFunctionsInList(&body->as<ListNode>());
}

// For Global and sloppy-Eval scripts, this performs most of the steps of the
// spec's [GlobalDeclarationInstantiation] and [EvalDeclarationInstantiation]
// operations.
//
// Note that while strict-Eval is handled in the same part of the spec, it never
// fails for global-redeclaration checks so those scripts initialize directly in
// their bytecode.
bool BytecodeEmitter::emitDeclarationInstantiation(ParseNode* body) {
  if (sc->isModuleContext()) {
    // ES Modules have dedicated variable and lexial environments and therefore
    // do not have to perform redeclaration checks. We initialize their bindings
    // elsewhere in bytecode.
    return true;
  }

  if (sc->isEvalContext() && sc->strict()) {
    // Strict Eval has a dedicated variables (and lexical) environment and
    // therefore does not have to perform redeclaration checks. We initialize
    // their bindings elsewhere in the bytecode.
    return true;
  }

  // If we have no variables bindings, then we are done!
  if (sc->isGlobalContext()) {
    if (!sc->asGlobalContext()->bindings) {
      return true;
    }
  } else {
    MOZ_ASSERT(sc->isEvalContext());

    if (!sc->asEvalContext()->bindings) {
      return true;
    }
  }

#if DEBUG
  // There should be no emitted functions yet.
  for (const auto& thing : perScriptData().gcThingList().objects()) {
    MOZ_ASSERT(thing.isEmptyGlobalScope() || thing.isScope());
  }
#endif

  // Emit the hoisted functions to gc-things list. There is no bytecode
  // generated yet to bind them.
  if (!defineHoistedTopLevelFunctions(body)) {
    return false;
  }

  // Save the last GCThingIndex emitted. The hoisted functions are contained in
  // the gc-things list up until this point. This set of gc-things also contain
  // initial scopes (of which there must be at least one).
  MOZ_ASSERT(perScriptData().gcThingList().length() > 0);
  GCThingIndex lastFun =
      GCThingIndex(perScriptData().gcThingList().length() - 1);

#if DEBUG
  for (const auto& thing : perScriptData().gcThingList().objects()) {
    MOZ_ASSERT(thing.isEmptyGlobalScope() || thing.isScope() ||
               thing.isFunction());
  }
#endif

  // Check for declaration conflicts and initialize the bindings.
  // NOTE: The self-hosting top-level script should not populate the builtins
  //       directly on the GlobalObject (and instead uses JSOp::GetIntrinsic for
  //       lookups).
  if (emitterMode == BytecodeEmitter::EmitterMode::Normal) {
    if (!emitGCIndexOp(JSOp::GlobalOrEvalDeclInstantiation, lastFun)) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitScript(ParseNode* body) {
  setScriptStartOffsetIfUnset(body->pn_pos.begin);

  MOZ_ASSERT(inPrologue());

  TDZCheckCache tdzCache(this);
  EmitterScope emitterScope(this);
  Maybe<AsyncEmitter> topLevelAwait;
  if (sc->isGlobalContext()) {
    if (!emitterScope.enterGlobal(this, sc->asGlobalContext())) {
      return false;
    }
  } else if (sc->isEvalContext()) {
    if (!emitterScope.enterEval(this, sc->asEvalContext())) {
      return false;
    }
  } else {
    MOZ_ASSERT(sc->isModuleContext());
    if (!emitterScope.enterModule(this, sc->asModuleContext())) {
      return false;
    }
    if (sc->asModuleContext()->isAsync()) {
      topLevelAwait.emplace(this);
    }
  }

  setFunctionBodyEndPos(body->pn_pos.end);

  bool isSloppyEval = sc->isEvalContext() && !sc->strict();
  if (isSloppyEval && body->is<LexicalScopeNode>() &&
      !body->as<LexicalScopeNode>().isEmptyScope()) {
    // Sloppy eval scripts may emit hoisted functions bindings with a
    // `JSOp::GlobalOrEvalDeclInstantiation` opcode below. If this eval needs a
    // top-level lexical environment, we must ensure that environment is created
    // before those functions are created and bound.
    //
    // This differs from the global-script case below because the global-lexical
    // environment exists outside the script itself. In the case of strict eval
    // scripts, the `emitterScope` above is already sufficient.
    EmitterScope lexicalEmitterScope(this);
    LexicalScopeNode* scope = &body->as<LexicalScopeNode>();

    if (!lexicalEmitterScope.enterLexical(this, ScopeKind::Lexical,
                                          scope->scopeBindings())) {
      return false;
    }

    if (!emitDeclarationInstantiation(scope->scopeBody())) {
      return false;
    }

    switchToMain();

    ParseNode* scopeBody = scope->scopeBody();
    if (!emitLexicalScopeBody(scopeBody)) {
      return false;
    }

    if (!updateSourceCoordNotes(scopeBody->pn_pos.end)) {
      return false;
    }

    if (!lexicalEmitterScope.leave(this)) {
      return false;
    }
  } else {
    if (!emitDeclarationInstantiation(body)) {
      return false;
    }
    if (topLevelAwait) {
      if (!topLevelAwait->prepareForModule()) {
        return false;
      }
    }

    switchToMain();

    if (topLevelAwait) {
      if (!topLevelAwait->prepareForBody()) {
        return false;
      }
    }

    if (!emitTree(body)) {
      //            [stack]
      return false;
    }

    if (!updateSourceCoordNotes(body->pn_pos.end)) {
      return false;
    }
  }

  if (topLevelAwait) {
    if (!topLevelAwait->emitEndModule()) {
      return false;
    }
  }

  if (!markSimpleBreakpoint()) {
    return false;
  }

  if (!emitReturnRval()) {
    return false;
  }

  if (!emitterScope.leave(this)) {
    return false;
  }

  if (!NameFunctions(fc, parserAtoms(), body)) {
    return false;
  }

  // Create a Stencil and convert it into a JSScript.
  return intoScriptStencil(CompilationStencil::TopLevelIndex);
}

js::UniquePtr<ImmutableScriptData>
BytecodeEmitter::createImmutableScriptData() {
  uint32_t nslots;
  if (!getNslots(&nslots)) {
    return nullptr;
  }

  bool isFunction = sc->isFunctionBox();
  uint16_t funLength = isFunction ? sc->asFunctionBox()->length() : 0;

  mozilla::SaturateUint8 propertyCountEstimate = propertyAdditionEstimate;

  // Add fields to the property count estimate.
  if (isFunction && sc->asFunctionBox()->useMemberInitializers()) {
    propertyCountEstimate +=
        sc->asFunctionBox()->memberInitializers().numMemberInitializers;
  }

  return ImmutableScriptData::new_(
      fc, mainOffset(), maxFixedSlots, nslots, bodyScopeIndex,
      bytecodeSection().numICEntries(), isFunction, funLength,
      propertyCountEstimate.value(), bytecodeSection().code(),
      bytecodeSection().notes(), bytecodeSection().resumeOffsetList().span(),
      bytecodeSection().scopeNoteList().span(),
      bytecodeSection().tryNoteList().span());
}

bool BytecodeEmitter::getNslots(uint32_t* nslots) {
  uint64_t nslots64 =
      maxFixedSlots + static_cast<uint64_t>(bytecodeSection().maxStackDepth());
  if (nslots64 > UINT32_MAX) {
    reportError(nullptr, JSMSG_NEED_DIET, js_script_str);
    return false;
  }
  *nslots = nslots64;
  return true;
}

bool BytecodeEmitter::emitFunctionScript(FunctionNode* funNode) {
  MOZ_ASSERT(inPrologue());
  ParamsBodyNode* paramsBody = funNode->body();
  FunctionBox* funbox = sc->asFunctionBox();

  setScriptStartOffsetIfUnset(paramsBody->pn_pos.begin);

  //                [stack]

  FunctionScriptEmitter fse(this, funbox, Some(paramsBody->pn_pos.begin),
                            Some(paramsBody->pn_pos.end));
  if (!fse.prepareForParameters()) {
    //              [stack]
    return false;
  }

  if (!emitFunctionFormalParameters(paramsBody)) {
    //              [stack]
    return false;
  }

  if (!fse.prepareForBody()) {
    //              [stack]
    return false;
  }

  if (!emitTree(paramsBody->body())) {
    //              [stack]
    return false;
  }

  if (!fse.emitEndBody()) {
    //              [stack]
    return false;
  }

  if (funbox->index() == CompilationStencil::TopLevelIndex) {
    if (!NameFunctions(fc, parserAtoms(), funNode)) {
      return false;
    }
  }

  return fse.intoStencil();
}

bool BytecodeEmitter::emitDestructuringLHSRef(ParseNode* target,
                                              size_t* emitted) {
#ifdef DEBUG
  int depth = bytecodeSection().stackDepth();
#endif

  switch (target->getKind()) {
    case ParseNodeKind::Name:
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      // No need to recurse into ParseNodeKind::Array and ParseNodeKind::Object
      // subpatterns here, since emitSetOrInitializeDestructuring does the
      // recursion when setting or initializing the value. Getting reference
      // doesn't recurse.
      *emitted = 0;
      break;

    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &target->as<PropertyAccess>();
      bool isSuper = prop->isSuper();
      PropOpEmitter poe(this, PropOpEmitter::Kind::SimpleAssignment,
                        isSuper ? PropOpEmitter::ObjKind::Super
                                : PropOpEmitter::ObjKind::Other);
      if (!poe.prepareForObj()) {
        return false;
      }
      if (isSuper) {
        UnaryNode* base = &prop->expression().as<UnaryNode>();
        if (!emitGetThisForSuperBase(base)) {
          //        [stack] THIS SUPERBASE
          return false;
        }
      } else {
        if (!emitTree(&prop->expression())) {
          //        [stack] OBJ
          return false;
        }
      }
      if (!poe.prepareForRhs()) {
        //          [stack] # if Super
        //          [stack] THIS SUPERBASE
        //          [stack] # otherwise
        //          [stack] OBJ
        return false;
      }

      // SUPERBASE was pushed onto THIS in poe.prepareForRhs above.
      *emitted = 1 + isSuper;
      break;
    }

    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &target->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter eoe(this, ElemOpEmitter::Kind::SimpleAssignment,
                        isSuper ? ElemOpEmitter::ObjKind::Super
                                : ElemOpEmitter::ObjKind::Other);
      if (!emitElemObjAndKey(elem, isSuper, eoe)) {
        //          [stack] # if Super
        //          [stack] THIS KEY
        //          [stack] # otherwise
        //          [stack] OBJ KEY
        return false;
      }
      if (!eoe.prepareForRhs()) {
        //          [stack] # if Super
        //          [stack] THIS KEY SUPERBASE
        //          [stack] # otherwise
        //          [stack] OBJ KEY
        return false;
      }

      // SUPERBASE was pushed onto KEY in eoe.prepareForRhs above.
      *emitted = 2 + isSuper;
      break;
    }

    case ParseNodeKind::PrivateMemberExpr: {
      PrivateMemberAccess* privateExpr = &target->as<PrivateMemberAccess>();
      PrivateOpEmitter xoe(this, PrivateOpEmitter::Kind::SimpleAssignment,
                           privateExpr->privateName().name());
      if (!emitTree(&privateExpr->expression())) {
        //          [stack] OBJ
        return false;
      }
      if (!xoe.emitReference()) {
        //          [stack] OBJ NAME
        return false;
      }
      *emitted = xoe.numReferenceSlots();
      break;
    }

    case ParseNodeKind::CallExpr:
      MOZ_ASSERT_UNREACHABLE(
          "Parser::reportIfNotValidSimpleAssignmentTarget "
          "rejects function calls as assignment "
          "targets in destructuring assignments");
      break;

    default:
      MOZ_CRASH("emitDestructuringLHSRef: bad lhs kind");
  }

  MOZ_ASSERT(bytecodeSection().stackDepth() == depth + int(*emitted));

  return true;
}

bool BytecodeEmitter::emitSetOrInitializeDestructuring(
    ParseNode* target, DestructuringFlavor flav) {
  // Now emit the lvalue opcode sequence. If the lvalue is a nested
  // destructuring initialiser-form, call ourselves to handle it, then pop
  // the matched value. Otherwise emit an lvalue bytecode sequence followed
  // by an assignment op.

  switch (target->getKind()) {
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      if (!emitDestructuringOps(&target->as<ListNode>(), flav)) {
        return false;
      }
      // emitDestructuringOps leaves the assigned (to-be-destructured) value on
      // top of the stack.
      break;

    case ParseNodeKind::Name: {
      auto name = target->as<NameNode>().name();
      NameLocation loc = lookupName(name);
      NameOpEmitter::Kind kind;
      switch (flav) {
        case DestructuringFlavor::Declaration:
          kind = NameOpEmitter::Kind::Initialize;
          break;

        case DestructuringFlavor::Assignment:
          kind = NameOpEmitter::Kind::SimpleAssignment;
          break;
      }

      NameOpEmitter noe(this, name, loc, kind);
      if (!noe.prepareForRhs()) {
        //          [stack] V ENV?
        return false;
      }
      if (noe.emittedBindOp()) {
        // This is like ordinary assignment, but with one difference.
        //
        // In `a = b`, we first determine a binding for `a` (using
        // JSOp::BindName or JSOp::BindGName), then we evaluate `b`, then
        // a JSOp::SetName instruction.
        //
        // In `[a] = [b]`, per spec, `b` is evaluated first, then we
        // determine a binding for `a`. Then we need to do assignment--
        // but the operands are on the stack in the wrong order for
        // JSOp::SetProp, so we have to add a JSOp::Swap.
        //
        // In the cases where we are emitting a name op, emit a swap
        // because of this.
        if (!emit1(JSOp::Swap)) {
          //        [stack] ENV V
          return false;
        }
      } else {
        // In cases of emitting a frame slot or environment slot,
        // nothing needs be done.
      }
      if (!noe.emitAssignment()) {
        //          [stack] V
        return false;
      }

      break;
    }

    case ParseNodeKind::DotExpr: {
      // The reference is already pushed by emitDestructuringLHSRef.
      //            [stack] # if Super
      //            [stack] THIS SUPERBASE VAL
      //            [stack] # otherwise
      //            [stack] OBJ VAL
      PropertyAccess* prop = &target->as<PropertyAccess>();
      bool isSuper = prop->isSuper();
      PropOpEmitter poe(this, PropOpEmitter::Kind::SimpleAssignment,
                        isSuper ? PropOpEmitter::ObjKind::Super
                                : PropOpEmitter::ObjKind::Other);
      if (!poe.skipObjAndRhs()) {
        return false;
      }
      //            [stack] # VAL
      if (!poe.emitAssignment(prop->key().atom())) {
        return false;
      }
      break;
    }

    case ParseNodeKind::ElemExpr: {
      // The reference is already pushed by emitDestructuringLHSRef.
      //            [stack] # if Super
      //            [stack] THIS KEY SUPERBASE VAL
      //            [stack] # otherwise
      //            [stack] OBJ KEY VAL
      PropertyByValue* elem = &target->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter eoe(this, ElemOpEmitter::Kind::SimpleAssignment,
                        isSuper ? ElemOpEmitter::ObjKind::Super
                                : ElemOpEmitter::ObjKind::Other);
      if (!eoe.skipObjAndKeyAndRhs()) {
        return false;
      }
      if (!eoe.emitAssignment()) {
        //          [stack] VAL
        return false;
      }
      break;
    }

    case ParseNodeKind::PrivateMemberExpr: {
      // The reference is already pushed by emitDestructuringLHSRef.
      //            [stack] OBJ NAME VAL
      PrivateMemberAccess* privateExpr = &target->as<PrivateMemberAccess>();
      PrivateOpEmitter xoe(this, PrivateOpEmitter::Kind::SimpleAssignment,
                           privateExpr->privateName().name());
      if (!xoe.skipReference()) {
        return false;
      }
      if (!xoe.emitAssignment()) {
        //          [stack] VAL
        return false;
      }
      break;
    }

    case ParseNodeKind::CallExpr:
      MOZ_ASSERT_UNREACHABLE(
          "Parser::reportIfNotValidSimpleAssignmentTarget "
          "rejects function calls as assignment "
          "targets in destructuring assignments");
      break;

    default:
      MOZ_CRASH("emitSetOrInitializeDestructuring: bad lhs kind");
  }

  // Pop the assigned value.
  if (!emit1(JSOp::Pop)) {
    //              [stack] # empty
    return false;
  }

  return true;
}

JSOp BytecodeEmitter::getIterCallOp(JSOp callOp,
                                    SelfHostedIter selfHostedIter) {
  if (emitterMode == BytecodeEmitter::SelfHosting) {
    MOZ_ASSERT(selfHostedIter == SelfHostedIter::Allow);

    switch (callOp) {
      case JSOp::Call:
        return JSOp::CallContent;
      case JSOp::CallIter:
        return JSOp::CallContentIter;
      default:
        MOZ_CRASH("Unknown iterator call op");
    }
  }

  return callOp;
}

bool BytecodeEmitter::emitIteratorNext(
    const Maybe<uint32_t>& callSourceCoordOffset,
    IteratorKind iterKind /* = IteratorKind::Sync */,
    SelfHostedIter selfHostedIter /* = SelfHostedIter::Deny */) {
  MOZ_ASSERT(selfHostedIter == SelfHostedIter::Allow ||
                 emitterMode != BytecodeEmitter::SelfHosting,
             ".next() iteration is prohibited in self-hosted code because it"
             "can run user-modifiable iteration code");

  //                [stack] ... NEXT ITER
  MOZ_ASSERT(bytecodeSection().stackDepth() >= 2);

  if (!emitCall(getIterCallOp(JSOp::Call, selfHostedIter), 0,
                callSourceCoordOffset)) {
    //              [stack] ... RESULT
    return false;
  }

  if (iterKind == IteratorKind::Async) {
    if (!emitAwaitInInnermostScope()) {
      //            [stack] ... RESULT
      return false;
    }
  }

  if (!emitCheckIsObj(CheckIsObjectKind::IteratorNext)) {
    //              [stack] ... RESULT
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitIteratorCloseInScope(
    EmitterScope& currentScope,
    IteratorKind iterKind /* = IteratorKind::Sync */,
    CompletionKind completionKind /* = CompletionKind::Normal */,
    SelfHostedIter selfHostedIter /* = SelfHostedIter::Deny */) {
  MOZ_ASSERT(selfHostedIter == SelfHostedIter::Allow ||
                 emitterMode != BytecodeEmitter::SelfHosting,
             ".close() on iterators is prohibited in self-hosted code because "
             "it can run user-modifiable iteration code");

  if (iterKind == IteratorKind::Sync) {
    return emit2(JSOp::CloseIter, uint8_t(completionKind));
  }

  // Generate inline logic corresponding to IteratorClose (ES2021 7.4.6) and
  // AsyncIteratorClose (ES2021 7.4.7). Steps numbers apply to both operations.
  //
  // Callers need to ensure that the iterator object is at the top of the
  // stack.

  // For non-Throw completions, we emit the equivalent of:
  //
  // var returnMethod = GetMethod(iterator, "return");
  // if (returnMethod !== undefined) {
  //   var innerResult = [Await] Call(returnMethod, iterator);
  //   CheckIsObj(innerResult);
  // }
  //
  // Whereas for Throw completions, we emit:
  //
  // try {
  //   var returnMethod = GetMethod(iterator, "return");
  //   if (returnMethod !== undefined) {
  //     [Await] Call(returnMethod, iterator);
  //   }
  // } catch {}

  Maybe<TryEmitter> tryCatch;

  if (completionKind == CompletionKind::Throw) {
    tryCatch.emplace(this, TryEmitter::Kind::TryCatch,
                     TryEmitter::ControlKind::NonSyntactic);

    if (!tryCatch->emitTry()) {
      //            [stack] ... ITER
      return false;
    }
  }

  if (!emit1(JSOp::Dup)) {
    //              [stack] ... ITER ITER
    return false;
  }

  // Steps 1-2 are assertions, step 3 is implicit.

  // Step 4.
  //
  // Get the "return" method.
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::return_())) {
    //              [stack] ... ITER RET
    return false;
  }

  // Step 5.
  //
  // Do nothing if "return" is undefined or null.
  InternalIfEmitter ifReturnMethodIsDefined(this);
  if (!emit1(JSOp::IsNullOrUndefined)) {
    //              [stack] ... ITER RET NULL-OR-UNDEF
    return false;
  }

  if (!ifReturnMethodIsDefined.emitThenElse(
          IfEmitter::ConditionKind::Negative)) {
    //              [stack] ... ITER RET
    return false;
  }

  // Steps 5.c, 7.
  //
  // Call the "return" method.
  if (!emit1(JSOp::Swap)) {
    //              [stack] ... RET ITER
    return false;
  }

  if (!emitCall(getIterCallOp(JSOp::Call, selfHostedIter), 0)) {
    //              [stack] ... RESULT
    return false;
  }

  // 7.4.7 AsyncIteratorClose, step 5.d.
  if (iterKind == IteratorKind::Async) {
    if (completionKind != CompletionKind::Throw) {
      // Await clobbers rval, so save the current rval.
      if (!emit1(JSOp::GetRval)) {
        //          [stack] ... RESULT RVAL
        return false;
      }
      if (!emit1(JSOp::Swap)) {
        //          [stack] ... RVAL RESULT
        return false;
      }
    }

    if (!emitAwaitInScope(currentScope)) {
      //            [stack] ... RVAL? RESULT
      return false;
    }

    if (completionKind != CompletionKind::Throw) {
      if (!emit1(JSOp::Swap)) {
        //          [stack] ... RESULT RVAL
        return false;
      }
      if (!emit1(JSOp::SetRval)) {
        //          [stack] ... RESULT
        return false;
      }
    }
  }

  // Step 6 (Handled in caller).

  // Step 8.
  if (completionKind != CompletionKind::Throw) {
    // Check that the "return" result is an object.
    if (!emitCheckIsObj(CheckIsObjectKind::IteratorReturn)) {
      //            [stack] ... RESULT
      return false;
    }
  }

  if (!ifReturnMethodIsDefined.emitElse()) {
    //              [stack] ... ITER RET
    return false;
  }

  if (!emit1(JSOp::Pop)) {
    //              [stack] ... ITER
    return false;
  }

  if (!ifReturnMethodIsDefined.emitEnd()) {
    return false;
  }

  if (completionKind == CompletionKind::Throw) {
    if (!tryCatch->emitCatch()) {
      //            [stack] ... ITER EXC
      return false;
    }

    // Just ignore the exception thrown by call and await.
    if (!emit1(JSOp::Pop)) {
      //            [stack] ... ITER
      return false;
    }

    if (!tryCatch->emitEnd()) {
      //            [stack] ... ITER
      return false;
    }
  }

  // Step 9 (Handled in caller).

  return emit1(JSOp::Pop);
  //                [stack] ...
}

template <typename InnerEmitter>
bool BytecodeEmitter::wrapWithDestructuringTryNote(int32_t iterDepth,
                                                   InnerEmitter emitter) {
  MOZ_ASSERT(bytecodeSection().stackDepth() >= iterDepth);

  // Pad a nop at the beginning of the bytecode covered by the trynote so
  // that when unwinding environments, we may unwind to the scope
  // corresponding to the pc *before* the start, in case the first bytecode
  // emitted by |emitter| is the start of an inner scope. See comment above
  // UnwindEnvironmentToTryPc.
  if (!emit1(JSOp::TryDestructuring)) {
    return false;
  }

  BytecodeOffset start = bytecodeSection().offset();
  if (!emitter(this)) {
    return false;
  }
  BytecodeOffset end = bytecodeSection().offset();
  if (start != end) {
    return addTryNote(TryNoteKind::Destructuring, iterDepth, start, end);
  }
  return true;
}

bool BytecodeEmitter::emitDefault(ParseNode* defaultExpr, ParseNode* pattern) {
  //                [stack] VALUE

  DefaultEmitter de(this);
  if (!de.prepareForDefault()) {
    //              [stack]
    return false;
  }
  if (!emitInitializer(defaultExpr, pattern)) {
    //              [stack] DEFAULTVALUE
    return false;
  }
  if (!de.emitEnd()) {
    //              [stack] VALUE/DEFAULTVALUE
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitAnonymousFunctionWithName(
    ParseNode* node, TaggedParserAtomIndex name) {
  MOZ_ASSERT(node->isDirectRHSAnonFunction());

  if (node->is<FunctionNode>()) {
    // Function doesn't have 'name' property at this point.
    // Set function's name at compile time.
    if (!setFunName(node->as<FunctionNode>().funbox(), name)) {
      return false;
    }

    return emitTree(node);
  }

  MOZ_ASSERT(node->is<ClassNode>());

  return emitClass(&node->as<ClassNode>(), ClassNameKind::InferredName, name);
}

bool BytecodeEmitter::emitAnonymousFunctionWithComputedName(
    ParseNode* node, FunctionPrefixKind prefixKind) {
  MOZ_ASSERT(node->isDirectRHSAnonFunction());

  if (node->is<FunctionNode>()) {
    if (!emitTree(node)) {
      //            [stack] NAME FUN
      return false;
    }
    if (!emitDupAt(1)) {
      //            [stack] NAME FUN NAME
      return false;
    }
    if (!emit2(JSOp::SetFunName, uint8_t(prefixKind))) {
      //            [stack] NAME FUN
      return false;
    }
    return true;
  }

  MOZ_ASSERT(node->is<ClassNode>());
  MOZ_ASSERT(prefixKind == FunctionPrefixKind::None);

  return emitClass(&node->as<ClassNode>(), ClassNameKind::ComputedName);
}

bool BytecodeEmitter::setFunName(FunctionBox* funbox,
                                 TaggedParserAtomIndex name) {
  // The inferred name may already be set if this function is an interpreted
  // lazy function and we OOM'ed after we set the inferred name the first
  // time.
  if (funbox->hasInferredName()) {
    MOZ_ASSERT(!funbox->emitBytecode);
    MOZ_ASSERT(funbox->displayAtom() == name);

    return true;
  }

  funbox->setInferredName(name);
  return true;
}

bool BytecodeEmitter::emitInitializer(ParseNode* initializer,
                                      ParseNode* pattern) {
  if (initializer->isDirectRHSAnonFunction()) {
    MOZ_ASSERT(!pattern->isInParens());
    auto name = pattern->as<NameNode>().name();
    if (!emitAnonymousFunctionWithName(initializer, name)) {
      return false;
    }
  } else {
    if (!emitTree(initializer)) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitDestructuringOpsArray(ListNode* pattern,
                                                DestructuringFlavor flav) {
  MOZ_ASSERT(pattern->isKind(ParseNodeKind::ArrayExpr));
  MOZ_ASSERT(bytecodeSection().stackDepth() != 0);

  // Here's pseudo code for |let [a, b, , c=y, ...d] = x;|
  //
  // Lines that are annotated "covered by trynote" mean that upon throwing
  // an exception, IteratorClose is called on iter only if done is false.
  //
  //   let x, y;
  //   let a, b, c, d;
  //   let iter, next, lref, result, done, value; // stack values
  //
  //   iter = x[Symbol.iterator]();
  //   next = iter.next;
  //
  //   // ==== emitted by loop for a ====
  //   lref = GetReference(a);              // covered by trynote
  //
  //   result = Call(next, iter);
  //   done = result.done;
  //
  //   if (done)
  //     value = undefined;
  //   else
  //     value = result.value;
  //
  //   SetOrInitialize(lref, value);        // covered by trynote
  //
  //   // ==== emitted by loop for b ====
  //   lref = GetReference(b);              // covered by trynote
  //
  //   if (done) {
  //     value = undefined;
  //   } else {
  //     result = Call(next, iter);
  //     done = result.done;
  //     if (done)
  //       value = undefined;
  //     else
  //       value = result.value;
  //   }
  //
  //   SetOrInitialize(lref, value);        // covered by trynote
  //
  //   // ==== emitted by loop for elision ====
  //   if (done) {
  //     value = undefined;
  //   } else {
  //     result = Call(next, iter);
  //     done = result.done;
  //     if (done)
  //       value = undefined;
  //     else
  //       value = result.value;
  //   }
  //
  //   // ==== emitted by loop for c ====
  //   lref = GetReference(c);              // covered by trynote
  //
  //   if (done) {
  //     value = undefined;
  //   } else {
  //     result = Call(next, iter);
  //     done = result.done;
  //     if (done)
  //       value = undefined;
  //     else
  //       value = result.value;
  //   }
  //
  //   if (value === undefined)
  //     value = y;                         // covered by trynote
  //
  //   SetOrInitialize(lref, value);        // covered by trynote
  //
  //   // ==== emitted by loop for d ====
  //   lref = GetReference(d);              // covered by trynote
  //
  //   if (done)
  //     value = [];
  //   else
  //     value = [...iter];
  //
  //   SetOrInitialize(lref, value);        // covered by trynote
  //
  //   // === emitted after loop ===
  //   if (!done)
  //      IteratorClose(iter);

  // Use an iterator to destructure the RHS, instead of index lookup. We
  // must leave the *original* value on the stack.
  if (!emit1(JSOp::Dup)) {
    //              [stack] ... OBJ OBJ
    return false;
  }
  if (!emitIterator()) {
    //              [stack] ... OBJ NEXT ITER
    return false;
  }

  // For an empty pattern [], call IteratorClose unconditionally. Nothing
  // else needs to be done.
  if (!pattern->head()) {
    if (!emit1(JSOp::Swap)) {
      //            [stack] ... OBJ ITER NEXT
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      //            [stack] ... OBJ ITER
      return false;
    }

    return emitIteratorCloseInInnermostScope();
    //              [stack] ... OBJ
  }

  // Push an initial FALSE value for DONE.
  if (!emit1(JSOp::False)) {
    //              [stack] ... OBJ NEXT ITER FALSE
    return false;
  }

  // TryNoteKind::Destructuring expects the iterator and the done value
  // to be the second to top and the top of the stack, respectively.
  // IteratorClose is called upon exception only if done is false.
  int32_t tryNoteDepth = bytecodeSection().stackDepth();

  for (ParseNode* member : pattern->contents()) {
    bool isFirst = member == pattern->head();
    DebugOnly<bool> hasNext = !!member->pn_next;

    ParseNode* subpattern;
    if (member->isKind(ParseNodeKind::Spread)) {
      subpattern = member->as<UnaryNode>().kid();

      MOZ_ASSERT(!subpattern->isKind(ParseNodeKind::AssignExpr));
    } else {
      subpattern = member;
    }

    ParseNode* lhsPattern = subpattern;
    ParseNode* pndefault = nullptr;
    if (subpattern->isKind(ParseNodeKind::AssignExpr)) {
      lhsPattern = subpattern->as<AssignmentNode>().left();
      pndefault = subpattern->as<AssignmentNode>().right();
    }

    // Number of stack slots emitted for the LHS reference.
    size_t emitted = 0;

    // Spec requires LHS reference to be evaluated first.
    bool isElision = lhsPattern->isKind(ParseNodeKind::Elision);
    if (!isElision) {
      auto emitLHSRef = [lhsPattern, &emitted](BytecodeEmitter* bce) {
        return bce->emitDestructuringLHSRef(lhsPattern, &emitted);
        //          [stack] ... OBJ NEXT ITER DONE LREF*
      };
      if (!wrapWithDestructuringTryNote(tryNoteDepth, emitLHSRef)) {
        return false;
      }
    }

    // Pick the DONE value to the top of the stack.
    if (emitted) {
      if (!emitPickN(emitted)) {
        //          [stack] ... OBJ NEXT ITER LREF* DONE
        return false;
      }
    }

    if (isFirst) {
      // If this element is the first, DONE is always FALSE, so pop it.
      //
      // Non-first elements should emit if-else depending on the
      // member pattern, below.
      if (!emit1(JSOp::Pop)) {
        //          [stack] ... OBJ NEXT ITER LREF*
        return false;
      }
    }

    if (member->isKind(ParseNodeKind::Spread)) {
      InternalIfEmitter ifThenElse(this);
      if (!isFirst) {
        // If spread is not the first element of the pattern,
        // iterator can already be completed.
        //          [stack] ... OBJ NEXT ITER LREF* DONE

        if (!ifThenElse.emitThenElse()) {
          //        [stack] ... OBJ NEXT ITER LREF*
          return false;
        }

        if (!emitUint32Operand(JSOp::NewArray, 0)) {
          //        [stack] ... OBJ NEXT ITER LREF* ARRAY
          return false;
        }
        if (!ifThenElse.emitElse()) {
          //        [stack] ... OBJ NEXT ITER LREF*
          return false;
        }
      }

      // If iterator is not completed, create a new array with the rest
      // of the iterator.
      if (!emitDupAt(emitted + 1, 2)) {
        //          [stack] ... OBJ NEXT ITER LREF* NEXT ITER
        return false;
      }
      if (!emitUint32Operand(JSOp::NewArray, 0)) {
        //          [stack] ... OBJ NEXT ITER LREF* NEXT ITER ARRAY
        return false;
      }
      if (!emitNumberOp(0)) {
        //          [stack] ... OBJ NEXT ITER LREF* NEXT ITER ARRAY INDEX
        return false;
      }
      if (!emitSpread()) {
        //          [stack] ... OBJ NEXT ITER LREF* ARRAY INDEX
        return false;
      }
      if (!emit1(JSOp::Pop)) {
        //          [stack] ... OBJ NEXT ITER LREF* ARRAY
        return false;
      }

      if (!isFirst) {
        if (!ifThenElse.emitEnd()) {
          return false;
        }
        MOZ_ASSERT(ifThenElse.pushed() == 1);
      }

      // At this point the iterator is done. Unpick a TRUE value for DONE above
      // ITER.
      if (!emit1(JSOp::True)) {
        //          [stack] ... OBJ NEXT ITER LREF* ARRAY TRUE
        return false;
      }
      if (!emitUnpickN(emitted + 1)) {
        //          [stack] ... OBJ NEXT ITER TRUE LREF* ARRAY
        return false;
      }

      auto emitAssignment = [lhsPattern, flav](BytecodeEmitter* bce) {
        return bce->emitSetOrInitializeDestructuring(lhsPattern, flav);
        //          [stack] ... OBJ NEXT ITER TRUE
      };
      if (!wrapWithDestructuringTryNote(tryNoteDepth, emitAssignment)) {
        return false;
      }

      MOZ_ASSERT(!hasNext);
      break;
    }

    InternalIfEmitter ifAlreadyDone(this);
    if (!isFirst) {
      //            [stack] ... OBJ NEXT ITER LREF* DONE

      if (!ifAlreadyDone.emitThenElse()) {
        //          [stack] ... OBJ NEXT ITER LREF*
        return false;
      }

      if (!emit1(JSOp::Undefined)) {
        //          [stack] ... OBJ NEXT ITER LREF* UNDEF
        return false;
      }
      if (!emit1(JSOp::NopDestructuring)) {
        //          [stack] ... OBJ NEXT ITER LREF* UNDEF
        return false;
      }

      // The iterator is done. Unpick a TRUE value for DONE above ITER.
      if (!emit1(JSOp::True)) {
        //          [stack] ... OBJ NEXT ITER LREF* UNDEF TRUE
        return false;
      }
      if (!emitUnpickN(emitted + 1)) {
        //          [stack] ... OBJ NEXT ITER TRUE LREF* UNDEF
        return false;
      }

      if (!ifAlreadyDone.emitElse()) {
        //          [stack] ... OBJ NEXT ITER LREF*
        return false;
      }
    }

    if (!emitDupAt(emitted + 1, 2)) {
      //            [stack] ... OBJ NEXT ITER LREF* NEXT
      return false;
    }
    if (!emitIteratorNext(Some(pattern->pn_pos.begin))) {
      //            [stack] ... OBJ NEXT ITER LREF* RESULT
      return false;
    }
    if (!emit1(JSOp::Dup)) {
      //            [stack] ... OBJ NEXT ITER LREF* RESULT RESULT
      return false;
    }
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::done())) {
      //            [stack] ... OBJ NEXT ITER LREF* RESULT DONE
      return false;
    }

    if (!emit1(JSOp::Dup)) {
      //            [stack] ... OBJ NEXT ITER LREF* RESULT DONE DONE
      return false;
    }
    if (!emitUnpickN(emitted + 2)) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* RESULT DONE
      return false;
    }

    InternalIfEmitter ifDone(this);
    if (!ifDone.emitThenElse()) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* RESULT
      return false;
    }

    if (!emit1(JSOp::Pop)) {
      //            [stack] ... OBJ NEXT ITER DONE LREF*
      return false;
    }
    if (!emit1(JSOp::Undefined)) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* UNDEF
      return false;
    }
    if (!emit1(JSOp::NopDestructuring)) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* UNDEF
      return false;
    }

    if (!ifDone.emitElse()) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* RESULT
      return false;
    }

    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::value())) {
      //            [stack] ... OBJ NEXT ITER DONE LREF* VALUE
      return false;
    }

    if (!ifDone.emitEnd()) {
      return false;
    }
    MOZ_ASSERT(ifDone.pushed() == 0);

    if (!isFirst) {
      if (!ifAlreadyDone.emitEnd()) {
        return false;
      }
      MOZ_ASSERT(ifAlreadyDone.pushed() == 2);
    }

    if (pndefault) {
      auto emitDefault = [pndefault, lhsPattern](BytecodeEmitter* bce) {
        return bce->emitDefault(pndefault, lhsPattern);
        //          [stack] ... OBJ NEXT ITER DONE LREF* VALUE
      };

      if (!wrapWithDestructuringTryNote(tryNoteDepth, emitDefault)) {
        return false;
      }
    }

    if (!isElision) {
      auto emitAssignment = [lhsPattern, flav](BytecodeEmitter* bce) {
        return bce->emitSetOrInitializeDestructuring(lhsPattern, flav);
        //          [stack] ... OBJ NEXT ITER DONE
      };

      if (!wrapWithDestructuringTryNote(tryNoteDepth, emitAssignment)) {
        return false;
      }
    } else {
      if (!emit1(JSOp::Pop)) {
        //          [stack] ... OBJ NEXT ITER DONE
        return false;
      }
    }
  }

  // The last DONE value is on top of the stack. If not DONE, call
  // IteratorClose.
  //                [stack] ... OBJ NEXT ITER DONE

  InternalIfEmitter ifDone(this);
  if (!ifDone.emitThenElse()) {
    //              [stack] ... OBJ NEXT ITER
    return false;
  }
  if (!emitPopN(2)) {
    //              [stack] ... OBJ
    return false;
  }
  if (!ifDone.emitElse()) {
    //              [stack] ... OBJ NEXT ITER
    return false;
  }
  if (!emit1(JSOp::Swap)) {
    //              [stack] ... OBJ ITER NEXT
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack] ... OBJ ITER
    return false;
  }
  if (!emitIteratorCloseInInnermostScope()) {
    //              [stack] ... OBJ
    return false;
  }
  if (!ifDone.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitComputedPropertyName(UnaryNode* computedPropName) {
  MOZ_ASSERT(computedPropName->isKind(ParseNodeKind::ComputedName));
  return emitTree(computedPropName->kid()) && emit1(JSOp::ToPropertyKey);
}

bool BytecodeEmitter::emitDestructuringOpsObject(ListNode* pattern,
                                                 DestructuringFlavor flav) {
  MOZ_ASSERT(pattern->isKind(ParseNodeKind::ObjectExpr));

  //                [stack] ... RHS
  MOZ_ASSERT(bytecodeSection().stackDepth() > 0);

  if (!emit1(JSOp::CheckObjCoercible)) {
    //              [stack] ... RHS
    return false;
  }

  bool needsRestPropertyExcludedSet =
      pattern->count() > 1 && pattern->last()->isKind(ParseNodeKind::Spread);
  if (needsRestPropertyExcludedSet) {
    if (!emitDestructuringObjRestExclusionSet(pattern)) {
      //            [stack] ... RHS SET
      return false;
    }

    if (!emit1(JSOp::Swap)) {
      //            [stack] ... SET RHS
      return false;
    }
  }

  for (ParseNode* member : pattern->contents()) {
    ParseNode* subpattern;
    if (member->isKind(ParseNodeKind::MutateProto) ||
        member->isKind(ParseNodeKind::Spread)) {
      subpattern = member->as<UnaryNode>().kid();

      MOZ_ASSERT_IF(member->isKind(ParseNodeKind::Spread),
                    !subpattern->isKind(ParseNodeKind::AssignExpr));
    } else {
      MOZ_ASSERT(member->isKind(ParseNodeKind::PropertyDefinition) ||
                 member->isKind(ParseNodeKind::Shorthand));
      subpattern = member->as<BinaryNode>().right();
    }

    ParseNode* lhs = subpattern;
    ParseNode* pndefault = nullptr;
    if (subpattern->isKind(ParseNodeKind::AssignExpr)) {
      lhs = subpattern->as<AssignmentNode>().left();
      pndefault = subpattern->as<AssignmentNode>().right();
    }

    // Number of stack slots emitted for the LHS reference.
    size_t emitted = 0;

    // Spec requires LHS reference to be evaluated first.
    if (!emitDestructuringLHSRef(lhs, &emitted)) {
      //            [stack] ... SET? RHS LREF*
      return false;
    }

    // Duplicate the value being destructured to use as a reference base.
    if (!emitDupAt(emitted)) {
      //            [stack] ... SET? RHS LREF* RHS
      return false;
    }

    if (member->isKind(ParseNodeKind::Spread)) {
      if (!updateSourceCoordNotes(member->pn_pos.begin)) {
        return false;
      }

      if (!emit1(JSOp::NewInit)) {
        //          [stack] ... SET? RHS LREF* RHS TARGET
        return false;
      }
      if (!emit1(JSOp::Dup)) {
        //          [stack] ... SET? RHS LREF* RHS TARGET TARGET
        return false;
      }
      if (!emit2(JSOp::Pick, 2)) {
        //          [stack] ... SET? RHS LREF* TARGET TARGET RHS
        return false;
      }

      if (needsRestPropertyExcludedSet) {
        if (!emit2(JSOp::Pick, emitted + 4)) {
          //        [stack] ... RHS LREF* TARGET TARGET RHS SET
          return false;
        }
      }

      CopyOption option = needsRestPropertyExcludedSet ? CopyOption::Filtered
                                                       : CopyOption::Unfiltered;
      if (!emitCopyDataProperties(option)) {
        //          [stack] ... RHS LREF* TARGET
        return false;
      }

      // Destructure TARGET per this member's lhs.
      if (!emitSetOrInitializeDestructuring(lhs, flav)) {
        //          [stack] ... RHS
        return false;
      }

      MOZ_ASSERT(member == pattern->last(), "Rest property is always last");
      break;
    }

    // Now push the property value currently being matched, which is the value
    // of the current property name "label" on the left of a colon in the object
    // initialiser.
    if (member->isKind(ParseNodeKind::MutateProto)) {
      if (!emitAtomOp(JSOp::GetProp,
                      TaggedParserAtomIndex::WellKnown::proto())) {
        //          [stack] ... SET? RHS LREF* PROP
        return false;
      }
    } else {
      MOZ_ASSERT(member->isKind(ParseNodeKind::PropertyDefinition) ||
                 member->isKind(ParseNodeKind::Shorthand));

      ParseNode* key = member->as<BinaryNode>().left();
      if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
          key->isKind(ParseNodeKind::StringExpr)) {
        if (!emitAtomOp(JSOp::GetProp, key->as<NameNode>().atom())) {
          //        [stack] ... SET? RHS LREF* PROP
          return false;
        }
      } else {
        if (key->isKind(ParseNodeKind::NumberExpr)) {
          if (!emitNumberOp(key->as<NumericLiteral>().value())) {
            //      [stack]... SET? RHS LREF* RHS KEY
            return false;
          }
        } else {
          // Otherwise this is a computed property name. BigInt keys are parsed
          // as (synthetic) computed property names, too.
          MOZ_ASSERT(key->isKind(ParseNodeKind::ComputedName));

          if (!emitComputedPropertyName(&key->as<UnaryNode>())) {
            //      [stack] ... SET? RHS LREF* RHS KEY
            return false;
          }

          // Add the computed property key to the exclusion set.
          if (needsRestPropertyExcludedSet) {
            if (!emitDupAt(emitted + 3)) {
              //    [stack] ... SET RHS LREF* RHS KEY SET
              return false;
            }
            if (!emitDupAt(1)) {
              //    [stack] ... SET RHS LREF* RHS KEY SET KEY
              return false;
            }
            if (!emit1(JSOp::Undefined)) {
              //    [stack] ... SET RHS LREF* RHS KEY SET KEY UNDEFINED
              return false;
            }
            if (!emit1(JSOp::InitElem)) {
              //    [stack] ... SET RHS LREF* RHS KEY SET
              return false;
            }
            if (!emit1(JSOp::Pop)) {
              //    [stack] ... SET RHS LREF* RHS KEY
              return false;
            }
          }
        }

        // Get the property value.
        if (!emitElemOpBase(JSOp::GetElem)) {
          //        [stack] ... SET? RHS LREF* PROP
          return false;
        }
      }
    }

    if (pndefault) {
      if (!emitDefault(pndefault, lhs)) {
        //          [stack] ... SET? RHS LREF* VALUE
        return false;
      }
    }

    // Destructure PROP per this member's lhs.
    if (!emitSetOrInitializeDestructuring(lhs, flav)) {
      //            [stack] ... SET? RHS
      return false;
    }
  }

  return true;
}

static bool IsDestructuringRestExclusionSetObjLiteralCompatible(
    ListNode* pattern) {
  uint32_t propCount = 0;
  for (ParseNode* member : pattern->contents()) {
    if (member->isKind(ParseNodeKind::Spread)) {
      MOZ_ASSERT(!member->pn_next, "unexpected trailing element after spread");
      break;
    }

    propCount++;

    if (member->isKind(ParseNodeKind::MutateProto)) {
      continue;
    }

    ParseNode* key = member->as<BinaryNode>().left();
    if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
        key->isKind(ParseNodeKind::StringExpr)) {
      continue;
    }

    // Number and BigInt keys aren't yet supported. Computed property names need
    // to be added dynamically.
    MOZ_ASSERT(key->isKind(ParseNodeKind::NumberExpr) ||
               key->isKind(ParseNodeKind::BigIntExpr) ||
               key->isKind(ParseNodeKind::ComputedName));
    return false;
  }

  if (propCount > SharedPropMap::MaxPropsForNonDictionary) {
    // JSOp::NewObject cannot accept dictionary-mode objects.
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDestructuringObjRestExclusionSet(ListNode* pattern) {
  MOZ_ASSERT(pattern->isKind(ParseNodeKind::ObjectExpr));
  MOZ_ASSERT(pattern->last()->isKind(ParseNodeKind::Spread));

  // See if we can use ObjLiteral to construct the exclusion set object.
  if (IsDestructuringRestExclusionSetObjLiteralCompatible(pattern)) {
    if (!emitDestructuringRestExclusionSetObjLiteral(pattern)) {
      //            [stack] OBJ
      return false;
    }
  } else {
    // Take the slow but sure way and start off with a blank object.
    if (!emit1(JSOp::NewInit)) {
      //            [stack] OBJ
      return false;
    }
  }

  for (ParseNode* member : pattern->contents()) {
    if (member->isKind(ParseNodeKind::Spread)) {
      MOZ_ASSERT(!member->pn_next, "unexpected trailing element after spread");
      break;
    }

    TaggedParserAtomIndex pnatom;
    if (member->isKind(ParseNodeKind::MutateProto)) {
      pnatom = TaggedParserAtomIndex::WellKnown::proto();
    } else {
      ParseNode* key = member->as<BinaryNode>().left();
      if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
          key->isKind(ParseNodeKind::StringExpr)) {
        pnatom = key->as<NameNode>().atom();
      } else if (key->isKind(ParseNodeKind::NumberExpr)) {
        if (!emitNumberOp(key->as<NumericLiteral>().value())) {
          return false;
        }
      } else {
        // Otherwise this is a computed property name which needs to be added
        // dynamically. BigInt keys are parsed as (synthetic) computed property
        // names, too.
        MOZ_ASSERT(key->isKind(ParseNodeKind::ComputedName));
        continue;
      }
    }

    // Initialize elements with |undefined|.
    if (!emit1(JSOp::Undefined)) {
      return false;
    }

    if (!pnatom) {
      if (!emit1(JSOp::InitElem)) {
        return false;
      }
    } else {
      if (!emitAtomOp(JSOp::InitProp, pnatom)) {
        return false;
      }
    }
  }

  return true;
}

bool BytecodeEmitter::emitDestructuringOps(ListNode* pattern,
                                           DestructuringFlavor flav) {
  if (pattern->isKind(ParseNodeKind::ArrayExpr)) {
    return emitDestructuringOpsArray(pattern, flav);
  }
  return emitDestructuringOpsObject(pattern, flav);
}

bool BytecodeEmitter::emitTemplateString(ListNode* templateString) {
  bool pushedString = false;

  for (ParseNode* item : templateString->contents()) {
    bool isString = (item->getKind() == ParseNodeKind::StringExpr ||
                     item->getKind() == ParseNodeKind::TemplateStringExpr);

    // Skip empty strings. These are very common: a template string like
    // `${a}${b}` has three empty strings and without this optimization
    // we'd emit four JSOp::Add operations instead of just one.
    if (isString && item->as<NameNode>().atom() ==
                        TaggedParserAtomIndex::WellKnown::empty()) {
      continue;
    }

    if (!isString) {
      // We update source notes before emitting the expression
      if (!updateSourceCoordNotes(item->pn_pos.begin)) {
        return false;
      }
    }

    if (!emitTree(item)) {
      return false;
    }

    if (!isString) {
      // We need to convert the expression to a string
      if (!emit1(JSOp::ToString)) {
        return false;
      }
    }

    if (pushedString) {
      // We've pushed two strings onto the stack. Add them together, leaving
      // just one.
      if (!emit1(JSOp::Add)) {
        return false;
      }
    } else {
      pushedString = true;
    }
  }

  if (!pushedString) {
    // All strings were empty, this can happen for something like `${""}`.
    // Just push an empty string.
    if (!emitStringOp(JSOp::String,
                      TaggedParserAtomIndex::WellKnown::empty())) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitDeclarationList(ListNode* declList) {
  for (ParseNode* decl : declList->contents()) {
    ParseNode* pattern;
    ParseNode* initializer;
    if (decl->isKind(ParseNodeKind::Name)) {
      pattern = decl;
      initializer = nullptr;
    } else {
      AssignmentNode* assignNode = &decl->as<AssignmentNode>();
      pattern = assignNode->left();
      initializer = assignNode->right();
    }

    if (pattern->isKind(ParseNodeKind::Name)) {
      // initializer can be null here.
      if (!emitSingleDeclaration(declList, &pattern->as<NameNode>(),
                                 initializer)) {
        return false;
      }
    } else {
      MOZ_ASSERT(pattern->isKind(ParseNodeKind::ArrayExpr) ||
                 pattern->isKind(ParseNodeKind::ObjectExpr));
      MOZ_ASSERT(initializer != nullptr);

      if (!updateSourceCoordNotes(initializer->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }
      if (!emitTree(initializer)) {
        return false;
      }

      if (!emitDestructuringOps(&pattern->as<ListNode>(),
                                DestructuringFlavor::Declaration)) {
        return false;
      }

      if (!emit1(JSOp::Pop)) {
        return false;
      }
    }
  }
  return true;
}

bool BytecodeEmitter::emitSingleDeclaration(ListNode* declList, NameNode* decl,
                                            ParseNode* initializer) {
  MOZ_ASSERT(decl->isKind(ParseNodeKind::Name));

  // Nothing to do for initializer-less 'var' declarations, as there's no TDZ.
  if (!initializer && declList->isKind(ParseNodeKind::VarStmt)) {
    return true;
  }

  auto nameAtom = decl->name();
  NameOpEmitter noe(this, nameAtom, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    //              [stack] ENV?
    return false;
  }
  if (!initializer) {
    // Lexical declarations are initialized to undefined without an
    // initializer.
    MOZ_ASSERT(declList->isKind(ParseNodeKind::LetDecl),
               "var declarations without initializers handled above, "
               "and const declarations must have initializers");
    if (!emit1(JSOp::Undefined)) {
      //            [stack] ENV? UNDEF
      return false;
    }
  } else {
    MOZ_ASSERT(initializer);

    if (!updateSourceCoordNotes(initializer->pn_pos.begin)) {
      return false;
    }
    if (!markStepBreakpoint()) {
      return false;
    }
    if (!emitInitializer(initializer, decl)) {
      //            [stack] ENV? V
      return false;
    }
  }
  if (!noe.emitAssignment()) {
    //              [stack] V
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitAssignmentRhs(
    ParseNode* rhs, TaggedParserAtomIndex anonFunctionName) {
  if (rhs->isDirectRHSAnonFunction()) {
    if (anonFunctionName) {
      return emitAnonymousFunctionWithName(rhs, anonFunctionName);
    }
    return emitAnonymousFunctionWithComputedName(rhs, FunctionPrefixKind::None);
  }
  return emitTree(rhs);
}

// The RHS value to assign is already on the stack, i.e., the next enumeration
// value in a for-in or for-of loop. Offset is the location in the stack of the
// already-emitted rhs. If we emitted a BIND[G]NAME, then the scope is on the
// top of the stack and we need to dig one deeper to get the right RHS value.
bool BytecodeEmitter::emitAssignmentRhs(uint8_t offset) {
  if (offset != 1) {
    return emitPickN(offset - 1);
  }

  return true;
}

static inline JSOp CompoundAssignmentParseNodeKindToJSOp(ParseNodeKind pnk) {
  switch (pnk) {
    case ParseNodeKind::InitExpr:
      return JSOp::Nop;
    case ParseNodeKind::AssignExpr:
      return JSOp::Nop;
    case ParseNodeKind::AddAssignExpr:
      return JSOp::Add;
    case ParseNodeKind::SubAssignExpr:
      return JSOp::Sub;
    case ParseNodeKind::BitOrAssignExpr:
      return JSOp::BitOr;
    case ParseNodeKind::BitXorAssignExpr:
      return JSOp::BitXor;
    case ParseNodeKind::BitAndAssignExpr:
      return JSOp::BitAnd;
    case ParseNodeKind::LshAssignExpr:
      return JSOp::Lsh;
    case ParseNodeKind::RshAssignExpr:
      return JSOp::Rsh;
    case ParseNodeKind::UrshAssignExpr:
      return JSOp::Ursh;
    case ParseNodeKind::MulAssignExpr:
      return JSOp::Mul;
    case ParseNodeKind::DivAssignExpr:
      return JSOp::Div;
    case ParseNodeKind::ModAssignExpr:
      return JSOp::Mod;
    case ParseNodeKind::PowAssignExpr:
      return JSOp::Pow;
    case ParseNodeKind::CoalesceAssignExpr:
    case ParseNodeKind::OrAssignExpr:
    case ParseNodeKind::AndAssignExpr:
      // Short-circuit assignment operators are handled elsewhere.
      [[fallthrough]];
    default:
      MOZ_CRASH("unexpected compound assignment op");
  }
}

bool BytecodeEmitter::emitAssignmentOrInit(ParseNodeKind kind, ParseNode* lhs,
                                           ParseNode* rhs) {
  JSOp compoundOp = CompoundAssignmentParseNodeKindToJSOp(kind);
  bool isCompound = compoundOp != JSOp::Nop;
  bool isInit = kind == ParseNodeKind::InitExpr;

  // We estimate the number of properties this could create
  // if used as constructor merely by counting this.foo = assignment
  // or init expressions;
  //
  // This currently doesn't handle this[x] = foo;
  if (isInit || kind == ParseNodeKind::AssignExpr) {
    if (lhs->isKind(ParseNodeKind::DotExpr)) {
      if (lhs->as<PropertyAccess>().expression().isKind(
              ParseNodeKind::ThisExpr)) {
        propertyAdditionEstimate++;
      }
    }
  }

  MOZ_ASSERT_IF(isInit, lhs->isKind(ParseNodeKind::DotExpr) ||
                            lhs->isKind(ParseNodeKind::ElemExpr) ||
                            lhs->isKind(ParseNodeKind::PrivateMemberExpr));

  // |name| is used within NameOpEmitter, so its lifetime must surpass |noe|.
  TaggedParserAtomIndex name;

  Maybe<NameOpEmitter> noe;
  Maybe<PropOpEmitter> poe;
  Maybe<ElemOpEmitter> eoe;
  Maybe<PrivateOpEmitter> xoe;

  // Deal with non-name assignments.
  uint8_t offset = 1;

  // Purpose of anonFunctionName:
  //
  // In normal name assignments (`f = function(){}`), an anonymous function gets
  // an inferred name based on the left-hand side name node.
  //
  // In normal property assignments (`obj.x = function(){}`), the anonymous
  // function does not have a computed name, and rhs->isDirectRHSAnonFunction()
  // will be false (and anonFunctionName will not be used). However, in field
  // initializers (`class C { x = function(){} }`), field initialization is
  // implemented via a property or elem assignment (where we are now), and
  // rhs->isDirectRHSAnonFunction() is set - so we'll assign the name of the
  // function.
  TaggedParserAtomIndex anonFunctionName;

  switch (lhs->getKind()) {
    case ParseNodeKind::Name: {
      name = lhs->as<NameNode>().name();
      anonFunctionName = name;
      noe.emplace(this, name,
                  isCompound ? NameOpEmitter::Kind::CompoundAssignment
                             : NameOpEmitter::Kind::SimpleAssignment);
      break;
    }
    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &lhs->as<PropertyAccess>();
      bool isSuper = prop->isSuper();
      poe.emplace(this,
                  isCompound ? PropOpEmitter::Kind::CompoundAssignment
                  : isInit   ? PropOpEmitter::Kind::PropInit
                             : PropOpEmitter::Kind::SimpleAssignment,
                  isSuper ? PropOpEmitter::ObjKind::Super
                          : PropOpEmitter::ObjKind::Other);
      if (!poe->prepareForObj()) {
        return false;
      }
      anonFunctionName = prop->name();
      if (isSuper) {
        UnaryNode* base = &prop->expression().as<UnaryNode>();
        if (!emitGetThisForSuperBase(base)) {
          //        [stack] THIS SUPERBASE
          return false;
        }
        // SUPERBASE is pushed onto THIS later in poe->emitGet below.
        offset += 2;
      } else {
        if (!emitTree(&prop->expression())) {
          //        [stack] OBJ
          return false;
        }
        offset += 1;
      }
      break;
    }
    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &lhs->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      eoe.emplace(this,
                  isCompound ? ElemOpEmitter::Kind::CompoundAssignment
                  : isInit   ? ElemOpEmitter::Kind::PropInit
                             : ElemOpEmitter::Kind::SimpleAssignment,
                  isSuper ? ElemOpEmitter::ObjKind::Super
                          : ElemOpEmitter::ObjKind::Other);
      if (!emitElemObjAndKey(elem, isSuper, *eoe)) {
        //          [stack] # if Super
        //          [stack] THIS KEY
        //          [stack] # otherwise
        //          [stack] OBJ KEY
        return false;
      }
      if (isSuper) {
        // SUPERBASE is pushed onto KEY in eoe->emitGet below.
        offset += 3;
      } else {
        offset += 2;
      }
      break;
    }
    case ParseNodeKind::PrivateMemberExpr: {
      PrivateMemberAccess* privateExpr = &lhs->as<PrivateMemberAccess>();
      xoe.emplace(this,
                  isCompound ? PrivateOpEmitter::Kind::CompoundAssignment
                  : isInit   ? PrivateOpEmitter::Kind::PropInit
                             : PrivateOpEmitter::Kind::SimpleAssignment,
                  privateExpr->privateName().name());
      if (!emitTree(&privateExpr->expression())) {
        //          [stack] OBJ
        return false;
      }
      if (!xoe->emitReference()) {
        //          [stack] OBJ KEY
        return false;
      }
      offset += xoe->numReferenceSlots();
      break;
    }
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      break;
    case ParseNodeKind::CallExpr:
      if (!emitTree(lhs)) {
        return false;
      }

      // Assignment to function calls is forbidden, but we have to make the
      // call first.  Now we can throw.
      if (!emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::AssignToCall))) {
        return false;
      }

      // Rebalance the stack to placate stack-depth assertions.
      if (!emit1(JSOp::Pop)) {
        return false;
      }
      break;
    default:
      MOZ_ASSERT(0);
  }

  if (isCompound) {
    MOZ_ASSERT(rhs);
    switch (lhs->getKind()) {
      case ParseNodeKind::DotExpr: {
        PropertyAccess* prop = &lhs->as<PropertyAccess>();
        if (!poe->emitGet(prop->key().atom())) {
          //        [stack] # if Super
          //        [stack] THIS SUPERBASE PROP
          //        [stack] # otherwise
          //        [stack] OBJ PROP
          return false;
        }
        break;
      }
      case ParseNodeKind::ElemExpr: {
        if (!eoe->emitGet()) {
          //        [stack] KEY THIS OBJ ELEM
          return false;
        }
        break;
      }
      case ParseNodeKind::PrivateMemberExpr: {
        if (!xoe->emitGet()) {
          //        [stack] OBJ KEY VALUE
          return false;
        }
        break;
      }
      case ParseNodeKind::CallExpr:
        // We just emitted a JSOp::ThrowMsg and popped the call's return
        // value.  Push a random value to make sure the stack depth is
        // correct.
        if (!emit1(JSOp::Null)) {
          //        [stack] NULL
          return false;
        }
        break;
      default:;
    }
  }

  switch (lhs->getKind()) {
    case ParseNodeKind::Name:
      if (!noe->prepareForRhs()) {
        //          [stack] ENV? VAL?
        return false;
      }
      offset += noe->emittedBindOp();
      break;
    case ParseNodeKind::DotExpr:
      if (!poe->prepareForRhs()) {
        //          [stack] # if Simple Assignment with Super
        //          [stack] THIS SUPERBASE
        //          [stack] # if Simple Assignment with other
        //          [stack] OBJ
        //          [stack] # if Compound Assignment with Super
        //          [stack] THIS SUPERBASE PROP
        //          [stack] # if Compound Assignment with other
        //          [stack] OBJ PROP
        return false;
      }
      break;
    case ParseNodeKind::ElemExpr:
      if (!eoe->prepareForRhs()) {
        //          [stack] # if Simple Assignment with Super
        //          [stack] THIS KEY SUPERBASE
        //          [stack] # if Simple Assignment with other
        //          [stack] OBJ KEY
        //          [stack] # if Compound Assignment with Super
        //          [stack] THIS KEY SUPERBASE ELEM
        //          [stack] # if Compound Assignment with other
        //          [stack] OBJ KEY ELEM
        return false;
      }
      break;
    case ParseNodeKind::PrivateMemberExpr:
      // no stack adjustment needed
      break;
    default:
      break;
  }

  if (rhs) {
    if (!emitAssignmentRhs(rhs, anonFunctionName)) {
      //            [stack] ... VAL? RHS
      return false;
    }
  } else {
    // Assumption: Things with pre-emitted RHS values never need to be named.
    if (!emitAssignmentRhs(offset)) {
      //            [stack] ... VAL? RHS
      return false;
    }
  }

  /* If += etc., emit the binary operator with a source note. */
  if (isCompound) {
    if (!newSrcNote(SrcNoteType::AssignOp)) {
      return false;
    }
    if (!emit1(compoundOp)) {
      //            [stack] ... VAL
      return false;
    }
  }

  /* Finally, emit the specialized assignment bytecode. */
  switch (lhs->getKind()) {
    case ParseNodeKind::Name: {
      if (!noe->emitAssignment()) {
        //          [stack] VAL
        return false;
      }
      break;
    }
    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &lhs->as<PropertyAccess>();
      if (!poe->emitAssignment(prop->key().atom())) {
        //          [stack] VAL
        return false;
      }
      break;
    }
    case ParseNodeKind::CallExpr:
      // We threw above, so nothing to do here.
      break;
    case ParseNodeKind::ElemExpr: {
      if (!eoe->emitAssignment()) {
        //          [stack] VAL
        return false;
      }
      break;
    }
    case ParseNodeKind::PrivateMemberExpr:
      if (!xoe->emitAssignment()) {
        //          [stack] VAL
        return false;
      }
      break;
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
      if (!emitDestructuringOps(&lhs->as<ListNode>(),
                                DestructuringFlavor::Assignment)) {
        return false;
      }
      break;
    default:
      MOZ_ASSERT(0);
  }
  return true;
}

bool BytecodeEmitter::emitShortCircuitAssignment(AssignmentNode* node) {
  TDZCheckCache tdzCache(this);

  JSOp op;
  switch (node->getKind()) {
    case ParseNodeKind::CoalesceAssignExpr:
      op = JSOp::Coalesce;
      break;
    case ParseNodeKind::OrAssignExpr:
      op = JSOp::Or;
      break;
    case ParseNodeKind::AndAssignExpr:
      op = JSOp::And;
      break;
    default:
      MOZ_CRASH("Unexpected ParseNodeKind");
  }

  ParseNode* lhs = node->left();
  ParseNode* rhs = node->right();

  // |name| is used within NameOpEmitter, so its lifetime must surpass |noe|.
  TaggedParserAtomIndex name;

  // Select the appropriate emitter based on the left-hand side.
  Maybe<NameOpEmitter> noe;
  Maybe<PropOpEmitter> poe;
  Maybe<ElemOpEmitter> eoe;
  Maybe<PrivateOpEmitter> xoe;

  int32_t depth = bytecodeSection().stackDepth();

  // Number of values pushed onto the stack in addition to the lhs value.
  int32_t numPushed;

  // Evaluate the left-hand side expression and compute any stack values needed
  // for the assignment.
  switch (lhs->getKind()) {
    case ParseNodeKind::Name: {
      name = lhs->as<NameNode>().name();
      noe.emplace(this, name, NameOpEmitter::Kind::CompoundAssignment);

      if (!noe->prepareForRhs()) {
        //          [stack] ENV? LHS
        return false;
      }

      numPushed = noe->emittedBindOp();
      break;
    }

    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &lhs->as<PropertyAccess>();
      bool isSuper = prop->isSuper();

      poe.emplace(this, PropOpEmitter::Kind::CompoundAssignment,
                  isSuper ? PropOpEmitter::ObjKind::Super
                          : PropOpEmitter::ObjKind::Other);

      if (!poe->prepareForObj()) {
        return false;
      }

      if (isSuper) {
        UnaryNode* base = &prop->expression().as<UnaryNode>();
        if (!emitGetThisForSuperBase(base)) {
          //        [stack] THIS SUPERBASE
          return false;
        }
      } else {
        if (!emitTree(&prop->expression())) {
          //        [stack] OBJ
          return false;
        }
      }

      if (!poe->emitGet(prop->key().atom())) {
        //          [stack] # if Super
        //          [stack] THIS SUPERBASE LHS
        //          [stack] # otherwise
        //          [stack] OBJ LHS
        return false;
      }

      if (!poe->prepareForRhs()) {
        //          [stack] # if Super
        //          [stack] THIS SUPERBASE LHS
        //          [stack] # otherwise
        //          [stack] OBJ LHS
        return false;
      }

      numPushed = 1 + isSuper;
      break;
    }

    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &lhs->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      eoe.emplace(this, ElemOpEmitter::Kind::CompoundAssignment,
                  isSuper ? ElemOpEmitter::ObjKind::Super
                          : ElemOpEmitter::ObjKind::Other);

      if (!emitElemObjAndKey(elem, isSuper, *eoe)) {
        //          [stack] # if Super
        //          [stack] THIS KEY
        //          [stack] # otherwise
        //          [stack] OBJ KEY
        return false;
      }

      if (!eoe->emitGet()) {
        //          [stack] # if Super
        //          [stack] THIS KEY SUPERBASE LHS
        //          [stack] # otherwise
        //          [stack] OBJ KEY LHS
        return false;
      }

      if (!eoe->prepareForRhs()) {
        //          [stack] # if Super
        //          [stack] THIS KEY SUPERBASE LHS
        //          [stack] # otherwise
        //          [stack] OBJ KEY LHS
        return false;
      }

      numPushed = 2 + isSuper;
      break;
    }

    case ParseNodeKind::PrivateMemberExpr: {
      PrivateMemberAccess* privateExpr = &lhs->as<PrivateMemberAccess>();
      xoe.emplace(this, PrivateOpEmitter::Kind::CompoundAssignment,
                  privateExpr->privateName().name());
      if (!emitTree(&privateExpr->expression())) {
        //          [stack] OBJ
        return false;
      }
      if (!xoe->emitReference()) {
        //          [stack] OBJ NAME
        return false;
      }
      if (!xoe->emitGet()) {
        //          [stack] OBJ NAME LHS
        return false;
      }
      numPushed = xoe->numReferenceSlots();
      break;
    }

    default:
      MOZ_CRASH();
  }

  MOZ_ASSERT(bytecodeSection().stackDepth() == depth + numPushed + 1);

  // Test for the short-circuit condition.
  JumpList jump;
  if (!emitJump(op, &jump)) {
    //              [stack] ... LHS
    return false;
  }

  // The short-circuit condition wasn't fulfilled, pop the left-hand side value
  // which was kept on the stack.
  if (!emit1(JSOp::Pop)) {
    //              [stack] ...
    return false;
  }

  if (!emitAssignmentRhs(rhs, name)) {
    //              [stack] ... RHS
    return false;
  }

  // Perform the actual assignment.
  switch (lhs->getKind()) {
    case ParseNodeKind::Name: {
      if (!noe->emitAssignment()) {
        //          [stack] RHS
        return false;
      }
      break;
    }

    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &lhs->as<PropertyAccess>();

      if (!poe->emitAssignment(prop->key().atom())) {
        //          [stack] RHS
        return false;
      }
      break;
    }

    case ParseNodeKind::ElemExpr: {
      if (!eoe->emitAssignment()) {
        //          [stack] RHS
        return false;
      }
      break;
    }

    case ParseNodeKind::PrivateMemberExpr:
      if (!xoe->emitAssignment()) {
        //          [stack] RHS
        return false;
      }
      break;

    default:
      MOZ_CRASH();
  }

  MOZ_ASSERT(bytecodeSection().stackDepth() == depth + 1);

  // Join with the short-circuit jump and pop anything left on the stack.
  if (numPushed > 0) {
    JumpList jumpAroundPop;
    if (!emitJump(JSOp::Goto, &jumpAroundPop)) {
      //            [stack] RHS
      return false;
    }

    if (!emitJumpTargetAndPatch(jump)) {
      //            [stack] ... LHS
      return false;
    }

    // Reconstruct the stack depth after the jump.
    bytecodeSection().setStackDepth(depth + 1 + numPushed);

    // Move the left-hand side value to the bottom and pop the rest.
    if (!emitUnpickN(numPushed)) {
      //            [stack] LHS ...
      return false;
    }
    if (!emitPopN(numPushed)) {
      //            [stack] LHS
      return false;
    }

    if (!emitJumpTargetAndPatch(jumpAroundPop)) {
      //            [stack] LHS | RHS
      return false;
    }
  } else {
    if (!emitJumpTargetAndPatch(jump)) {
      //            [stack] LHS | RHS
      return false;
    }
  }

  MOZ_ASSERT(bytecodeSection().stackDepth() == depth + 1);

  return true;
}

bool BytecodeEmitter::emitCallSiteObjectArray(ObjLiteralWriter& writer,
                                              ListNode* cookedOrRaw,
                                              ParseNode* head, uint32_t count) {
  DebugOnly<size_t> idx = 0;
  for (ParseNode* pn : cookedOrRaw->contentsFrom(head)) {
    MOZ_ASSERT(pn->isKind(ParseNodeKind::TemplateStringExpr) ||
               pn->isKind(ParseNodeKind::RawUndefinedExpr));

    if (!emitObjLiteralValue(writer, pn)) {
      return false;
    }
    idx++;
  }
  MOZ_ASSERT(idx == count);

  return true;
}

bool BytecodeEmitter::emitCallSiteObject(CallSiteNode* callSiteObj) {
  constexpr JSOp op = JSOp::CallSiteObj;

  // The first element of a call-site node is the raw-values list. Skip over it.
  ListNode* raw = callSiteObj->rawNodes();
  MOZ_ASSERT(raw->isKind(ParseNodeKind::ArrayExpr));
  ParseNode* head = callSiteObj->head()->pn_next;

  uint32_t count = callSiteObj->count() - 1;
  MOZ_ASSERT(count == raw->count());

  ObjLiteralWriter writer;
  writer.beginCallSiteObj(op);
  writer.beginDenseArrayElements();

  // Write elements of the two arrays: the 'cooked' values followed by the
  // 'raw' values.
  MOZ_RELEASE_ASSERT(count < UINT32_MAX / 2,
                     "Number of elements for both arrays must fit in uint32_t");
  if (!emitCallSiteObjectArray(writer, callSiteObj, head, count)) {
    return false;
  }
  if (!emitCallSiteObjectArray(writer, raw, raw->head(), count)) {
    return false;
  }

  GCThingIndex cookedIndex;
  if (!addObjLiteralData(writer, &cookedIndex)) {
    return false;
  }

  MOZ_ASSERT(sc->hasCallSiteObj());

  return emitInternedObjectOp(cookedIndex, op);
}

bool BytecodeEmitter::emitCatch(BinaryNode* catchClause) {
  // We must be nested under a try-finally statement.
  MOZ_ASSERT(innermostNestableControl->is<TryFinallyControl>());

  ParseNode* param = catchClause->left();
  if (!param) {
    // Catch parameter was omitted; just discard the exception.
    if (!emit1(JSOp::Pop)) {
      return false;
    }
  } else {
    switch (param->getKind()) {
      case ParseNodeKind::ArrayExpr:
      case ParseNodeKind::ObjectExpr:
        if (!emitDestructuringOps(&param->as<ListNode>(),
                                  DestructuringFlavor::Declaration)) {
          return false;
        }
        if (!emit1(JSOp::Pop)) {
          return false;
        }
        break;

      case ParseNodeKind::Name:
        if (!emitLexicalInitialization(&param->as<NameNode>())) {
          return false;
        }
        if (!emit1(JSOp::Pop)) {
          return false;
        }
        break;

      default:
        MOZ_ASSERT(0);
    }
  }

  /* Emit the catch body. */
  return emitTree(catchClause->right());
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See the
// comment on EmitSwitch.
MOZ_NEVER_INLINE bool BytecodeEmitter::emitTry(TryNode* tryNode) {
  LexicalScopeNode* catchScope = tryNode->catchScope();
  ParseNode* finallyNode = tryNode->finallyBlock();

  TryEmitter::Kind kind;
  if (catchScope) {
    if (finallyNode) {
      kind = TryEmitter::Kind::TryCatchFinally;
    } else {
      kind = TryEmitter::Kind::TryCatch;
    }
  } else {
    MOZ_ASSERT(finallyNode);
    kind = TryEmitter::Kind::TryFinally;
  }
  TryEmitter tryCatch(this, kind, TryEmitter::ControlKind::Syntactic);

  if (!tryCatch.emitTry()) {
    return false;
  }

  if (!emitTree(tryNode->body())) {
    return false;
  }

  // If this try has a catch block, emit it.
  if (catchScope) {
    // The emitted code for a catch block looks like:
    //
    // [pushlexicalenv]             only if any local aliased
    // exception
    // setlocal 0; pop              assign or possibly destructure exception
    // < catch block contents >
    // debugleaveblock
    // [poplexicalenv]              only if any local aliased
    // if there is a finally block:
    //   goto <finally>
    //   [jump target for returning from finally]
    //   goto <after finally>
    if (!tryCatch.emitCatch()) {
      return false;
    }

    // Emit the lexical scope and catch body.
    if (!emitTree(catchScope)) {
      return false;
    }
  }

  // Emit the finally handler, if there is one.
  if (finallyNode) {
    if (!tryCatch.emitFinally(Some(finallyNode->pn_pos.begin))) {
      return false;
    }

    if (!emitTree(finallyNode)) {
      return false;
    }
  }

  if (!tryCatch.emitEnd()) {
    return false;
  }

  return true;
}

[[nodiscard]] bool BytecodeEmitter::emitJumpToFinally(JumpList* jump,
                                                      uint32_t idx) {
  // Push the continuation index.
  if (!emitNumberOp(idx)) {
    return false;
  }

  // Push |throwing|.
  if (!emit1(JSOp::False)) {
    return false;
  }

  // Jump to the finally block.
  if (!emitJumpNoFallthrough(JSOp::Goto, jump)) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitIf(TernaryNode* ifNode) {
  IfEmitter ifThenElse(this);

  if (!ifThenElse.emitIf(Some(ifNode->kid1()->pn_pos.begin))) {
    return false;
  }

if_again:
  ParseNode* testNode = ifNode->kid1();
  auto conditionKind = IfEmitter::ConditionKind::Positive;
  if (testNode->isKind(ParseNodeKind::NotExpr)) {
    testNode = testNode->as<UnaryNode>().kid();
    conditionKind = IfEmitter::ConditionKind::Negative;
  }

  if (!markStepBreakpoint()) {
    return false;
  }

  // Emit code for the condition before pushing stmtInfo.
  // NOTE: NotExpr of testNode may be unwrapped, and in that case the negation
  //       is handled by conditionKind.
  if (!emitTree(testNode)) {
    return false;
  }

  ParseNode* elseNode = ifNode->kid3();
  if (elseNode) {
    if (!ifThenElse.emitThenElse(conditionKind)) {
      return false;
    }
  } else {
    if (!ifThenElse.emitThen(conditionKind)) {
      return false;
    }
  }

  /* Emit code for the then part. */
  if (!emitTree(ifNode->kid2())) {
    return false;
  }

  if (elseNode) {
    if (elseNode->isKind(ParseNodeKind::IfStmt)) {
      ifNode = &elseNode->as<TernaryNode>();

      if (!ifThenElse.emitElseIf(Some(ifNode->kid1()->pn_pos.begin))) {
        return false;
      }

      goto if_again;
    }

    if (!ifThenElse.emitElse()) {
      return false;
    }

    /* Emit code for the else part. */
    if (!emitTree(elseNode)) {
      return false;
    }
  }

  if (!ifThenElse.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitHoistedFunctionsInList(ListNode* stmtList) {
  MOZ_ASSERT(stmtList->hasTopLevelFunctionDeclarations());

  // We can call this multiple times for sloppy eval scopes.
  if (stmtList->emittedTopLevelFunctionDeclarations()) {
    return true;
  }

  stmtList->setEmittedTopLevelFunctionDeclarations();

  for (ParseNode* stmt : stmtList->contents()) {
    ParseNode* maybeFun = stmt;

    if (!sc->strict()) {
      while (maybeFun->isKind(ParseNodeKind::LabelStmt)) {
        maybeFun = maybeFun->as<LabeledStatement>().statement();
      }
    }

    if (maybeFun->is<FunctionNode>() &&
        maybeFun->as<FunctionNode>().functionIsHoisted()) {
      if (!emitTree(maybeFun)) {
        return false;
      }
    }
  }

  return true;
}

bool BytecodeEmitter::emitLexicalScopeBody(
    ParseNode* body, EmitLineNumberNote emitLineNote /* = EMIT_LINENOTE */) {
  if (body->isKind(ParseNodeKind::StatementList) &&
      body->as<ListNode>().hasTopLevelFunctionDeclarations()) {
    // This block contains function statements whose definitions are
    // hoisted to the top of the block. Emit these as a separate pass
    // before the rest of the block.
    if (!emitHoistedFunctionsInList(&body->as<ListNode>())) {
      return false;
    }
  }

  // Line notes were updated by emitLexicalScope or emitScript.
  return emitTree(body, ValueUsage::WantValue, emitLineNote);
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool BytecodeEmitter::emitLexicalScope(
    LexicalScopeNode* lexicalScope) {
  LexicalScopeEmitter lse(this);

  ParseNode* body = lexicalScope->scopeBody();
  if (lexicalScope->isEmptyScope()) {
    if (!lse.emitEmptyScope()) {
      return false;
    }

    if (!emitLexicalScopeBody(body)) {
      return false;
    }

    if (!lse.emitEnd()) {
      return false;
    }

    return true;
  }

  // We are about to emit some bytecode for what the spec calls "declaration
  // instantiation". Assign these instructions to the opening `{` of the
  // block. (Using the location of each declaration we're instantiating is
  // too weird when stepping in the debugger.)
  if (!ParseNodeRequiresSpecialLineNumberNotes(body)) {
    if (!updateSourceCoordNotes(lexicalScope->pn_pos.begin)) {
      return false;
    }
  }

  ScopeKind kind;
  if (body->isKind(ParseNodeKind::Catch)) {
    BinaryNode* catchNode = &body->as<BinaryNode>();
    kind =
        (!catchNode->left() || catchNode->left()->isKind(ParseNodeKind::Name))
            ? ScopeKind::SimpleCatch
            : ScopeKind::Catch;
  } else {
    kind = lexicalScope->kind();
  }

  if (!lse.emitScope(kind, lexicalScope->scopeBindings())) {
    return false;
  }

  if (body->isKind(ParseNodeKind::ForStmt)) {
    // for loops need to emit {FRESHEN,RECREATE}LEXICALENV if there are
    // lexical declarations in the head. Signal this by passing a
    // non-nullptr lexical scope.
    if (!emitFor(&body->as<ForNode>(), &lse.emitterScope())) {
      return false;
    }
  } else {
    if (!emitLexicalScopeBody(body, SUPPRESS_LINENOTE)) {
      return false;
    }
  }

  if (!lse.emitEnd()) {
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitWith(BinaryNode* withNode) {
  // Ensure that the column of the 'with' is set properly.
  if (!updateSourceCoordNotes(withNode->left()->pn_pos.begin)) {
    return false;
  }

  if (!markStepBreakpoint()) {
    return false;
  }

  if (!emitTree(withNode->left())) {
    return false;
  }

  EmitterScope emitterScope(this);
  if (!emitterScope.enterWith(this)) {
    return false;
  }

  if (!emitTree(withNode->right())) {
    return false;
  }

  return emitterScope.leave(this);
}

bool BytecodeEmitter::emitCopyDataProperties(CopyOption option) {
  DebugOnly<int32_t> depth = bytecodeSection().stackDepth();

  uint32_t argc;
  if (option == CopyOption::Filtered) {
    MOZ_ASSERT(depth > 2);
    //              [stack] TARGET SOURCE SET
    argc = 3;

    if (!emitAtomOp(JSOp::GetIntrinsic,
                    TaggedParserAtomIndex::WellKnown::CopyDataProperties())) {
      //            [stack] TARGET SOURCE SET COPYDATAPROPERTIES
      return false;
    }
  } else {
    MOZ_ASSERT(depth > 1);
    //              [stack] TARGET SOURCE
    argc = 2;

    if (!emitAtomOp(
            JSOp::GetIntrinsic,
            TaggedParserAtomIndex::WellKnown::CopyDataPropertiesUnfiltered())) {
      //            [stack] TARGET SOURCE COPYDATAPROPERTIES
      return false;
    }
  }

  if (!emit1(JSOp::Undefined)) {
    //              [stack] TARGET SOURCE SET? COPYDATAPROPERTIES
    //                    UNDEFINED
    return false;
  }
  if (!emit2(JSOp::Pick, argc + 1)) {
    //              [stack] SOURCE SET? COPYDATAPROPERTIES UNDEFINED
    //                    TARGET
    return false;
  }
  if (!emit2(JSOp::Pick, argc + 1)) {
    //              [stack] SET? COPYDATAPROPERTIES UNDEFINED TARGET
    //                    SOURCE
    return false;
  }
  if (option == CopyOption::Filtered) {
    if (!emit2(JSOp::Pick, argc + 1)) {
      //            [stack] COPYDATAPROPERTIES UNDEFINED TARGET SOURCE SET
      return false;
    }
  }
  // Callee is always self-hosted instrinsic, and cannot be content function.
  if (!emitCall(JSOp::CallIgnoresRv, argc)) {
    //              [stack] IGNORED
    return false;
  }

  if (!emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  MOZ_ASSERT(depth - int(argc) == bytecodeSection().stackDepth());
  return true;
}

bool BytecodeEmitter::emitBigIntOp(BigIntLiteral* bigint) {
  GCThingIndex index;
  if (!perScriptData().gcThingList().append(bigint, &index)) {
    return false;
  }
  return emitGCIndexOp(JSOp::BigInt, index);
}

bool BytecodeEmitter::emitIterator(
    SelfHostedIter selfHostedIter /* = SelfHostedIter::Deny */,
    bool isIteratorMethodOnStack /* = false */) {
  MOZ_ASSERT(selfHostedIter == SelfHostedIter::Allow ||
                 emitterMode != BytecodeEmitter::SelfHosting,
             "[Symbol.iterator]() call is prohibited in self-hosted code "
             "because it can run user-modifiable iteration code");

  if (!isIteratorMethodOnStack) {
    //              [stack] OBJ

    // Convert iterable to iterator.
    if (!emit1(JSOp::Dup)) {
      //            [stack] OBJ OBJ
      return false;
    }
    if (!emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::iterator))) {
      //            [stack] OBJ OBJ @@ITERATOR
      return false;
    }
    if (!emitElemOpBase(JSOp::GetElem)) {
      //            [stack] OBJ ITERFN
      return false;
    }
  }

  if (!emit1(JSOp::Swap)) {
    //              [stack] ITERFN OBJ
    return false;
  }
  if (!emitCall(getIterCallOp(JSOp::CallIter, selfHostedIter), 0)) {
    //              [stack] ITER
    return false;
  }
  if (!emitCheckIsObj(CheckIsObjectKind::GetIterator)) {
    //              [stack] ITER
    return false;
  }
  if (!emit1(JSOp::Dup)) {
    //              [stack] ITER ITER
    return false;
  }
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::next())) {
    //              [stack] ITER NEXT
    return false;
  }
  if (!emit1(JSOp::Swap)) {
    //              [stack] NEXT ITER
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitAsyncIterator(
    SelfHostedIter selfHostedIter /* = SelfHostedIter::Deny */,
    bool isIteratorMethodOnStack /* = false */) {
  MOZ_ASSERT(selfHostedIter == SelfHostedIter::Allow ||
                 emitterMode != BytecodeEmitter::SelfHosting,
             "[Symbol.asyncIterator]() call is prohibited in self-hosted code "
             "because it can run user-modifiable iteration code");

  if (!isIteratorMethodOnStack) {
    //              [stack] OBJ

    // Convert iterable to iterator.
    if (!emit1(JSOp::Dup)) {
      //            [stack] OBJ OBJ
      return false;
    }
    if (!emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::asyncIterator))) {
      //            [stack] OBJ OBJ @@ASYNCITERATOR
      return false;
    }
    if (!emitElemOpBase(JSOp::GetElem)) {
      //            [stack] OBJ ASYNC_ITERFN
      return false;
    }
  } else {
    //              [stack] OBJ ASYNC_ITERFN SYNC_ITERFN

    if (!emitElemOpBase(JSOp::Swap)) {
      //            [stack] OBJ SYNC_ITERFN ASYNC_ITERFN
      return false;
    }
  }

  InternalIfEmitter ifAsyncIterIsUndefined(this);
  if (!emit1(JSOp::IsNullOrUndefined)) {
    //              [stack] OBJ SYNC_ITERFN? ASYNC_ITERFN NULL-OR-UNDEF
    return false;
  }
  if (!ifAsyncIterIsUndefined.emitThenElse()) {
    //              [stack] OBJ SYNC_ITERFN? ASYNC_ITERFN
    return false;
  }

  if (!emit1(JSOp::Pop)) {
    //              [stack] OBJ SYNC_ITERFN?
    return false;
  }

  if (!isIteratorMethodOnStack) {
    if (!emit1(JSOp::Dup)) {
      //            [stack] OBJ OBJ
      return false;
    }
    if (!emit2(JSOp::Symbol, uint8_t(JS::SymbolCode::iterator))) {
      //            [stack] OBJ OBJ @@ITERATOR
      return false;
    }
    if (!emitElemOpBase(JSOp::GetElem)) {
      //            [stack] OBJ SYNC_ITERFN
      return false;
    }
  } else {
    //              [stack] OBJ SYNC_ITERFN
  }

  if (!emit1(JSOp::Swap)) {
    //              [stack] SYNC_ITERFN OBJ
    return false;
  }
  if (!emitCall(getIterCallOp(JSOp::CallIter, selfHostedIter), 0)) {
    //              [stack] ITER
    return false;
  }
  if (!emitCheckIsObj(CheckIsObjectKind::GetIterator)) {
    //              [stack] ITER
    return false;
  }

  if (!emit1(JSOp::Dup)) {
    //              [stack] ITER ITER
    return false;
  }
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::next())) {
    //              [stack] ITER SYNCNEXT
    return false;
  }

  if (!emit1(JSOp::ToAsyncIter)) {
    //              [stack] ITER
    return false;
  }

  if (!ifAsyncIterIsUndefined.emitElse()) {
    //              [stack] OBJ SYNC_ITERFN? ASYNC_ITERFN
    return false;
  }

  if (isIteratorMethodOnStack) {
    if (!emit1(JSOp::Swap)) {
      //            [stack] OBJ ASYNC_ITERFN SYNC_ITERFN
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      //            [stack] OBJ ASYNC_ITERFN
      return false;
    }
  }

  if (!emit1(JSOp::Swap)) {
    //              [stack] ASYNC_ITERFN OBJ
    return false;
  }
  if (!emitCall(getIterCallOp(JSOp::CallIter, selfHostedIter), 0)) {
    //              [stack] ITER
    return false;
  }
  if (!emitCheckIsObj(CheckIsObjectKind::GetAsyncIterator)) {
    //              [stack] ITER
    return false;
  }

  if (!ifAsyncIterIsUndefined.emitEnd()) {
    //              [stack] ITER
    return false;
  }

  if (!emit1(JSOp::Dup)) {
    //              [stack] ITER ITER
    return false;
  }
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::next())) {
    //              [stack] ITER NEXT
    return false;
  }
  if (!emit1(JSOp::Swap)) {
    //              [stack] NEXT ITER
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitSpread(SelfHostedIter selfHostedIter) {
  // [stack] NEXT ITER ARR I
  return emitSpread(selfHostedIter, 2, JSOp::InitElemInc);
  // [stack] ARR FINAL_INDEX
}

bool BytecodeEmitter::emitSpread(SelfHostedIter selfHostedIter,
                                 int spreadeeStackItems, JSOp storeElementOp) {
  LoopControl loopInfo(this, StatementKind::Spread);
  // In the [stack] annotations, (spreadee) can be "ARR I" (when spreading
  // into an array or into call parameters, or "TUPLE" (when spreading into a
  // tuple)

  if (!loopInfo.emitLoopHead(this, Nothing())) {
    //              [stack] NEXT ITER (spreadee)
    return false;
  }

  {
#ifdef DEBUG
    auto loopDepth = bytecodeSection().stackDepth();
#endif

    // Spread operations can't contain |continue|, so don't bother setting loop
    // and enclosing "update" offsets, as we do with for-loops.

    if (!emitDupAt(spreadeeStackItems + 1, 2)) {
      //            [stack] NEXT ITER (spreadee) NEXT ITER
      return false;
    }
    if (!emitIteratorNext(Nothing(), IteratorKind::Sync, selfHostedIter)) {
      //            [stack] NEXT ITER (spreadee) RESULT
      return false;
    }
    if (!emit1(JSOp::Dup)) {
      //            [stack] NEXT ITER (spreadee) RESULT RESULT
      return false;
    }
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::done())) {
      //            [stack] NEXT ITER (spreadee) RESULT DONE
      return false;
    }
    if (!emitJump(JSOp::JumpIfTrue, &loopInfo.breaks)) {
      //            [stack] NEXT ITER (spreadee) RESULT
      return false;
    }

    // Emit code to assign result.value to the iteration variable.
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::value())) {
      //            [stack] NEXT ITER (spreadee) VALUE
      return false;
    }
    if (!emit1(storeElementOp)) {
      //            [stack] NEXT ITER (spreadee)
      return false;
    }

    if (!loopInfo.emitLoopEnd(this, JSOp::Goto, TryNoteKind::ForOf)) {
      //            [stack] NEXT ITER (spreadee)
      return false;
    }

    MOZ_ASSERT(bytecodeSection().stackDepth() == loopDepth);
  }

  // When we leave the loop body and jump to this point, the result value is
  // still on the stack. Account for that by updating the stack depth
  // manually.
  bytecodeSection().setStackDepth(bytecodeSection().stackDepth() + 1);

  // No continues should occur in spreads.
  MOZ_ASSERT(!loopInfo.continues.offset.valid());

  if (!emit2(JSOp::Pick, spreadeeStackItems + 2)) {
    //              [stack] ITER (spreadee) RESULT NEXT
    return false;
  }
  if (!emit2(JSOp::Pick, spreadeeStackItems + 2)) {
    //              [stack] (spreadee) RESULT NEXT ITER
    return false;
  }

  return emitPopN(3);
  //                [stack] (spreadee)
}

bool BytecodeEmitter::emitInitializeForInOrOfTarget(TernaryNode* forHead) {
  MOZ_ASSERT(forHead->isKind(ParseNodeKind::ForIn) ||
             forHead->isKind(ParseNodeKind::ForOf));

  MOZ_ASSERT(bytecodeSection().stackDepth() >= 1,
             "must have a per-iteration value for initializing");

  ParseNode* target = forHead->kid1();
  MOZ_ASSERT(!forHead->kid2());

  // If the for-in/of loop didn't have a variable declaration, per-loop
  // initialization is just assigning the iteration value to a target
  // expression.
  if (!target->is<DeclarationListNode>()) {
    return emitAssignmentOrInit(ParseNodeKind::AssignExpr, target, nullptr);
    //              [stack] ... ITERVAL
  }

  // Otherwise, per-loop initialization is (possibly) declaration
  // initialization.  If the declaration is a lexical declaration, it must be
  // initialized.  If the declaration is a variable declaration, an
  // assignment to that name (which does *not* necessarily assign to the
  // variable!) must be generated.

  auto* declarationList = &target->as<DeclarationListNode>();
  if (!updateSourceCoordNotes(declarationList->pn_pos.begin)) {
    return false;
  }

  target = declarationList->singleBinding();

  NameNode* nameNode = nullptr;
  if (target->isKind(ParseNodeKind::Name)) {
    nameNode = &target->as<NameNode>();
  } else if (target->isKind(ParseNodeKind::AssignExpr)) {
    BinaryNode* assignNode = &target->as<BinaryNode>();
    if (assignNode->left()->is<NameNode>()) {
      nameNode = &assignNode->left()->as<NameNode>();
    }
  }

  if (nameNode) {
    auto nameAtom = nameNode->name();
    NameOpEmitter noe(this, nameAtom, NameOpEmitter::Kind::Initialize);
    if (!noe.prepareForRhs()) {
      return false;
    }
    if (noe.emittedBindOp()) {
      // Per-iteration initialization in for-in/of loops computes the
      // iteration value *before* initializing.  Thus the initializing
      // value may be buried under a bind-specific value on the stack.
      // Swap it to the top of the stack.
      MOZ_ASSERT(bytecodeSection().stackDepth() >= 2);
      if (!emit1(JSOp::Swap)) {
        return false;
      }
    } else {
      // In cases of emitting a frame slot or environment slot,
      // nothing needs be done.
      MOZ_ASSERT(bytecodeSection().stackDepth() >= 1);
    }
    if (!noe.emitAssignment()) {
      return false;
    }

    // The caller handles removing the iteration value from the stack.
    return true;
  }

  MOZ_ASSERT(
      !target->isKind(ParseNodeKind::AssignExpr),
      "for-in/of loop destructuring declarations can't have initializers");

  MOZ_ASSERT(target->isKind(ParseNodeKind::ArrayExpr) ||
             target->isKind(ParseNodeKind::ObjectExpr));
  return emitDestructuringOps(&target->as<ListNode>(),
                              DestructuringFlavor::Declaration);
}

bool BytecodeEmitter::emitForOf(ForNode* forOfLoop,
                                const EmitterScope* headLexicalEmitterScope) {
  MOZ_ASSERT(forOfLoop->isKind(ParseNodeKind::ForStmt));

  TernaryNode* forOfHead = forOfLoop->head();
  MOZ_ASSERT(forOfHead->isKind(ParseNodeKind::ForOf));

  unsigned iflags = forOfLoop->iflags();
  IteratorKind iterKind =
      (iflags & JSITER_FORAWAITOF) ? IteratorKind::Async : IteratorKind::Sync;
  MOZ_ASSERT_IF(iterKind == IteratorKind::Async, sc->isSuspendableContext());
  MOZ_ASSERT_IF(iterKind == IteratorKind::Async,
                sc->asSuspendableContext()->isAsync());

  ParseNode* forHeadExpr = forOfHead->kid3();

  // Certain builtins (e.g. Array.from) are implemented in self-hosting
  // as for-of loops.
  ForOfEmitter forOf(this, headLexicalEmitterScope,
                     getSelfHostedIterFor(forHeadExpr), iterKind);

  if (!forOf.emitIterated()) {
    //              [stack]
    return false;
  }

  if (!updateSourceCoordNotes(forHeadExpr->pn_pos.begin)) {
    return false;
  }
  if (!markStepBreakpoint()) {
    return false;
  }
  if (!emitTree(forHeadExpr)) {
    //              [stack] ITERABLE
    return false;
  }

  if (headLexicalEmitterScope) {
    DebugOnly<ParseNode*> forOfTarget = forOfHead->kid1();
    MOZ_ASSERT(forOfTarget->isKind(ParseNodeKind::LetDecl) ||
               forOfTarget->isKind(ParseNodeKind::ConstDecl));
  }

  bool isIteratorMethodOnStack = false;
  if (emitterMode == BytecodeEmitter::SelfHosting &&
      forHeadExpr->isKind(ParseNodeKind::CallExpr) &&
      forHeadExpr->as<BinaryNode>().left()->isName(
          TaggedParserAtomIndex::WellKnown::allowContentIterWith())) {
    // This is the following case:
    //
    //   for (const nextValue of allowContentIterWith(items, usingIterator)) {
    //
    // `items` is emitted by `emitTree(forHeadExpr)` above, and the result
    // is on the stack as ITERABLE.
    // `usingIterator` is the value of `items[Symbol.iterator]`, that's already
    // retrieved.
    ListNode* argsList = &forHeadExpr->as<BinaryNode>().right()->as<ListNode>();
    MOZ_ASSERT_IF(iterKind == IteratorKind::Sync, argsList->count() == 2);
    MOZ_ASSERT_IF(iterKind == IteratorKind::Async, argsList->count() == 3);

    if (!emitTree(argsList->head()->pn_next)) {
      //            [stack] ITERABLE ITERFN
      return false;
    }

    // Async iterator has two possible iterators: An async iterator and a sync
    // iterator.
    if (iterKind == IteratorKind::Async) {
      if (!emitTree(argsList->head()->pn_next->pn_next)) {
        //          [stack] ITERABLE ASYNC_ITERFN SYNC_ITERFN
        return false;
      }
    }

    isIteratorMethodOnStack = true;
  }

  if (!forOf.emitInitialize(forOfHead->pn_pos.begin, isIteratorMethodOnStack)) {
    //              [stack] NEXT ITER VALUE
    return false;
  }

  if (!emitInitializeForInOrOfTarget(forOfHead)) {
    //              [stack] NEXT ITER VALUE
    return false;
  }

  if (!forOf.emitBody()) {
    //              [stack] NEXT ITER UNDEF
    return false;
  }

  // Perform the loop body.
  ParseNode* forBody = forOfLoop->body();
  if (!emitTree(forBody)) {
    //              [stack] NEXT ITER UNDEF
    return false;
  }

  if (!forOf.emitEnd(forHeadExpr->pn_pos.begin)) {
    //              [stack]
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitForIn(ForNode* forInLoop,
                                const EmitterScope* headLexicalEmitterScope) {
  TernaryNode* forInHead = forInLoop->head();
  MOZ_ASSERT(forInHead->isKind(ParseNodeKind::ForIn));

  ForInEmitter forIn(this, headLexicalEmitterScope);

  // Annex B: Evaluate the var-initializer expression if present.
  // |for (var i = initializer in expr) { ... }|
  ParseNode* forInTarget = forInHead->kid1();
  if (forInTarget->is<DeclarationListNode>()) {
    auto* declarationList = &forInTarget->as<DeclarationListNode>();

    ParseNode* decl = declarationList->singleBinding();
    if (decl->isKind(ParseNodeKind::AssignExpr)) {
      BinaryNode* assignNode = &decl->as<BinaryNode>();
      if (assignNode->left()->is<NameNode>()) {
        NameNode* nameNode = &assignNode->left()->as<NameNode>();
        ParseNode* initializer = assignNode->right();
        MOZ_ASSERT(
            forInTarget->isKind(ParseNodeKind::VarStmt),
            "for-in initializers are only permitted for |var| declarations");

        if (!updateSourceCoordNotes(decl->pn_pos.begin)) {
          return false;
        }

        auto nameAtom = nameNode->name();
        NameOpEmitter noe(this, nameAtom, NameOpEmitter::Kind::Initialize);
        if (!noe.prepareForRhs()) {
          return false;
        }
        if (!emitInitializer(initializer, nameNode)) {
          return false;
        }
        if (!noe.emitAssignment()) {
          return false;
        }

        // Pop the initializer.
        if (!emit1(JSOp::Pop)) {
          return false;
        }
      }
    }
  }

  if (!forIn.emitIterated()) {
    //              [stack]
    return false;
  }

  // Evaluate the expression being iterated.
  ParseNode* expr = forInHead->kid3();

  if (!updateSourceCoordNotes(expr->pn_pos.begin)) {
    return false;
  }
  if (!markStepBreakpoint()) {
    return false;
  }
  if (!emitTree(expr)) {
    //              [stack] EXPR
    return false;
  }

  MOZ_ASSERT(forInLoop->iflags() == 0);

  MOZ_ASSERT_IF(headLexicalEmitterScope,
                forInTarget->isKind(ParseNodeKind::LetDecl) ||
                    forInTarget->isKind(ParseNodeKind::ConstDecl));

  if (!forIn.emitInitialize()) {
    //              [stack] ITER ITERVAL
    return false;
  }

  if (!emitInitializeForInOrOfTarget(forInHead)) {
    //              [stack] ITER ITERVAL
    return false;
  }

  if (!forIn.emitBody()) {
    //              [stack] ITER ITERVAL
    return false;
  }

  // Perform the loop body.
  ParseNode* forBody = forInLoop->body();
  if (!emitTree(forBody)) {
    //              [stack] ITER ITERVAL
    return false;
  }

  if (!forIn.emitEnd(forInHead->pn_pos.begin)) {
    //              [stack]
    return false;
  }

  return true;
}

/* C-style `for (init; cond; update) ...` loop. */
bool BytecodeEmitter::emitCStyleFor(
    ForNode* forNode, const EmitterScope* headLexicalEmitterScope) {
  TernaryNode* forHead = forNode->head();
  ParseNode* forBody = forNode->body();
  ParseNode* init = forHead->kid1();
  ParseNode* cond = forHead->kid2();
  ParseNode* update = forHead->kid3();
  bool isLet = init && init->isKind(ParseNodeKind::LetDecl);

  CForEmitter cfor(this, isLet ? headLexicalEmitterScope : nullptr);

  if (!cfor.emitInit(init ? Some(init->pn_pos.begin) : Nothing())) {
    //              [stack]
    return false;
  }

  // If the head of this for-loop declared any lexical variables, the parser
  // wrapped this ParseNodeKind::For node in a ParseNodeKind::LexicalScope
  // representing the implicit scope of those variables. By the time we get
  // here, we have already entered that scope. So far, so good.
  if (init) {
    // Emit the `init` clause, whether it's an expression or a variable
    // declaration. (The loop variables were hoisted into an enclosing
    // scope, but we still need to emit code for the initializers.)
    if (init->is<DeclarationListNode>()) {
      MOZ_ASSERT(!init->as<DeclarationListNode>().empty());

      if (!emitTree(init)) {
        //          [stack]
        return false;
      }
    } else {
      if (!updateSourceCoordNotes(init->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }

      // 'init' is an expression, not a declaration. emitTree left its
      // value on the stack.
      if (!emitTree(init, ValueUsage::IgnoreValue)) {
        //          [stack] VAL
        return false;
      }
      if (!emit1(JSOp::Pop)) {
        //          [stack]
        return false;
      }
    }
  }

  if (!cfor.emitCond(cond ? Some(cond->pn_pos.begin) : Nothing())) {
    //              [stack]
    return false;
  }

  if (cond) {
    if (!updateSourceCoordNotes(cond->pn_pos.begin)) {
      return false;
    }
    if (!markStepBreakpoint()) {
      return false;
    }
    if (!emitTree(cond)) {
      //            [stack] VAL
      return false;
    }
  }

  if (!cfor.emitBody(cond ? CForEmitter::Cond::Present
                          : CForEmitter::Cond::Missing)) {
    //              [stack]
    return false;
  }

  if (!emitTree(forBody)) {
    //              [stack]
    return false;
  }

  if (!cfor.emitUpdate(
          update ? CForEmitter::Update::Present : CForEmitter::Update::Missing,
          update ? Some(update->pn_pos.begin) : Nothing())) {
    //              [stack]
    return false;
  }

  // Check for update code to do before the condition (if any).
  if (update) {
    if (!updateSourceCoordNotes(update->pn_pos.begin)) {
      return false;
    }
    if (!markStepBreakpoint()) {
      return false;
    }
    if (!emitTree(update, ValueUsage::IgnoreValue)) {
      //            [stack] VAL
      return false;
    }
  }

  if (!cfor.emitEnd(forNode->pn_pos.begin)) {
    //              [stack]
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitFor(ForNode* forNode,
                              const EmitterScope* headLexicalEmitterScope) {
  if (forNode->head()->isKind(ParseNodeKind::ForHead)) {
    return emitCStyleFor(forNode, headLexicalEmitterScope);
  }

  if (!updateLineNumberNotes(forNode->pn_pos.begin)) {
    return false;
  }

  if (forNode->head()->isKind(ParseNodeKind::ForIn)) {
    return emitForIn(forNode, headLexicalEmitterScope);
  }

  MOZ_ASSERT(forNode->head()->isKind(ParseNodeKind::ForOf));
  return emitForOf(forNode, headLexicalEmitterScope);
}

MOZ_NEVER_INLINE bool BytecodeEmitter::emitFunction(
    FunctionNode* funNode, bool needsProto /* = false */) {
  FunctionBox* funbox = funNode->funbox();

  //                [stack]

  FunctionEmitter fe(this, funbox, funNode->syntaxKind(),
                     funNode->functionIsHoisted()
                         ? FunctionEmitter::IsHoisted::Yes
                         : FunctionEmitter::IsHoisted::No);

  // |wasEmittedByEnclosingScript| flag is set to true once the function has
  // been emitted. Function definitions that need hoisting to the top of the
  // function will be seen by emitFunction in two places.
  if (funbox->wasEmittedByEnclosingScript()) {
    if (!fe.emitAgain()) {
      //            [stack]
      return false;
    }
    MOZ_ASSERT(funNode->functionIsHoisted());
  } else if (funbox->isInterpreted()) {
    if (!funbox->emitBytecode) {
      return fe.emitLazy();
      //            [stack] FUN?
    }

    if (!fe.prepareForNonLazy()) {
      //            [stack]
      return false;
    }

    BytecodeEmitter bce2(this, funbox);
    if (!bce2.init(funNode->pn_pos)) {
      return false;
    }

    /* We measured the max scope depth when we parsed the function. */
    if (!bce2.emitFunctionScript(funNode)) {
      return false;
    }

    if (!fe.emitNonLazyEnd()) {
      //            [stack] FUN?
      return false;
    }
  } else {
    if (!fe.emitAsmJSModule()) {
      //            [stack]
      return false;
    }
  }

  // Track the last emitted top-level self-hosted function, so that intrinsics
  // can adjust attributes at parse time.
  //
  // NOTE: We also disallow lambda functions in the top-level body. This is done
  // to simplify handling of the self-hosted stencil. Within normal function
  // declarations there are no such restrictions.
  if (emitterMode == EmitterMode::SelfHosting) {
    if (sc->isTopLevelContext()) {
      MOZ_ASSERT(!funbox->isLambda());
      MOZ_ASSERT(funbox->explicitName());
      prevSelfHostedTopLevelFunction = funbox;
    }
  }

  return true;
}

bool BytecodeEmitter::emitDo(BinaryNode* doNode) {
  ParseNode* bodyNode = doNode->left();

  DoWhileEmitter doWhile(this);
  if (!doWhile.emitBody(doNode->pn_pos.begin, getOffsetForLoop(bodyNode))) {
    return false;
  }

  if (!emitTree(bodyNode)) {
    return false;
  }

  if (!doWhile.emitCond()) {
    return false;
  }

  ParseNode* condNode = doNode->right();
  if (!updateSourceCoordNotes(condNode->pn_pos.begin)) {
    return false;
  }
  if (!markStepBreakpoint()) {
    return false;
  }
  if (!emitTree(condNode)) {
    return false;
  }

  if (!doWhile.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitWhile(BinaryNode* whileNode) {
  ParseNode* bodyNode = whileNode->right();

  WhileEmitter wh(this);

  ParseNode* condNode = whileNode->left();
  if (!wh.emitCond(whileNode->pn_pos.begin, getOffsetForLoop(condNode),
                   whileNode->pn_pos.end)) {
    return false;
  }

  if (!updateSourceCoordNotes(condNode->pn_pos.begin)) {
    return false;
  }
  if (!markStepBreakpoint()) {
    return false;
  }
  if (!emitTree(condNode)) {
    return false;
  }

  if (!wh.emitBody()) {
    return false;
  }
  if (!emitTree(bodyNode)) {
    return false;
  }

  if (!wh.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitBreak(TaggedParserAtomIndex label) {
  BreakableControl* target;
  if (label) {
    // Any statement with the matching label may be the break target.
    auto hasSameLabel = [label](LabelControl* labelControl) {
      return labelControl->label() == label;
    };
    target = findInnermostNestableControl<LabelControl>(hasSameLabel);
  } else {
    auto isNotLabel = [](BreakableControl* control) {
      return !control->is<LabelControl>();
    };
    target = findInnermostNestableControl<BreakableControl>(isNotLabel);
  }

  return emitGoto(target, GotoKind::Break);
}

bool BytecodeEmitter::emitContinue(TaggedParserAtomIndex label) {
  LoopControl* target = nullptr;
  if (label) {
    // Find the loop statement enclosed by the matching label.
    NestableControl* control = innermostNestableControl;
    while (!control->is<LabelControl>() ||
           control->as<LabelControl>().label() != label) {
      if (control->is<LoopControl>()) {
        target = &control->as<LoopControl>();
      }
      control = control->enclosing();
    }
  } else {
    target = findInnermostNestableControl<LoopControl>();
  }
  return emitGoto(target, GotoKind::Continue);
}

bool BytecodeEmitter::emitGetFunctionThis(NameNode* thisName) {
  MOZ_ASSERT(sc->hasFunctionThisBinding());
  MOZ_ASSERT(thisName->isName(TaggedParserAtomIndex::WellKnown::dotThis()));

  if (!updateLineNumberNotes(thisName->pn_pos.begin)) {
    return false;
  }

  if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
    //              [stack] THIS
    return false;
  }
  if (sc->needsThisTDZChecks()) {
    if (!emit1(JSOp::CheckThis)) {
      //            [stack] THIS
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitGetThisForSuperBase(UnaryNode* superBase) {
  MOZ_ASSERT(superBase->isKind(ParseNodeKind::SuperBase));
  NameNode* nameNode = &superBase->kid()->as<NameNode>();
  return emitGetFunctionThis(nameNode);
  //                [stack] THIS
}

bool BytecodeEmitter::emitThisLiteral(ThisLiteral* pn) {
  if (ParseNode* kid = pn->kid()) {
    NameNode* thisName = &kid->as<NameNode>();
    return emitGetFunctionThis(thisName);
    //              [stack] THIS
  }

  if (sc->thisBinding() == ThisBinding::Module) {
    return emit1(JSOp::Undefined);
    //              [stack] UNDEF
  }

  MOZ_ASSERT(sc->thisBinding() == ThisBinding::Global);

  MOZ_ASSERT(outermostScope().hasNonSyntacticScopeOnChain() ==
             sc->hasNonSyntacticScope());
  if (sc->hasNonSyntacticScope()) {
    return emit1(JSOp::NonSyntacticGlobalThis);
    //                [stack] THIS
  }

  return emit1(JSOp::GlobalThis);
  //                [stack] THIS
}

bool BytecodeEmitter::emitCheckDerivedClassConstructorReturn() {
  MOZ_ASSERT(
      lookupName(TaggedParserAtomIndex::WellKnown::dotThis()).hasKnownSlot());
  if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
    return false;
  }
  if (!emit1(JSOp::CheckReturn)) {
    return false;
  }
  if (!emit1(JSOp::SetRval)) {
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitNewTarget() {
  MOZ_ASSERT(sc->allowNewTarget());

  if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotNewTarget())) {
    //              [stack] NEW.TARGET
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitNewTarget(NewTargetNode* pn) {
  MOZ_ASSERT(pn->newTargetName()->isName(
      TaggedParserAtomIndex::WellKnown::dotNewTarget()));

  return emitNewTarget();
}

bool BytecodeEmitter::emitNewTarget(CallNode* pn) {
  MOZ_ASSERT(pn->callOp() == JSOp::SuperCall ||
             pn->callOp() == JSOp::SpreadSuperCall);

  // The parser is responsible for marking the "new.target" binding as being
  // implicitly used in super() calls.
  return emitNewTarget();
}

bool BytecodeEmitter::emitReturn(UnaryNode* returnNode) {
  if (!updateSourceCoordNotes(returnNode->pn_pos.begin)) {
    return false;
  }

  if (!markStepBreakpoint()) {
    return false;
  }

  /* Push a return value */
  if (ParseNode* expr = returnNode->kid()) {
    if (!emitTree(expr)) {
      return false;
    }

    if (sc->asSuspendableContext()->isAsync() &&
        sc->asSuspendableContext()->isGenerator()) {
      if (!emitAwaitInInnermostScope()) {
        return false;
      }
    }
  } else {
    /* No explicit return value provided */
    if (!emit1(JSOp::Undefined)) {
      return false;
    }
  }

  // We know functionBodyEndPos is set because "return" is only
  // valid in a function, and so we've passed through
  // emitFunctionScript.
  if (!updateSourceCoordNotes(*functionBodyEndPos)) {
    return false;
  }

  /*
   * The return value is currently on the stack. We would like to
   * generate JSOp::Return, but if we have work to do before returning,
   * we will instead generate JSOp::SetRval / JSOp::RetRval.
   *
   * We don't know whether we will need fixup code until after calling
   * prepareForNonLocalJumpToOutermost, so we start by generating
   * JSOp::SetRval, then mutate it to JSOp::Return in finishReturn if it
   * wasn't needed.
   */
  BytecodeOffset setRvalOffset = bytecodeSection().offset();
  if (!emit1(JSOp::SetRval)) {
    return false;
  }

  NonLocalExitControl nle(this, NonLocalExitKind::Return);
  return nle.emitReturn(setRvalOffset);
}

bool BytecodeEmitter::finishReturn(BytecodeOffset setRvalOffset) {
  // The return value is currently in rval. Depending on the current function,
  // we may have to do additional work before returning:
  // - Derived class constructors must check if the return value is an object.
  // - Generators and async functions must do a final yield.
  // - Non-async generators must return the value as an iterator result:
  //   { value: <rval>, done: true }
  // - Non-generator async functions must resolve the function's result promise
  //   with the value.
  //
  // If we have not generated any code since the SetRval that stored the return
  // value, we can also optimize the bytecode by rewriting that SetRval as a
  // JSOp::Return. See |emitReturn| above.

  bool isDerivedClassConstructor =
      sc->isFunctionBox() && sc->asFunctionBox()->isDerivedClassConstructor();
  bool needsFinalYield =
      sc->isFunctionBox() && sc->asFunctionBox()->needsFinalYield();
  bool isSimpleReturn =
      setRvalOffset.valid() &&
      setRvalOffset + BytecodeOffsetDiff(JSOpLength_SetRval) ==
          bytecodeSection().offset();

  if (isDerivedClassConstructor) {
    MOZ_ASSERT(!needsFinalYield);
    if (!emitJump(JSOp::Goto, &endOfDerivedClassConstructorBody)) {
      return false;
    }
    return true;
  }

  if (needsFinalYield) {
    if (!emitJump(JSOp::Goto, &finalYields)) {
      return false;
    }
    return true;
  }

  if (isSimpleReturn) {
    MOZ_ASSERT(JSOp(bytecodeSection().code()[setRvalOffset.value()]) ==
               JSOp::SetRval);
    bytecodeSection().code()[setRvalOffset.value()] = jsbytecode(JSOp::Return);
    return true;
  }

  // Nothing special needs to be done.
  return emitReturnRval();
}

bool BytecodeEmitter::emitGetDotGeneratorInScope(EmitterScope& currentScope) {
  if (!sc->isFunction() && sc->isModuleContext() &&
      sc->asModuleContext()->isAsync()) {
    NameLocation loc = *locationOfNameBoundInScopeType<ModuleScope>(
        TaggedParserAtomIndex::WellKnown::dotGenerator(), &currentScope);
    return emitGetNameAtLocation(
        TaggedParserAtomIndex::WellKnown::dotGenerator(), loc);
  }
  NameLocation loc = *locationOfNameBoundInScopeType<FunctionScope>(
      TaggedParserAtomIndex::WellKnown::dotGenerator(), &currentScope);
  return emitGetNameAtLocation(TaggedParserAtomIndex::WellKnown::dotGenerator(),
                               loc);
}

bool BytecodeEmitter::emitInitialYield(UnaryNode* yieldNode) {
  if (!emitTree(yieldNode->kid())) {
    return false;
  }

  if (!emitYieldOp(JSOp::InitialYield)) {
    //              [stack] RVAL GENERATOR RESUMEKIND
    return false;
  }
  if (!emit1(JSOp::CheckResumeKind)) {
    //              [stack] RVAL
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitYield(UnaryNode* yieldNode) {
  MOZ_ASSERT(sc->isFunctionBox());
  MOZ_ASSERT(sc->asFunctionBox()->isGenerator());
  MOZ_ASSERT(yieldNode->isKind(ParseNodeKind::YieldExpr));

  bool needsIteratorResult = sc->asFunctionBox()->needsIteratorResult();
  if (needsIteratorResult) {
    if (!emitPrepareIteratorResult()) {
      //            [stack] ITEROBJ
      return false;
    }
  }
  if (ParseNode* expr = yieldNode->kid()) {
    if (!emitTree(expr)) {
      //            [stack] ITEROBJ? VAL
      return false;
    }
  } else {
    if (!emit1(JSOp::Undefined)) {
      //            [stack] ITEROBJ? UNDEFINED
      return false;
    }
  }

  if (sc->asSuspendableContext()->isAsync()) {
    MOZ_ASSERT(!needsIteratorResult);
    if (!emitAwaitInInnermostScope()) {
      //            [stack] RESULT
      return false;
    }
  }

  if (needsIteratorResult) {
    if (!emitFinishIteratorResult(false)) {
      //            [stack] ITEROBJ
      return false;
    }
  }

  if (!emitGetDotGeneratorInInnermostScope()) {
    //              [stack] # if needsIteratorResult
    //              [stack] ITEROBJ .GENERATOR
    //              [stack] # else
    //              [stack] RESULT .GENERATOR
    return false;
  }

  if (!emitYieldOp(JSOp::Yield)) {
    //              [stack] YIELDRESULT GENERATOR RESUMEKIND
    return false;
  }

  if (!emit1(JSOp::CheckResumeKind)) {
    //              [stack] YIELDRESULT
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitAwaitInInnermostScope(UnaryNode* awaitNode) {
  MOZ_ASSERT(sc->isSuspendableContext());
  MOZ_ASSERT(awaitNode->isKind(ParseNodeKind::AwaitExpr));

  if (!emitTree(awaitNode->kid())) {
    return false;
  }
  return emitAwaitInInnermostScope();
}

bool BytecodeEmitter::emitAwaitInScope(EmitterScope& currentScope) {
  if (!emit1(JSOp::CanSkipAwait)) {
    //              [stack] VALUE CANSKIP
    return false;
  }

  if (!emit1(JSOp::MaybeExtractAwaitValue)) {
    //              [stack] VALUE_OR_RESOLVED CANSKIP
    return false;
  }

  InternalIfEmitter ifCanSkip(this);
  if (!ifCanSkip.emitThen(IfEmitter::ConditionKind::Negative)) {
    //              [stack] VALUE_OR_RESOLVED
    return false;
  }

  if (sc->asSuspendableContext()->needsPromiseResult()) {
    if (!emitGetDotGeneratorInScope(currentScope)) {
      //            [stack] VALUE GENERATOR
      return false;
    }
    if (!emit1(JSOp::AsyncAwait)) {
      //            [stack] PROMISE
      return false;
    }
  }

  if (!emitGetDotGeneratorInScope(currentScope)) {
    //              [stack] VALUE|PROMISE GENERATOR
    return false;
  }
  if (!emitYieldOp(JSOp::Await)) {
    //              [stack] RESOLVED GENERATOR RESUMEKIND
    return false;
  }
  if (!emit1(JSOp::CheckResumeKind)) {
    //              [stack] RESOLVED
    return false;
  }

  if (!ifCanSkip.emitEnd()) {
    return false;
  }

  MOZ_ASSERT(ifCanSkip.popped() == 0);

  return true;
}

// ES2019 draft rev 49b781ec80117b60f73327ef3054703a3111e40c
// 14.4.14 Runtime Semantics: Evaluation
// YieldExpression : yield* AssignmentExpression
bool BytecodeEmitter::emitYieldStar(ParseNode* iter) {
  MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting,
             "yield* is prohibited in self-hosted code because it can run "
             "user-modifiable iteration code");

  MOZ_ASSERT(sc->isSuspendableContext());
  MOZ_ASSERT(sc->asSuspendableContext()->isGenerator());

  // Step 1.
  IteratorKind iterKind = sc->asSuspendableContext()->isAsync()
                              ? IteratorKind::Async
                              : IteratorKind::Sync;
  bool needsIteratorResult = sc->asSuspendableContext()->needsIteratorResult();

  // Steps 2-5.
  if (!emitTree(iter)) {
    //              [stack] ITERABLE
    return false;
  }
  if (iterKind == IteratorKind::Async) {
    if (!emitAsyncIterator()) {
      //            [stack] NEXT ITER
      return false;
    }
  } else {
    if (!emitIterator()) {
      //            [stack] NEXT ITER
      return false;
    }
  }

  // Step 6.
  // Start with NormalCompletion(undefined).
  if (!emit1(JSOp::Undefined)) {
    //              [stack] NEXT ITER RECEIVED
    return false;
  }
  if (!emitPushResumeKind(GeneratorResumeKind::Next)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }

  const int32_t startDepth = bytecodeSection().stackDepth();
  MOZ_ASSERT(startDepth >= 4);

  // Step 7 is a loop.
  LoopControl loopInfo(this, StatementKind::YieldStar);
  if (!loopInfo.emitLoopHead(this, Nothing())) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }

  // Step 7.a. Check for Normal completion.
  if (!emit1(JSOp::Dup)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND RESUMEKIND
    return false;
  }
  if (!emitPushResumeKind(GeneratorResumeKind::Next)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND RESUMEKIND NORMAL
    return false;
  }
  if (!emit1(JSOp::StrictEq)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND IS_NORMAL
    return false;
  }

  InternalIfEmitter ifKind(this);
  if (!ifKind.emitThenElse()) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }
  {
    if (!emit1(JSOp::Pop)) {
      //            [stack] NEXT ITER RECEIVED
      return false;
    }

    // Step 7.a.i.
    // result = iter.next(received)
    if (!emit2(JSOp::Unpick, 2)) {
      //            [stack] RECEIVED NEXT ITER
      return false;
    }
    if (!emit1(JSOp::Dup2)) {
      //            [stack] RECEIVED NEXT ITER NEXT ITER
      return false;
    }
    if (!emit2(JSOp::Pick, 4)) {
      //            [stack] NEXT ITER NEXT ITER RECEIVED
      return false;
    }
    if (!emitCall(JSOp::Call, 1, iter)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Step 7.a.ii.
    if (iterKind == IteratorKind::Async) {
      if (!emitAwaitInInnermostScope()) {
        //          [stack] NEXT ITER RESULT
        return false;
      }
    }

    // Step 7.a.iii.
    if (!emitCheckIsObj(CheckIsObjectKind::IteratorNext)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Bytecode for steps 7.a.iv-vii is emitted after the ifKind if-else because
    // it's shared with other branches.
  }

  // Step 7.b. Check for Throw completion.
  if (!ifKind.emitElseIf(Nothing())) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }
  if (!emit1(JSOp::Dup)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND RESUMEKIND
    return false;
  }
  if (!emitPushResumeKind(GeneratorResumeKind::Throw)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND RESUMEKIND THROW
    return false;
  }
  if (!emit1(JSOp::StrictEq)) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND IS_THROW
    return false;
  }
  if (!ifKind.emitThenElse()) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }
  {
    if (!emit1(JSOp::Pop)) {
      //            [stack] NEXT ITER RECEIVED
      return false;
    }
    // Step 7.b.i.
    if (!emitDupAt(1)) {
      //            [stack] NEXT ITER RECEIVED ITER
      return false;
    }
    if (!emit1(JSOp::Dup)) {
      //            [stack] NEXT ITER RECEIVED ITER ITER
      return false;
    }
    if (!emitAtomOp(JSOp::GetProp,
                    TaggedParserAtomIndex::WellKnown::throw_())) {
      //            [stack] NEXT ITER RECEIVED ITER THROW
      return false;
    }

    // Step 7.b.ii.
    InternalIfEmitter ifThrowMethodIsNotDefined(this);
    if (!emit1(JSOp::IsNullOrUndefined)) {
      //            [stack] NEXT ITER RECEIVED ITER THROW NULL-OR-UNDEF
      return false;
    }

    if (!ifThrowMethodIsNotDefined.emitThenElse(
            IfEmitter::ConditionKind::Negative)) {
      //            [stack] NEXT ITER RECEIVED ITER THROW
      return false;
    }

    // Step 7.b.ii.1.
    // RESULT = ITER.throw(EXCEPTION)
    if (!emit1(JSOp::Swap)) {
      //            [stack] NEXT ITER RECEIVED THROW ITER
      return false;
    }
    if (!emit2(JSOp::Pick, 2)) {
      //            [stack] NEXT ITER THROW ITER RECEIVED
      return false;
    }
    if (!emitCall(JSOp::Call, 1, iter)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Step 7.b.ii.2.
    if (iterKind == IteratorKind::Async) {
      if (!emitAwaitInInnermostScope()) {
        //          [stack] NEXT ITER RESULT
        return false;
      }
    }

    // Step 7.b.ii.4.
    if (!emitCheckIsObj(CheckIsObjectKind::IteratorThrow)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Bytecode for steps 7.b.ii.5-8 is emitted after the ifKind if-else because
    // it's shared with other branches.

    // Step 7.b.iii.
    if (!ifThrowMethodIsNotDefined.emitElse()) {
      //            [stack] NEXT ITER RECEIVED ITER THROW
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      //            [stack] NEXT ITER RECEIVED ITER
      return false;
    }

    // Steps 7.b.iii.1-4.
    //
    // If the iterator does not have a "throw" method, it calls IteratorClose
    // and then throws a TypeError.
    if (!emitIteratorCloseInInnermostScope(iterKind, CompletionKind::Normal,
                                           getSelfHostedIterFor(iter))) {
      //            [stack] NEXT ITER RECEIVED ITER
      return false;
    }
    // Steps 7.b.iii.5-6.
    if (!emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::IteratorNoThrow))) {
      //            [stack] NEXT ITER RECEIVED ITER
      //            [stack] # throw
      return false;
    }

    if (!ifThrowMethodIsNotDefined.emitEnd()) {
      return false;
    }
  }

  // Step 7.c. It must be a Return completion.
  if (!ifKind.emitElse()) {
    //              [stack] NEXT ITER RECEIVED RESUMEKIND
    return false;
  }
  {
    if (!emit1(JSOp::Pop)) {
      //            [stack] NEXT ITER RECEIVED
      return false;
    }

    // Step 7.c.i.
    //
    // Call iterator.return() for receiving a "forced return" completion from
    // the generator.

    // Step 7.c.ii.
    //
    // Get the "return" method.
    if (!emitDupAt(1)) {
      //            [stack] NEXT ITER RECEIVED ITER
      return false;
    }
    if (!emit1(JSOp::Dup)) {
      //            [stack] NEXT ITER RECEIVED ITER ITER
      return false;
    }
    if (!emitAtomOp(JSOp::GetProp,
                    TaggedParserAtomIndex::WellKnown::return_())) {
      //            [stack] NEXT ITER RECEIVED ITER RET
      return false;
    }

    // Step 7.c.iii.
    //
    // Do nothing if "return" is undefined or null.
    InternalIfEmitter ifReturnMethodIsDefined(this);
    if (!emit1(JSOp::IsNullOrUndefined)) {
      //            [stack] NEXT ITER RECEIVED ITER RET NULL-OR-UNDEF
      return false;
    }

    // Step 7.c.iv.
    //
    // Call "return" with the argument passed to Generator.prototype.return.
    if (!ifReturnMethodIsDefined.emitThenElse(
            IfEmitter::ConditionKind::Negative)) {
      //            [stack] NEXT ITER RECEIVED ITER RET
      return false;
    }
    if (!emit1(JSOp::Swap)) {
      //            [stack] NEXT ITER RECEIVED RET ITER
      return false;
    }
    if (!emit2(JSOp::Pick, 2)) {
      //            [stack] NEXT ITER RET ITER RECEIVED
      return false;
    }
    if (needsIteratorResult) {
      if (!emitAtomOp(JSOp::GetProp,
                      TaggedParserAtomIndex::WellKnown::value())) {
        //          [stack] NEXT ITER RET ITER VAL
        return false;
      }
    }
    if (!emitCall(JSOp::Call, 1)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Step 7.c.v.
    if (iterKind == IteratorKind::Async) {
      if (!emitAwaitInInnermostScope()) {
        //          [stack] NEXT ITER RESULT
        return false;
      }
    }

    // Step 7.c.vi.
    if (!emitCheckIsObj(CheckIsObjectKind::IteratorReturn)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Check if the returned object from iterator.return() is done. If not,
    // continue yielding.

    // Steps 7.c.vii-viii.
    InternalIfEmitter ifReturnDone(this);
    if (!emit1(JSOp::Dup)) {
      //            [stack] NEXT ITER RESULT RESULT
      return false;
    }
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::done())) {
      //            [stack] NEXT ITER RESULT DONE
      return false;
    }
    if (!ifReturnDone.emitThenElse()) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Step 7.c.viii.1.
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::value())) {
      //            [stack] NEXT ITER VALUE
      return false;
    }
    if (needsIteratorResult) {
      if (!emitPrepareIteratorResult()) {
        //          [stack] NEXT ITER VALUE RESULT
        return false;
      }
      if (!emit1(JSOp::Swap)) {
        //          [stack] NEXT ITER RESULT VALUE
        return false;
      }
      if (!emitFinishIteratorResult(true)) {
        //          [stack] NEXT ITER RESULT
        return false;
      }
    }

    if (!ifReturnDone.emitElse()) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Jump to continue label for steps 7.c.ix-x.
    if (!emitJump(JSOp::Goto, &loopInfo.continues)) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    if (!ifReturnDone.emitEnd()) {
      //            [stack] NEXT ITER RESULT
      return false;
    }

    // Step 7.c.iii.
    if (!ifReturnMethodIsDefined.emitElse()) {
      //            [stack] NEXT ITER RECEIVED ITER RET
      return false;
    }
    if (!emitPopN(2)) {
      //            [stack] NEXT ITER RECEIVED
      return false;
    }
    if (iterKind == IteratorKind::Async) {
      // Step 7.c.iii.1.
      if (!emitAwaitInInnermostScope()) {
        //          [stack] NEXT ITER RECEIVED
        return false;
      }
    }
    if (!ifReturnMethodIsDefined.emitEnd()) {
      //            [stack] NEXT ITER RECEIVED
      return false;
    }

    // Perform a "forced generator return".
    //
    // Step 7.c.iii.2.
    // Step 7.c.viii.2.
    if (!emitGetDotGeneratorInInnermostScope()) {
      //            [stack] NEXT ITER RESULT GENOBJ
      return false;
    }
    if (!emitPushResumeKind(GeneratorResumeKind::Return)) {
      //            [stack] NEXT ITER RESULT GENOBJ RESUMEKIND
      return false;
    }
    if (!emit1(JSOp::CheckResumeKind)) {
      //            [stack] NEXT ITER RESULT GENOBJ RESUMEKIND
      return false;
    }
  }

  if (!ifKind.emitEnd()) {
    //              [stack] NEXT ITER RESULT
    return false;
  }

  // Shared tail for Normal/Throw completions.
  //
  // Steps 7.a.iv-v.
  // Steps 7.b.ii.5-6.
  //
  //                [stack] NEXT ITER RESULT

  // if (result.done) break;
  if (!emit1(JSOp::Dup)) {
    //              [stack] NEXT ITER RESULT RESULT
    return false;
  }
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::done())) {
    //              [stack] NEXT ITER RESULT DONE
    return false;
  }
  if (!emitJump(JSOp::JumpIfTrue, &loopInfo.breaks)) {
    //              [stack] NEXT ITER RESULT
    return false;
  }

  // Steps 7.a.vi-vii.
  // Steps 7.b.ii.7-8.
  // Steps 7.c.ix-x.
  if (!loopInfo.emitContinueTarget(this)) {
    //              [stack] NEXT ITER RESULT
    return false;
  }
  if (iterKind == IteratorKind::Async) {
    if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::value())) {
      //            [stack] NEXT ITER RESULT
      return false;
    }
  }
  if (!emitGetDotGeneratorInInnermostScope()) {
    //              [stack] NEXT ITER RESULT GENOBJ
    return false;
  }
  if (!emitYieldOp(JSOp::Yield)) {
    //              [stack] NEXT ITER RVAL GENOBJ RESUMEKIND
    return false;
  }
  if (!emit1(JSOp::Swap)) {
    //              [stack] NEXT ITER RVAL RESUMEKIND GENOBJ
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack] NEXT ITER RVAL RESUMEKIND
    return false;
  }
  if (!loopInfo.emitLoopEnd(this, JSOp::Goto, TryNoteKind::Loop)) {
    //              [stack] NEXT ITER RVAL RESUMEKIND
    return false;
  }

  // Jumps to this point have 3 (instead of 4) values on the stack.
  MOZ_ASSERT(bytecodeSection().stackDepth() == startDepth);
  bytecodeSection().setStackDepth(startDepth - 1);

  //                [stack] NEXT ITER RESULT

  // Step 7.a.v.1.
  // Step 7.b.ii.6.a.
  //
  // result.value
  if (!emit2(JSOp::Unpick, 2)) {
    //              [stack] RESULT NEXT ITER
    return false;
  }
  if (!emitPopN(2)) {
    //              [stack] RESULT
    return false;
  }
  if (!emitAtomOp(JSOp::GetProp, TaggedParserAtomIndex::WellKnown::value())) {
    //              [stack] VALUE
    return false;
  }

  MOZ_ASSERT(bytecodeSection().stackDepth() == startDepth - 3);

  return true;
}

bool BytecodeEmitter::emitStatementList(ListNode* stmtList) {
  for (ParseNode* stmt : stmtList->contents()) {
    if (!emitTree(stmt)) {
      return false;
    }
  }
  return true;
}

bool BytecodeEmitter::emitExpressionStatement(UnaryNode* exprStmt) {
  MOZ_ASSERT(exprStmt->isKind(ParseNodeKind::ExpressionStmt));

  /*
   * Top-level or called-from-a-native JS_Execute/EvaluateScript,
   * debugger, and eval frames may need the value of the ultimate
   * expression statement as the script's result, despite the fact
   * that it appears useless to the compiler.
   *
   * API users may also set the JSOPTION_NO_SCRIPT_RVAL option when
   * calling JS_Compile* to suppress JSOp::SetRval.
   */
  bool wantval = false;
  bool useful = false;
  if (sc->isTopLevelContext()) {
    useful = wantval = !sc->noScriptRval();
  }

  /* Don't eliminate expressions with side effects. */
  ParseNode* expr = exprStmt->kid();
  if (!useful) {
    if (!checkSideEffects(expr, &useful)) {
      return false;
    }

    /*
     * Don't eliminate apparently useless expressions if they are labeled
     * expression statements. The startOffset() test catches the case
     * where we are nesting in emitTree for a labeled compound statement.
     */
    if (innermostNestableControl &&
        innermostNestableControl->is<LabelControl>() &&
        innermostNestableControl->as<LabelControl>().startOffset() >=
            bytecodeSection().offset()) {
      useful = true;
    }
  }

  if (useful) {
    ValueUsage valueUsage =
        wantval ? ValueUsage::WantValue : ValueUsage::IgnoreValue;
    ExpressionStatementEmitter ese(this, valueUsage);
    if (!ese.prepareForExpr(exprStmt->pn_pos.begin)) {
      return false;
    }
    if (!markStepBreakpoint()) {
      return false;
    }
    if (!emitTree(expr, valueUsage)) {
      return false;
    }
    if (!ese.emitEnd()) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitDeleteName(UnaryNode* deleteNode) {
  MOZ_ASSERT(deleteNode->isKind(ParseNodeKind::DeleteNameExpr));

  NameNode* nameExpr = &deleteNode->kid()->as<NameNode>();
  MOZ_ASSERT(nameExpr->isKind(ParseNodeKind::Name));

  return emitAtomOp(JSOp::DelName, nameExpr->atom());
}

bool BytecodeEmitter::emitDeleteProperty(UnaryNode* deleteNode) {
  MOZ_ASSERT(deleteNode->isKind(ParseNodeKind::DeletePropExpr));

  PropertyAccess* propExpr = &deleteNode->kid()->as<PropertyAccess>();
  PropOpEmitter poe(this, PropOpEmitter::Kind::Delete,
                    propExpr->as<PropertyAccess>().isSuper()
                        ? PropOpEmitter::ObjKind::Super
                        : PropOpEmitter::ObjKind::Other);
  if (propExpr->isSuper()) {
    // The expression |delete super.foo;| has to evaluate |super.foo|,
    // which could throw if |this| hasn't yet been set by a |super(...)|
    // call or the super-base is not an object, before throwing a
    // ReferenceError for attempting to delete a super-reference.
    UnaryNode* base = &propExpr->expression().as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] THIS
      return false;
    }
  } else {
    if (!poe.prepareForObj()) {
      return false;
    }
    if (!emitPropLHS(propExpr)) {
      //            [stack] OBJ
      return false;
    }
  }

  if (!poe.emitDelete(propExpr->key().atom())) {
    //              [stack] # if Super
    //              [stack] THIS
    //              [stack] # otherwise
    //              [stack] SUCCEEDED
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDeleteElement(UnaryNode* deleteNode) {
  MOZ_ASSERT(deleteNode->isKind(ParseNodeKind::DeleteElemExpr));

  PropertyByValue* elemExpr = &deleteNode->kid()->as<PropertyByValue>();
  bool isSuper = elemExpr->isSuper();
  DebugOnly<bool> isPrivate =
      elemExpr->key().isKind(ParseNodeKind::PrivateName);
  MOZ_ASSERT(!isPrivate);
  ElemOpEmitter eoe(
      this, ElemOpEmitter::Kind::Delete,
      isSuper ? ElemOpEmitter::ObjKind::Super : ElemOpEmitter::ObjKind::Other);
  if (isSuper) {
    // The expression |delete super[foo];| has to evaluate |super[foo]|,
    // which could throw if |this| hasn't yet been set by a |super(...)|
    // call, or trigger side-effects when evaluating ToPropertyKey(foo),
    // or also throw when the super-base is not an object, before throwing
    // a ReferenceError for attempting to delete a super-reference.
    if (!eoe.prepareForObj()) {
      //            [stack]
      return false;
    }

    UnaryNode* base = &elemExpr->expression().as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] THIS
      return false;
    }
    if (!eoe.prepareForKey()) {
      //            [stack] THIS
      return false;
    }
    if (!emitTree(&elemExpr->key())) {
      //            [stack] THIS KEY
      return false;
    }
  } else {
    if (!emitElemObjAndKey(elemExpr, false, eoe)) {
      //            [stack] OBJ KEY
      return false;
    }
  }
  if (!eoe.emitDelete()) {
    //              [stack] # if Super
    //              [stack] THIS
    //              [stack] # otherwise
    //              [stack] SUCCEEDED
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDeleteExpression(UnaryNode* deleteNode) {
  MOZ_ASSERT(deleteNode->isKind(ParseNodeKind::DeleteExpr));

  ParseNode* expression = deleteNode->kid();

  // If useless, just emit JSOp::True; otherwise convert |delete <expr>| to
  // effectively |<expr>, true|.
  bool useful = false;
  if (!checkSideEffects(expression, &useful)) {
    return false;
  }

  if (useful) {
    if (!emitTree(expression)) {
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      return false;
    }
  }

  return emit1(JSOp::True);
}

bool BytecodeEmitter::emitDeleteOptionalChain(UnaryNode* deleteNode) {
  MOZ_ASSERT(deleteNode->isKind(ParseNodeKind::DeleteOptionalChainExpr));

  OptionalEmitter oe(this, bytecodeSection().stackDepth());

  ParseNode* kid = deleteNode->kid();
  switch (kid->getKind()) {
    case ParseNodeKind::ElemExpr:
    case ParseNodeKind::OptionalElemExpr: {
      auto* elemExpr = &kid->as<PropertyByValueBase>();
      if (!emitDeleteElementInOptChain(elemExpr, oe)) {
        //          [stack] # If shortcircuit
        //          [stack] UNDEFINED-OR-NULL
        //          [stack] # otherwise
        //          [stack] SUCCEEDED
        return false;
      }

      break;
    }
    case ParseNodeKind::DotExpr:
    case ParseNodeKind::OptionalDotExpr: {
      auto* propExpr = &kid->as<PropertyAccessBase>();
      if (!emitDeletePropertyInOptChain(propExpr, oe)) {
        //          [stack] # If shortcircuit
        //          [stack] UNDEFINED-OR-NULL
        //          [stack] # otherwise
        //          [stack] SUCCEEDED
        return false;
      }
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE("Unrecognized optional delete ParseNodeKind");
  }

  if (!oe.emitOptionalJumpTarget(JSOp::True)) {
    //              [stack] # If shortcircuit
    //              [stack] TRUE
    //              [stack] # otherwise
    //              [stack] SUCCEEDED
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDeletePropertyInOptChain(PropertyAccessBase* propExpr,
                                                   OptionalEmitter& oe) {
  MOZ_ASSERT_IF(propExpr->is<PropertyAccess>(),
                !propExpr->as<PropertyAccess>().isSuper());
  PropOpEmitter poe(this, PropOpEmitter::Kind::Delete,
                    PropOpEmitter::ObjKind::Other);

  if (!poe.prepareForObj()) {
    //              [stack]
    return false;
  }
  if (!emitOptionalTree(&propExpr->expression(), oe)) {
    //              [stack] OBJ
    return false;
  }
  if (propExpr->isKind(ParseNodeKind::OptionalDotExpr)) {
    if (!oe.emitJumpShortCircuit()) {
      //            [stack] # if Jump
      //            [stack] UNDEFINED-OR-NULL
      //            [stack] # otherwise
      //            [stack] OBJ
      return false;
    }
  }

  if (!poe.emitDelete(propExpr->key().atom())) {
    //              [stack] SUCCEEDED
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDeleteElementInOptChain(PropertyByValueBase* elemExpr,
                                                  OptionalEmitter& oe) {
  MOZ_ASSERT_IF(elemExpr->is<PropertyByValue>(),
                !elemExpr->as<PropertyByValue>().isSuper());
  ElemOpEmitter eoe(this, ElemOpEmitter::Kind::Delete,
                    ElemOpEmitter::ObjKind::Other);

  if (!eoe.prepareForObj()) {
    //              [stack]
    return false;
  }

  if (!emitOptionalTree(&elemExpr->expression(), oe)) {
    //              [stack] OBJ
    return false;
  }

  if (elemExpr->isKind(ParseNodeKind::OptionalElemExpr)) {
    if (!oe.emitJumpShortCircuit()) {
      //            [stack] # if Jump
      //            [stack] UNDEFINED-OR-NULL
      //            [stack] # otherwise
      //            [stack] OBJ
      return false;
    }
  }

  if (!eoe.prepareForKey()) {
    //              [stack] OBJ
    return false;
  }

  if (!emitTree(&elemExpr->key())) {
    //              [stack] OBJ KEY
    return false;
  }

  if (!eoe.emitDelete()) {
    //              [stack] SUCCEEDED
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDebugCheckSelfHosted() {
  //                [stack] CALLEE

#ifdef DEBUG
  if (!emit1(JSOp::DebugCheckSelfHosted)) {
    //              [stack] CALLEE
    return false;
  }
#endif

  return true;
}

bool BytecodeEmitter::emitSelfHostedCallFunction(CallNode* callNode, JSOp op) {
  // Special-casing of callFunction to emit bytecode that directly
  // invokes the callee with the correct |this| object and arguments.
  // callFunction(fun, thisArg, arg0, arg1) thus becomes:
  // - emit lookup for fun
  // - emit lookup for thisArg
  // - emit lookups for arg0, arg1
  //
  // argc is set to the amount of actually emitted args and the
  // emitting of args below is disabled by setting emitArgs to false.
  NameNode* calleeNode = &callNode->left()->as<NameNode>();
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() >= 2);

  MOZ_ASSERT(callNode->callOp() == JSOp::Call);

  bool constructing =
      calleeNode->name() ==
      TaggedParserAtomIndex::WellKnown::constructContentFunction();
  ParseNode* funNode = argsList->head();

  if (!emitTree(funNode)) {
    //              [stack] CALLEE
    return false;
  }

#ifdef DEBUG
  MOZ_ASSERT(op == JSOp::Call || op == JSOp::CallContent ||
             op == JSOp::NewContent);
  if (op == JSOp::Call) {
    if (!emitDebugCheckSelfHosted()) {
      //            [stack] CALLEE
      return false;
    }
  }
#endif

  ParseNode* thisOrNewTarget = funNode->pn_next;
  if (constructing) {
    // Save off the new.target value, but here emit a proper |this| for a
    // constructing call.
    if (!emit1(JSOp::IsConstructing)) {
      //            [stack] CALLEE IS_CONSTRUCTING
      return false;
    }
  } else {
    // It's |this|, emit it.
    if (!emitTree(thisOrNewTarget)) {
      //            [stack] CALLEE THIS
      return false;
    }
  }

  for (ParseNode* argpn : argsList->contentsFrom(thisOrNewTarget->pn_next)) {
    if (!emitTree(argpn)) {
      //            [stack] CALLEE ... ARGS...
      return false;
    }
  }

  if (constructing) {
    if (!emitTree(thisOrNewTarget)) {
      //            [stack] CALLEE IS_CONSTRUCTING ARGS... NEW.TARGET
      return false;
    }
  }

  uint32_t argc = argsList->count() - 2;
  if (!emitCall(op, argc)) {
    //              [stack] RVAL
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitSelfHostedResumeGenerator(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  // Syntax: resumeGenerator(gen, value, 'next'|'throw'|'return')
  MOZ_ASSERT(argsList->count() == 3);

  ParseNode* genNode = argsList->head();
  if (!emitTree(genNode)) {
    //              [stack] GENERATOR
    return false;
  }

  ParseNode* valNode = genNode->pn_next;
  if (!emitTree(valNode)) {
    //              [stack] GENERATOR VALUE
    return false;
  }

  ParseNode* kindNode = valNode->pn_next;
  MOZ_ASSERT(kindNode->isKind(ParseNodeKind::StringExpr));
  GeneratorResumeKind kind =
      ParserAtomToResumeKind(kindNode->as<NameNode>().atom());
  MOZ_ASSERT(!kindNode->pn_next);

  if (!emitPushResumeKind(kind)) {
    //              [stack] GENERATOR VALUE RESUMEKIND
    return false;
  }

  if (!emit1(JSOp::Resume)) {
    //              [stack] RVAL
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitSelfHostedForceInterpreter() {
  // JSScript::hasForceInterpreterOp() relies on JSOp::ForceInterpreter being
  // the first bytecode op in the script.
  MOZ_ASSERT(bytecodeSection().code().empty());

  if (!emit1(JSOp::ForceInterpreter)) {
    return false;
  }
  if (!emit1(JSOp::Undefined)) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitSelfHostedAllowContentIter(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  // We're just here as a sentinel. Pass the value through directly.
  return emitTree(argsList->head());
}

bool BytecodeEmitter::emitSelfHostedAllowContentIterWith(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 2 || argsList->count() == 3);

  // We're just here as a sentinel. Pass the value through directly.
  return emitTree(argsList->head());
}

bool BytecodeEmitter::emitSelfHostedDefineDataProperty(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  // Only optimize when 3 arguments are passed.
  MOZ_ASSERT(argsList->count() == 3);

  ParseNode* objNode = argsList->head();
  if (!emitTree(objNode)) {
    return false;
  }

  ParseNode* idNode = objNode->pn_next;
  if (!emitTree(idNode)) {
    return false;
  }

  ParseNode* valNode = idNode->pn_next;
  if (!emitTree(valNode)) {
    return false;
  }

  // This will leave the object on the stack instead of pushing |undefined|,
  // but that's fine because the self-hosted code doesn't use the return
  // value.
  return emit1(JSOp::InitElem);
}

bool BytecodeEmitter::emitSelfHostedHasOwn(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 2);

  ParseNode* idNode = argsList->head();
  if (!emitTree(idNode)) {
    return false;
  }

  ParseNode* objNode = idNode->pn_next;
  if (!emitTree(objNode)) {
    return false;
  }

  return emit1(JSOp::HasOwn);
}

bool BytecodeEmitter::emitSelfHostedGetPropertySuper(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 3);

  ParseNode* objNode = argsList->head();
  ParseNode* idNode = objNode->pn_next;
  ParseNode* receiverNode = idNode->pn_next;

  if (!emitTree(receiverNode)) {
    return false;
  }

  if (!emitTree(idNode)) {
    return false;
  }

  if (!emitTree(objNode)) {
    return false;
  }

  return emitElemOpBase(JSOp::GetElemSuper);
}

bool BytecodeEmitter::emitSelfHostedToNumeric(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();

  if (!emitTree(argNode)) {
    return false;
  }

  return emit1(JSOp::ToNumeric);
}

bool BytecodeEmitter::emitSelfHostedToString(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();

  if (!emitTree(argNode)) {
    return false;
  }

  return emit1(JSOp::ToString);
}

bool BytecodeEmitter::emitSelfHostedIsNullOrUndefined(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();

  if (!emitTree(argNode)) {
    //              [stack] ARG
    return false;
  }
  if (!emit1(JSOp::IsNullOrUndefined)) {
    //              [stack] ARG IS_NULL_OR_UNDEF
    return false;
  }
  if (!emit1(JSOp::Swap)) {
    //              [stack] IS_NULL_OR_UNDEF ARG
    return false;
  }
  if (!emit1(JSOp::Pop)) {
    //              [stack] IS_NULL_OR_UNDEF
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitSelfHostedGetBuiltinConstructorOrPrototype(
    CallNode* callNode, bool isConstructor) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();

  if (!argNode->isKind(ParseNodeKind::StringExpr)) {
    reportError(callNode, JSMSG_UNEXPECTED_TYPE, "built-in name",
                "not a string constant");
    return false;
  }

  auto name = argNode->as<NameNode>().atom();

  BuiltinObjectKind kind;
  if (isConstructor) {
    kind = BuiltinConstructorForName(name);
  } else {
    kind = BuiltinPrototypeForName(name);
  }

  if (kind == BuiltinObjectKind::None) {
    reportError(callNode, JSMSG_UNEXPECTED_TYPE, "built-in name",
                "not a valid built-in");
    return false;
  }

  return emitBuiltinObject(kind);
}

bool BytecodeEmitter::emitSelfHostedGetBuiltinConstructor(CallNode* callNode) {
  return emitSelfHostedGetBuiltinConstructorOrPrototype(
      callNode, /* isConstructor = */ true);
}

bool BytecodeEmitter::emitSelfHostedGetBuiltinPrototype(CallNode* callNode) {
  return emitSelfHostedGetBuiltinConstructorOrPrototype(
      callNode, /* isConstructor = */ false);
}

JS::SymbolCode ParserAtomToSymbolCode(TaggedParserAtomIndex atom) {
  // NOTE: This is a linear search, but the set of entries is quite small and
  // this is only used for initial self-hosted parse.
#define MATCH_WELL_KNOWN_SYMBOL(NAME)                     \
  if (atom == TaggedParserAtomIndex::WellKnown::NAME()) { \
    return JS::SymbolCode::NAME;                          \
  }
  JS_FOR_EACH_WELL_KNOWN_SYMBOL(MATCH_WELL_KNOWN_SYMBOL)
#undef MATCH_WELL_KNOWN_SYMBOL

  return JS::SymbolCode::Limit;
}

bool BytecodeEmitter::emitSelfHostedGetBuiltinSymbol(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();

  if (!argNode->isKind(ParseNodeKind::StringExpr)) {
    reportError(callNode, JSMSG_UNEXPECTED_TYPE, "built-in name",
                "not a string constant");
    return false;
  }

  auto name = argNode->as<NameNode>().atom();

  JS::SymbolCode code = ParserAtomToSymbolCode(name);
  if (code == JS::SymbolCode::Limit) {
    reportError(callNode, JSMSG_UNEXPECTED_TYPE, "built-in name",
                "not a valid built-in");
    return false;
  }

  return emit2(JSOp::Symbol, uint8_t(code));
}

bool BytecodeEmitter::emitSelfHostedArgumentsLength(CallNode* callNode) {
  MOZ_ASSERT(!sc->asFunctionBox()->needsArgsObj());
  sc->asFunctionBox()->setUsesArgumentsIntrinsics();

  MOZ_ASSERT(callNode->right()->as<ListNode>().count() == 0);

  return emit1(JSOp::ArgumentsLength);
}

bool BytecodeEmitter::emitSelfHostedGetArgument(CallNode* callNode) {
  MOZ_ASSERT(!sc->asFunctionBox()->needsArgsObj());
  sc->asFunctionBox()->setUsesArgumentsIntrinsics();

  ListNode* argsList = &callNode->right()->as<ListNode>();
  MOZ_ASSERT(argsList->count() == 1);

  ParseNode* argNode = argsList->head();
  if (!emitTree(argNode)) {
    return false;
  }

  return emit1(JSOp::GetActualArg);
}

#ifdef DEBUG
void BytecodeEmitter::assertSelfHostedExpectedTopLevel(ParseNode* node) {
  // The function argument is expected to be a simple binding/function name.
  // Eg. `function foo() { }; SpecialIntrinsic(foo)`
  MOZ_ASSERT(node->isKind(ParseNodeKind::Name),
             "argument must be a function name");
  TaggedParserAtomIndex targetName = node->as<NameNode>().name();

  // The special intrinsics must follow the target functions definition. A
  // simple assert is fine here since any hoisted function will cause a non-null
  // value to be set here.
  MOZ_ASSERT(prevSelfHostedTopLevelFunction);

  // The target function must match the most recently defined top-level
  // self-hosted function.
  MOZ_ASSERT(prevSelfHostedTopLevelFunction->explicitName() == targetName,
             "selfhost decorator must immediately follow target function");
}
#endif

bool BytecodeEmitter::emitSelfHostedSetIsInlinableLargeFunction(
    CallNode* callNode) {
#ifdef DEBUG
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 1);

  assertSelfHostedExpectedTopLevel(argsList->head());
#endif

  MOZ_ASSERT(prevSelfHostedTopLevelFunction->isInitialCompilation);
  prevSelfHostedTopLevelFunction->setIsInlinableLargeFunction();

  // This is still a call node, so we must generate a stack value.
  return emit1(JSOp::Undefined);
}

bool BytecodeEmitter::emitSelfHostedSetCanonicalName(CallNode* callNode) {
  ListNode* argsList = &callNode->right()->as<ListNode>();

  MOZ_ASSERT(argsList->count() == 2);

#ifdef DEBUG
  assertSelfHostedExpectedTopLevel(argsList->head());
#endif

  ParseNode* nameNode = argsList->last();
  MOZ_ASSERT(nameNode->isKind(ParseNodeKind::StringExpr));
  TaggedParserAtomIndex specName = nameNode->as<NameNode>().atom();
  // Canonical name must be atomized.
  compilationState.parserAtoms.markUsedByStencil(specName,
                                                 ParserAtom::Atomize::Yes);

  // Store the canonical name for instantiation.
  prevSelfHostedTopLevelFunction->functionStencil().setSelfHostedCanonicalName(
      specName);

  return emit1(JSOp::Undefined);
}

#ifdef DEBUG
void BytecodeEmitter::assertSelfHostedUnsafeGetReservedSlot(
    ListNode* argsList) {
  MOZ_ASSERT(argsList->count() == 2);

  ParseNode* objNode = argsList->head();
  ParseNode* slotNode = objNode->pn_next;

  // Ensure that the slot argument is fixed, this is required by the JITs.
  MOZ_ASSERT(slotNode->isKind(ParseNodeKind::NumberExpr),
             "slot argument must be a constant");
}

void BytecodeEmitter::assertSelfHostedUnsafeSetReservedSlot(
    ListNode* argsList) {
  MOZ_ASSERT(argsList->count() == 3);

  ParseNode* objNode = argsList->head();
  ParseNode* slotNode = objNode->pn_next;

  // Ensure that the slot argument is fixed, this is required by the JITs.
  MOZ_ASSERT(slotNode->isKind(ParseNodeKind::NumberExpr),
             "slot argument must be a constant");
}
#endif

/* A version of emitCalleeAndThis for the optional cases:
 *   * a?.()
 *   * a?.b()
 *   * a?.["b"]()
 *   * (a?.b)()
 *   * a?.#b()
 *
 * See emitCallOrNew and emitOptionalCall for more context.
 */
bool BytecodeEmitter::emitOptionalCalleeAndThis(ParseNode* callee,
                                                CallNode* call,
                                                CallOrNewEmitter& cone,
                                                OptionalEmitter& oe) {
  AutoCheckRecursionLimit recursion(fc);
  if (!recursion.check(fc)) {
    return false;
  }

  switch (ParseNodeKind kind = callee->getKind()) {
    case ParseNodeKind::Name: {
      auto name = callee->as<NameNode>().name();
      if (!cone.emitNameCallee(name)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }

    case ParseNodeKind::OptionalDotExpr: {
      MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
      OptionalPropertyAccess* prop = &callee->as<OptionalPropertyAccess>();
      bool isSuper = false;

      PropOpEmitter& poe = cone.prepareForPropCallee(isSuper);
      if (!emitOptionalDotExpression(prop, poe, isSuper, oe)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }
    case ParseNodeKind::DotExpr: {
      MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
      PropertyAccess* prop = &callee->as<PropertyAccess>();
      bool isSuper = prop->isSuper();

      PropOpEmitter& poe = cone.prepareForPropCallee(isSuper);
      if (!emitOptionalDotExpression(prop, poe, isSuper, oe)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }

    case ParseNodeKind::OptionalElemExpr: {
      OptionalPropertyByValue* elem = &callee->as<OptionalPropertyByValue>();
      bool isSuper = false;
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter& eoe = cone.prepareForElemCallee(isSuper);
      if (!emitOptionalElemExpression(elem, eoe, isSuper, oe)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }
    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &callee->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter& eoe = cone.prepareForElemCallee(isSuper);
      if (!emitOptionalElemExpression(elem, eoe, isSuper, oe)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }

    case ParseNodeKind::PrivateMemberExpr:
    case ParseNodeKind::OptionalPrivateMemberExpr: {
      PrivateMemberAccessBase* privateExpr =
          &callee->as<PrivateMemberAccessBase>();
      PrivateOpEmitter& xoe =
          cone.prepareForPrivateCallee(privateExpr->privateName().name());
      if (!emitOptionalPrivateExpression(privateExpr, xoe, oe)) {
        //          [stack] CALLEE THIS
        return false;
      }
      break;
    }

    case ParseNodeKind::Function:
      if (!cone.prepareForFunctionCallee()) {
        return false;
      }
      if (!emitOptionalTree(callee, oe)) {
        //          [stack] CALLEE
        return false;
      }
      break;

    case ParseNodeKind::OptionalChain: {
      return emitCalleeAndThisForOptionalChain(&callee->as<UnaryNode>(), call,
                                               cone);
    }

    default:
      MOZ_RELEASE_ASSERT(kind != ParseNodeKind::SuperBase);

      if (!cone.prepareForOtherCallee()) {
        return false;
      }
      if (!emitOptionalTree(callee, oe)) {
        //          [stack] CALLEE
        return false;
      }
      break;
  }

  if (!cone.emitThis()) {
    //              [stack] CALLEE THIS
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitCalleeAndThis(ParseNode* callee, ParseNode* call,
                                        CallOrNewEmitter& cone) {
  switch (callee->getKind()) {
    case ParseNodeKind::Name: {
      auto name = callee->as<NameNode>().name();
      if (!cone.emitNameCallee(name)) {
        //          [stack] CALLEE THIS?
        return false;
      }
      break;
    }
    case ParseNodeKind::DotExpr: {
      MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
      PropertyAccess* prop = &callee->as<PropertyAccess>();
      bool isSuper = prop->isSuper();

      PropOpEmitter& poe = cone.prepareForPropCallee(isSuper);
      if (!poe.prepareForObj()) {
        return false;
      }
      if (isSuper) {
        UnaryNode* base = &prop->expression().as<UnaryNode>();
        if (!emitGetThisForSuperBase(base)) {
          //        [stack] THIS
          return false;
        }
      } else {
        if (!emitPropLHS(prop)) {
          //        [stack] OBJ
          return false;
        }
      }
      if (!poe.emitGet(prop->key().atom())) {
        //          [stack] CALLEE THIS?
        return false;
      }

      break;
    }
    case ParseNodeKind::ElemExpr: {
      MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
      PropertyByValue* elem = &callee->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter& eoe = cone.prepareForElemCallee(isSuper);
      if (!emitElemObjAndKey(elem, isSuper, eoe)) {
        //          [stack] # if Super
        //          [stack] THIS? THIS KEY
        //          [stack] # otherwise
        //          [stack] OBJ? OBJ KEY
        return false;
      }
      if (!eoe.emitGet()) {
        //          [stack] CALLEE THIS?
        return false;
      }

      break;
    }
    case ParseNodeKind::PrivateMemberExpr: {
      MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
      PrivateMemberAccessBase* privateExpr =
          &callee->as<PrivateMemberAccessBase>();
      PrivateOpEmitter& xoe =
          cone.prepareForPrivateCallee(privateExpr->privateName().name());
      if (!emitTree(&privateExpr->expression())) {
        //          [stack] OBJ
        return false;
      }
      if (!xoe.emitReference()) {
        //          [stack] OBJ NAME
        return false;
      }
      if (!xoe.emitGetForCallOrNew()) {
        //          [stack] CALLEE THIS
        return false;
      }

      break;
    }
    case ParseNodeKind::Function:
      if (!cone.prepareForFunctionCallee()) {
        return false;
      }
      if (!emitTree(callee)) {
        //          [stack] CALLEE
        return false;
      }
      break;
    case ParseNodeKind::SuperBase:
      MOZ_ASSERT(call->isKind(ParseNodeKind::SuperCallExpr));
      MOZ_ASSERT(callee->isKind(ParseNodeKind::SuperBase));
      if (!cone.emitSuperCallee()) {
        //          [stack] CALLEE IsConstructing
        return false;
      }
      break;
    case ParseNodeKind::OptionalChain: {
      return emitCalleeAndThisForOptionalChain(&callee->as<UnaryNode>(),
                                               &call->as<CallNode>(), cone);
    }
    default:
      if (!cone.prepareForOtherCallee()) {
        return false;
      }
      if (!emitTree(callee)) {
        return false;
      }
      break;
  }

  if (!cone.emitThis()) {
    //              [stack] CALLEE THIS
    return false;
  }

  return true;
}

ParseNode* BytecodeEmitter::getCoordNode(ParseNode* callNode,
                                         ParseNode* calleeNode, JSOp op,
                                         ListNode* argsList) {
  ParseNode* coordNode = callNode;
  if (op == JSOp::Call || op == JSOp::SpreadCall) {
    // Default to using the location of the `(` itself.
    // obj[expr]() // expression
    //          ^  // column coord
    coordNode = argsList;

    switch (calleeNode->getKind()) {
      case ParseNodeKind::DotExpr:
        // Use the position of a property access identifier.
        //
        // obj().aprop() // expression
        //       ^       // column coord
        //
        // Note: Because of the constant folding logic in FoldElement,
        // this case also applies for constant string properties.
        //
        // obj()['aprop']() // expression
        //       ^          // column coord
        coordNode = &calleeNode->as<PropertyAccess>().key();
        break;
      case ParseNodeKind::Name: {
        // Use the start of callee name unless it is at a separator
        // or has no args.
        //
        // 2 + obj()   // expression
        //     ^       // column coord
        //
        if (argsList->empty() ||
            !bytecodeSection().atSeparator(calleeNode->pn_pos.begin)) {
          // Use the start of callee names.
          coordNode = calleeNode;
        }
        break;
      }

      default:
        break;
    }
  }
  return coordNode;
}

bool BytecodeEmitter::emitArguments(ListNode* argsList, bool isCall,
                                    bool isSpread, CallOrNewEmitter& cone) {
  uint32_t argc = argsList->count();
  if (argc >= ARGC_LIMIT) {
    reportError(argsList,
                isCall ? JSMSG_TOO_MANY_FUN_ARGS : JSMSG_TOO_MANY_CON_ARGS);
    return false;
  }
  if (!isSpread) {
    if (!cone.prepareForNonSpreadArguments()) {
      //            [stack] CALLEE THIS
      return false;
    }
    for (ParseNode* arg : argsList->contents()) {
      if (!emitTree(arg)) {
        //          [stack] CALLEE THIS ARG*
        return false;
      }
    }
  } else if (cone.wantSpreadOperand()) {
    auto* spreadNode = &argsList->head()->as<UnaryNode>();
    if (!emitTree(spreadNode->kid())) {
      //            [stack] CALLEE THIS ARG0
      return false;
    }

    if (!cone.emitSpreadArgumentsTest()) {
      //            [stack] CALLEE THIS ARG0
      return false;
    }

    if (cone.wantSpreadIteration()) {
      if (!emitSpreadIntoArray(spreadNode)) {
        //          [stack] CALLEE THIS ARR
        return false;
      }
    }

    if (!cone.emitSpreadArgumentsTestEnd()) {
      //            [stack] CALLEE THIS ARR
      return false;
    }
  } else {
    if (!cone.prepareForSpreadArguments()) {
      //            [stack] CALLEE THIS
      return false;
    }
    if (!emitArray(argsList)) {
      //            [stack] CALLEE THIS ARR
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitOptionalCall(CallNode* callNode, OptionalEmitter& oe,
                                       ValueUsage valueUsage) {
  /*
   * A modified version of emitCallOrNew that handles optional calls.
   *
   * These include the following:
   *    a?.()
   *    a.b?.()
   *    a.["b"]?.()
   *    (a?.b)?.()
   *
   * See CallOrNewEmitter for more context.
   */
  ParseNode* calleeNode = callNode->left();
  ListNode* argsList = &callNode->right()->as<ListNode>();
  bool isSpread = IsSpreadOp(callNode->callOp());
  JSOp op = callNode->callOp();
  uint32_t argc = argsList->count();
  bool isOptimizableSpread = isSpread && argc == 1;

  CallOrNewEmitter cone(this, op,
                        isOptimizableSpread
                            ? CallOrNewEmitter::ArgumentsKind::SingleSpread
                            : CallOrNewEmitter::ArgumentsKind::Other,
                        valueUsage);

  ParseNode* coordNode = getCoordNode(callNode, calleeNode, op, argsList);

  if (!emitOptionalCalleeAndThis(calleeNode, callNode, cone, oe)) {
    //              [stack] CALLEE THIS
    return false;
  }

  if (callNode->isKind(ParseNodeKind::OptionalCallExpr)) {
    if (!oe.emitJumpShortCircuitForCall()) {
      //            [stack] CALLEE THIS
      return false;
    }
  }

  if (!emitArguments(argsList, /* isCall = */ true, isSpread, cone)) {
    //              [stack] CALLEE THIS ARGS...
    return false;
  }

  if (!cone.emitEnd(argc, coordNode->pn_pos.begin)) {
    //              [stack] RVAL
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitCallOrNew(CallNode* callNode, ValueUsage valueUsage) {
  /*
   * Emit callable invocation or operator new (constructor call) code.
   * First, emit code for the left operand to evaluate the callable or
   * constructable object expression.
   *
   * Then (or in a call case that has no explicit reference-base
   * object) we emit JSOp::Undefined to produce the undefined |this|
   * value required for calls (which non-strict mode functions
   * will box into the global object).
   */
  bool isCall = callNode->isKind(ParseNodeKind::CallExpr) ||
                callNode->isKind(ParseNodeKind::TaggedTemplateExpr);
  ParseNode* calleeNode = callNode->left();
  ListNode* argsList = &callNode->right()->as<ListNode>();
  JSOp op = callNode->callOp();

  if (calleeNode->isKind(ParseNodeKind::Name) &&
      emitterMode == BytecodeEmitter::SelfHosting && op == JSOp::Call) {
    // Calls to "forceInterpreter", "callFunction",
    // "callContentFunction", or "resumeGenerator" in self-hosted
    // code generate inline bytecode.
    //
    // NOTE: The list of special instruction names has to be kept in sync with
    // "js/src/builtin/.eslintrc.js".
    auto calleeName = calleeNode->as<NameNode>().name();
    if (calleeName == TaggedParserAtomIndex::WellKnown::callFunction()) {
      return emitSelfHostedCallFunction(callNode, JSOp::Call);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::callContentFunction()) {
      return emitSelfHostedCallFunction(callNode, JSOp::CallContent);
    }
    if (calleeName ==
        TaggedParserAtomIndex::WellKnown::constructContentFunction()) {
      return emitSelfHostedCallFunction(callNode, JSOp::NewContent);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::resumeGenerator()) {
      return emitSelfHostedResumeGenerator(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::forceInterpreter()) {
      return emitSelfHostedForceInterpreter();
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::allowContentIter()) {
      return emitSelfHostedAllowContentIter(callNode);
    }
    if (calleeName ==
        TaggedParserAtomIndex::WellKnown::allowContentIterWith()) {
      return emitSelfHostedAllowContentIterWith(callNode);
    }
    if (calleeName ==
            TaggedParserAtomIndex::WellKnown::defineDataPropertyIntrinsic() &&
        argsList->count() == 3) {
      return emitSelfHostedDefineDataProperty(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::hasOwn()) {
      return emitSelfHostedHasOwn(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::getPropertySuper()) {
      return emitSelfHostedGetPropertySuper(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::ToNumeric()) {
      return emitSelfHostedToNumeric(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::ToString()) {
      return emitSelfHostedToString(callNode);
    }
    if (calleeName ==
        TaggedParserAtomIndex::WellKnown::GetBuiltinConstructor()) {
      return emitSelfHostedGetBuiltinConstructor(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::GetBuiltinPrototype()) {
      return emitSelfHostedGetBuiltinPrototype(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::GetBuiltinSymbol()) {
      return emitSelfHostedGetBuiltinSymbol(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::ArgumentsLength()) {
      return emitSelfHostedArgumentsLength(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::GetArgument()) {
      return emitSelfHostedGetArgument(callNode);
    }
    if (calleeName ==
        TaggedParserAtomIndex::WellKnown::SetIsInlinableLargeFunction()) {
      return emitSelfHostedSetIsInlinableLargeFunction(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::SetCanonicalName()) {
      return emitSelfHostedSetCanonicalName(callNode);
    }
    if (calleeName == TaggedParserAtomIndex::WellKnown::IsNullOrUndefined()) {
      return emitSelfHostedIsNullOrUndefined(callNode);
    }
#ifdef DEBUG
    if (calleeName ==
            TaggedParserAtomIndex::WellKnown::UnsafeGetReservedSlot() ||
        calleeName == TaggedParserAtomIndex::WellKnown::
                          UnsafeGetObjectFromReservedSlot() ||
        calleeName == TaggedParserAtomIndex::WellKnown::
                          UnsafeGetInt32FromReservedSlot() ||
        calleeName == TaggedParserAtomIndex::WellKnown::
                          UnsafeGetStringFromReservedSlot()) {
      // Make sure that this call is correct, but don't emit any special code.
      assertSelfHostedUnsafeGetReservedSlot(argsList);
    }
    if (calleeName ==
        TaggedParserAtomIndex::WellKnown::UnsafeSetReservedSlot()) {
      // Make sure that this call is correct, but don't emit any special code.
      assertSelfHostedUnsafeSetReservedSlot(argsList);
    }
#endif
    // Fall through
  }

  uint32_t argc = argsList->count();
  bool isSpread = IsSpreadOp(op);
  bool isOptimizableSpread = isSpread && argc == 1;
  bool isDefaultDerivedClassConstructor =
      sc->isFunctionBox() && sc->asFunctionBox()->isDerivedClassConstructor() &&
      sc->asFunctionBox()->isSyntheticFunction();
  MOZ_ASSERT_IF(isDefaultDerivedClassConstructor, isOptimizableSpread);
  CallOrNewEmitter cone(
      this, op,
      isOptimizableSpread
          ? isDefaultDerivedClassConstructor
                ? CallOrNewEmitter::ArgumentsKind::PassthroughRest
                : CallOrNewEmitter::ArgumentsKind::SingleSpread
          : CallOrNewEmitter::ArgumentsKind::Other,
      valueUsage);

  if (!emitCalleeAndThis(calleeNode, callNode, cone)) {
    //              [stack] CALLEE THIS
    return false;
  }
  if (!emitArguments(argsList, isCall, isSpread, cone)) {
    //              [stack] CALLEE THIS ARGS...
    return false;
  }

  // Push new.target for construct calls.
  if (IsConstructOp(op)) {
    if (op == JSOp::SuperCall || op == JSOp::SpreadSuperCall) {
      if (!emitNewTarget(callNode)) {
        //          [stack] CALLEE THIS ARGS.. NEW.TARGET
        return false;
      }
    } else {
      // Repush the callee as new.target
      uint32_t effectiveArgc = isSpread ? 1 : argc;
      if (!emitDupAt(effectiveArgc + 1)) {
        //          [stack] CALLEE THIS ARGS.. CALLEE
        return false;
      }
    }
  }

  ParseNode* coordNode = getCoordNode(callNode, calleeNode, op, argsList);

  if (!cone.emitEnd(argc, coordNode->pn_pos.begin)) {
    //              [stack] RVAL
    return false;
  }

  return true;
}

// This list must be kept in the same order in several places:
//   - The binary operators in ParseNode.h ,
//   - the binary operators in TokenKind.h
//   - the precedence list in Parser.cpp
static const JSOp ParseNodeKindToJSOp[] = {
    // Some binary ops require special code generation (PrivateIn);
    // these should not use BinaryOpParseNodeKindToJSOp. This table fills those
    // slots with Nops to make the rest of the table lookup work.
    JSOp::Coalesce, JSOp::Or,       JSOp::And, JSOp::BitOr,    JSOp::BitXor,
    JSOp::BitAnd,   JSOp::StrictEq, JSOp::Eq,  JSOp::StrictNe, JSOp::Ne,
    JSOp::Lt,       JSOp::Le,       JSOp::Gt,  JSOp::Ge,       JSOp::Instanceof,
    JSOp::In,       JSOp::Nop,      JSOp::Lsh, JSOp::Rsh,      JSOp::Ursh,
    JSOp::Add,      JSOp::Sub,      JSOp::Mul, JSOp::Div,      JSOp::Mod,
    JSOp::Pow};

static inline JSOp BinaryOpParseNodeKindToJSOp(ParseNodeKind pnk) {
  MOZ_ASSERT(pnk >= ParseNodeKind::BinOpFirst);
  MOZ_ASSERT(pnk <= ParseNodeKind::BinOpLast);
  int parseNodeFirst = size_t(ParseNodeKind::BinOpFirst);
#ifdef DEBUG
  int jsopArraySize = std::size(ParseNodeKindToJSOp);
  int parseNodeKindListSize =
      size_t(ParseNodeKind::BinOpLast) - parseNodeFirst + 1;
  MOZ_ASSERT(jsopArraySize == parseNodeKindListSize);
  // Ensure we don't use this to find an op for a parse node
  // requiring special emission rules.
  MOZ_ASSERT(ParseNodeKindToJSOp[size_t(pnk) - parseNodeFirst] != JSOp::Nop);
#endif
  return ParseNodeKindToJSOp[size_t(pnk) - parseNodeFirst];
}

bool BytecodeEmitter::emitRightAssociative(ListNode* node) {
  // ** is the only right-associative operator.
  MOZ_ASSERT(node->isKind(ParseNodeKind::PowExpr));

  // Right-associative operator chain.
  for (ParseNode* subexpr : node->contents()) {
    if (!emitTree(subexpr)) {
      return false;
    }
  }
  for (uint32_t i = 0; i < node->count() - 1; i++) {
    if (!emit1(JSOp::Pow)) {
      return false;
    }
  }
  return true;
}

bool BytecodeEmitter::emitLeftAssociative(ListNode* node) {
  // Left-associative operator chain.
  if (!emitTree(node->head())) {
    return false;
  }
  JSOp op = BinaryOpParseNodeKindToJSOp(node->getKind());
  ParseNode* nextExpr = node->head()->pn_next;
  do {
    if (!emitTree(nextExpr)) {
      return false;
    }
    if (!emit1(op)) {
      return false;
    }
  } while ((nextExpr = nextExpr->pn_next));
  return true;
}

bool BytecodeEmitter::emitPrivateInExpr(ListNode* node) {
  MOZ_ASSERT(node->head()->isKind(ParseNodeKind::PrivateName));

  NameNode& privateNameNode = node->head()->as<NameNode>();
  TaggedParserAtomIndex privateName = privateNameNode.name();

  PrivateOpEmitter xoe(this, PrivateOpEmitter::Kind::ErgonomicBrandCheck,
                       privateName);

  ParseNode* valueNode = node->head()->pn_next;
  MOZ_ASSERT(valueNode->pn_next == nullptr);

  if (!emitTree(valueNode)) {
    //              [stack] OBJ
    return false;
  }

  if (!xoe.emitReference()) {
    //              [stack] OBJ BRAND  if private method
    //              [stack] OBJ NAME   if private field or accessor.
    return false;
  }

  if (!xoe.emitBrandCheck()) {
    //              [stack] OBJ BRAND BOOL if private method
    //              [stack] OBJ NAME  BOOL if private field or accessor.
    return false;
  }

  if (!emitUnpickN(2)) {
    //              [stack] BOOL OBJ BRAND if private method
    //              [stack] BOOL OBJ NAME   if private field or accessor.
    return false;
  }

  if (!emitPopN(2)) {
    //              [stack] BOOL
    return false;
  }

  return true;
}

/*
 * Special `emitTree` for Optional Chaining case.
 * Examples of this are `emitOptionalChain`, `emitDeleteOptionalChain` and
 * `emitCalleeAndThisForOptionalChain`.
 */
bool BytecodeEmitter::emitOptionalTree(
    ParseNode* pn, OptionalEmitter& oe,
    ValueUsage valueUsage /* = ValueUsage::WantValue */) {
  AutoCheckRecursionLimit recursion(fc);
  if (!recursion.check(fc)) {
    return false;
  }
  ParseNodeKind kind = pn->getKind();
  switch (kind) {
    case ParseNodeKind::OptionalDotExpr: {
      OptionalPropertyAccess* prop = &pn->as<OptionalPropertyAccess>();
      bool isSuper = false;
      PropOpEmitter poe(this, PropOpEmitter::Kind::Get,
                        PropOpEmitter::ObjKind::Other);
      if (!emitOptionalDotExpression(prop, poe, isSuper, oe)) {
        return false;
      }
      break;
    }
    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &pn->as<PropertyAccess>();
      bool isSuper = prop->isSuper();
      PropOpEmitter poe(this, PropOpEmitter::Kind::Get,
                        isSuper ? PropOpEmitter::ObjKind::Super
                                : PropOpEmitter::ObjKind::Other);
      if (!emitOptionalDotExpression(prop, poe, isSuper, oe)) {
        return false;
      }
      break;
    }

    case ParseNodeKind::OptionalElemExpr: {
      OptionalPropertyByValue* elem = &pn->as<OptionalPropertyByValue>();
      bool isSuper = false;
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter eoe(this, ElemOpEmitter::Kind::Get,
                        ElemOpEmitter::ObjKind::Other);

      if (!emitOptionalElemExpression(elem, eoe, isSuper, oe)) {
        return false;
      }
      break;
    }
    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &pn->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter eoe(this, ElemOpEmitter::Kind::Get,
                        isSuper ? ElemOpEmitter::ObjKind::Super
                                : ElemOpEmitter::ObjKind::Other);

      if (!emitOptionalElemExpression(elem, eoe, isSuper, oe)) {
        return false;
      }
      break;
    }
    case ParseNodeKind::PrivateMemberExpr:
    case ParseNodeKind::OptionalPrivateMemberExpr: {
      PrivateMemberAccessBase* privateExpr = &pn->as<PrivateMemberAccessBase>();
      PrivateOpEmitter xoe(this, PrivateOpEmitter::Kind::Get,
                           privateExpr->privateName().name());
      if (!emitOptionalPrivateExpression(privateExpr, xoe, oe)) {
        return false;
      }
      break;
    }
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::OptionalCallExpr:
      if (!emitOptionalCall(&pn->as<CallNode>(), oe, valueUsage)) {
        return false;
      }
      break;
    // List of accepted ParseNodeKinds that might appear only at the beginning
    // of an Optional Chain.
    // For example, a taggedTemplateExpr node might occur if we have
    // `test`?.b, with `test` as the taggedTemplateExpr ParseNode.
    default:
#ifdef DEBUG
      // https://tc39.es/ecma262/#sec-primary-expression
      bool isPrimaryExpression =
          kind == ParseNodeKind::ThisExpr || kind == ParseNodeKind::Name ||
          kind == ParseNodeKind::PrivateName ||
          kind == ParseNodeKind::NullExpr || kind == ParseNodeKind::TrueExpr ||
          kind == ParseNodeKind::FalseExpr ||
          kind == ParseNodeKind::NumberExpr ||
          kind == ParseNodeKind::BigIntExpr ||
          kind == ParseNodeKind::StringExpr ||
          kind == ParseNodeKind::ArrayExpr ||
          kind == ParseNodeKind::ObjectExpr ||
          kind == ParseNodeKind::Function || kind == ParseNodeKind::ClassDecl ||
          kind == ParseNodeKind::RegExpExpr ||
          kind == ParseNodeKind::TemplateStringExpr ||
          kind == ParseNodeKind::TemplateStringListExpr ||
          kind == ParseNodeKind::RawUndefinedExpr || pn->isInParens();

      // https://tc39.es/ecma262/#sec-left-hand-side-expressions
      bool isMemberExpression = isPrimaryExpression ||
                                kind == ParseNodeKind::TaggedTemplateExpr ||
                                kind == ParseNodeKind::NewExpr ||
                                kind == ParseNodeKind::NewTargetExpr ||
                                kind == ParseNodeKind::ImportMetaExpr;

      bool isCallExpression = kind == ParseNodeKind::SetThis ||
                              kind == ParseNodeKind::CallImportExpr;

      MOZ_ASSERT(isMemberExpression || isCallExpression,
                 "Unknown ParseNodeKind for OptionalChain");
#endif
      return emitTree(pn);
  }
  return true;
}

// Handle the case of a call made on a OptionalChainParseNode.
// For example `(a?.b)()` and `(a?.b)?.()`.
bool BytecodeEmitter::emitCalleeAndThisForOptionalChain(
    UnaryNode* optionalChain, CallNode* callNode, CallOrNewEmitter& cone) {
  ParseNode* calleeNode = optionalChain->kid();

  // Create a new OptionalEmitter, in order to emit the right bytecode
  // in isolation.
  OptionalEmitter oe(this, bytecodeSection().stackDepth());

  if (!emitOptionalCalleeAndThis(calleeNode, callNode, cone, oe)) {
    //              [stack] CALLEE THIS
    return false;
  }

  // complete the jump if necessary. This will set both the "this" value
  // and the "callee" value to undefined, if the callee is undefined. It
  // does not matter much what the this value is, the function call will
  // fail if it is not optional, and be set to undefined otherwise.
  if (!oe.emitOptionalJumpTarget(JSOp::Undefined,
                                 OptionalEmitter::Kind::Reference)) {
    //              [stack] # If shortcircuit
    //              [stack] UNDEFINED UNDEFINED
    //              [stack] # otherwise
    //              [stack] CALLEE THIS
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitOptionalChain(UnaryNode* optionalChain,
                                        ValueUsage valueUsage) {
  ParseNode* expr = optionalChain->kid();

  OptionalEmitter oe(this, bytecodeSection().stackDepth());

  if (!emitOptionalTree(expr, oe, valueUsage)) {
    //              [stack] VAL
    return false;
  }

  if (!oe.emitOptionalJumpTarget(JSOp::Undefined)) {
    //              [stack] # If shortcircuit
    //              [stack] UNDEFINED
    //              [stack] # otherwise
    //              [stack] VAL
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitOptionalDotExpression(PropertyAccessBase* prop,
                                                PropOpEmitter& poe,
                                                bool isSuper,
                                                OptionalEmitter& oe) {
  if (!poe.prepareForObj()) {
    //              [stack]
    return false;
  }

  if (isSuper) {
    UnaryNode* base = &prop->expression().as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] OBJ
      return false;
    }
  } else {
    if (!emitOptionalTree(&prop->expression(), oe)) {
      //            [stack] OBJ
      return false;
    }
  }

  if (prop->isKind(ParseNodeKind::OptionalDotExpr)) {
    MOZ_ASSERT(!isSuper);
    if (!oe.emitJumpShortCircuit()) {
      //            [stack] # if Jump
      //            [stack] UNDEFINED-OR-NULL
      //            [stack] # otherwise
      //            [stack] OBJ
      return false;
    }
  }

  if (!poe.emitGet(prop->key().atom())) {
    //              [stack] PROP
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitOptionalElemExpression(PropertyByValueBase* elem,
                                                 ElemOpEmitter& eoe,
                                                 bool isSuper,
                                                 OptionalEmitter& oe) {
  if (!eoe.prepareForObj()) {
    //              [stack]
    return false;
  }

  if (isSuper) {
    UnaryNode* base = &elem->expression().as<UnaryNode>();
    if (!emitGetThisForSuperBase(base)) {
      //            [stack] OBJ
      return false;
    }
  } else {
    if (!emitOptionalTree(&elem->expression(), oe)) {
      //            [stack] OBJ
      return false;
    }
  }

  if (elem->isKind(ParseNodeKind::OptionalElemExpr)) {
    MOZ_ASSERT(!isSuper);
    if (!oe.emitJumpShortCircuit()) {
      //            [stack] # if Jump
      //            [stack] UNDEFINED-OR-NULL
      //            [stack] # otherwise
      //            [stack] OBJ
      return false;
    }
  }

  if (!eoe.prepareForKey()) {
    //              [stack] OBJ? OBJ
    return false;
  }

  if (!emitTree(&elem->key())) {
    //              [stack] OBJ? OBJ KEY
    return false;
  }

  if (!eoe.emitGet()) {
    //              [stack] ELEM
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitOptionalPrivateExpression(
    PrivateMemberAccessBase* privateExpr, PrivateOpEmitter& xoe,
    OptionalEmitter& oe) {
  if (!emitOptionalTree(&privateExpr->expression(), oe)) {
    //              [stack] OBJ
    return false;
  }

  if (privateExpr->isKind(ParseNodeKind::OptionalPrivateMemberExpr)) {
    if (!oe.emitJumpShortCircuit()) {
      //            [stack] # if Jump
      //            [stack] UNDEFINED-OR-NULL
      //            [stack] # otherwise
      //            [stack] OBJ
      return false;
    }
  }

  if (!xoe.emitReference()) {
    //              [stack] OBJ NAME
    return false;
  }
  if (!xoe.emitGet()) {
    //              [stack] CALLEE THIS  # if call
    //              [stack] VALUE        # otherwise
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitShortCircuit(ListNode* node, ValueUsage valueUsage) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::OrExpr) ||
             node->isKind(ParseNodeKind::CoalesceExpr) ||
             node->isKind(ParseNodeKind::AndExpr));

  /*
   * JSOp::Or converts the operand on the stack to boolean, leaves the original
   * value on the stack and jumps if true; otherwise it falls into the next
   * bytecode, which pops the left operand and then evaluates the right operand.
   * The jump goes around the right operand evaluation.
   *
   * JSOp::And converts the operand on the stack to boolean and jumps if false;
   * otherwise it falls into the right operand's bytecode.
   */

  TDZCheckCache tdzCache(this);

  JSOp op;
  switch (node->getKind()) {
    case ParseNodeKind::OrExpr:
      op = JSOp::Or;
      break;
    case ParseNodeKind::CoalesceExpr:
      op = JSOp::Coalesce;
      break;
    case ParseNodeKind::AndExpr:
      op = JSOp::And;
      break;
    default:
      MOZ_CRASH("Unexpected ParseNodeKind");
  }

  JumpList jump;

  // Left-associative operator chain: avoid too much recursion.
  //
  // Emit all nodes but the last.
  for (ParseNode* expr : node->contentsTo(node->last())) {
    if (!emitTree(expr)) {
      return false;
    }
    if (!emitJump(op, &jump)) {
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      return false;
    }
  }

  // Emit the last node
  if (!emitTree(node->last(), valueUsage)) {
    return false;
  }

  if (!emitJumpTargetAndPatch(jump)) {
    return false;
  }
  return true;
}

bool BytecodeEmitter::emitSequenceExpr(ListNode* node, ValueUsage valueUsage) {
  for (ParseNode* child : node->contentsTo(node->last())) {
    if (!updateSourceCoordNotes(child->pn_pos.begin)) {
      return false;
    }
    if (!emitTree(child, ValueUsage::IgnoreValue)) {
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      return false;
    }
  }

  ParseNode* child = node->last();
  if (!updateSourceCoordNotes(child->pn_pos.begin)) {
    return false;
  }
  if (!emitTree(child, valueUsage)) {
    return false;
  }
  return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool BytecodeEmitter::emitIncOrDec(UnaryNode* incDec,
                                                    ValueUsage valueUsage) {
  switch (incDec->kid()->getKind()) {
    case ParseNodeKind::DotExpr:
      return emitPropIncDec(incDec, valueUsage);
    case ParseNodeKind::ElemExpr:
      return emitElemIncDec(incDec, valueUsage);
    case ParseNodeKind::PrivateMemberExpr:
      return emitPrivateIncDec(incDec, valueUsage);
    case ParseNodeKind::CallExpr:
      return emitCallIncDec(incDec);
    default:
      return emitNameIncDec(incDec, valueUsage);
  }
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool BytecodeEmitter::emitLabeledStatement(
    const LabeledStatement* labeledStmt) {
  auto name = labeledStmt->label();
  LabelEmitter label(this);

  label.emitLabel(name);

  if (!emitTree(labeledStmt->statement())) {
    return false;
  }
  if (!label.emitEnd()) {
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitConditionalExpression(
    ConditionalExpression& conditional, ValueUsage valueUsage) {
  CondEmitter cond(this);
  if (!cond.emitCond()) {
    return false;
  }

  ParseNode* conditionNode = &conditional.condition();
  auto conditionKind = IfEmitter::ConditionKind::Positive;
  if (conditionNode->isKind(ParseNodeKind::NotExpr)) {
    conditionNode = conditionNode->as<UnaryNode>().kid();
    conditionKind = IfEmitter::ConditionKind::Negative;
  }

  // NOTE: NotExpr of conditionNode may be unwrapped, and in that case the
  //       negation is handled by conditionKind.
  if (!emitTree(conditionNode)) {
    return false;
  }

  if (!cond.emitThenElse(conditionKind)) {
    return false;
  }

  if (!emitTree(&conditional.thenExpression(), valueUsage)) {
    return false;
  }

  if (!cond.emitElse()) {
    return false;
  }

  if (!emitTree(&conditional.elseExpression(), valueUsage)) {
    return false;
  }

  if (!cond.emitEnd()) {
    return false;
  }
  MOZ_ASSERT(cond.pushed() == 1);

  return true;
}

// Check for an object-literal property list that can be handled by the
// ObjLiteral writer. We ensure that for each `prop: value` pair, the key is a
// constant name or numeric index, there is no accessor specified, and the value
// can be encoded by an ObjLiteral instruction (constant number, string,
// boolean, null/undefined).
void BytecodeEmitter::isPropertyListObjLiteralCompatible(ListNode* obj,
                                                         bool* withValues,
                                                         bool* withoutValues) {
  bool keysOK = true;
  bool valuesOK = true;
  uint32_t propCount = 0;

  for (ParseNode* propdef : obj->contents()) {
    if (!propdef->is<BinaryNode>()) {
      keysOK = false;
      break;
    }
    propCount++;

    BinaryNode* prop = &propdef->as<BinaryNode>();
    ParseNode* key = prop->left();
    ParseNode* value = prop->right();

    // Computed keys not OK (ObjLiteral data stores constant keys).
    if (key->isKind(ParseNodeKind::ComputedName)) {
      keysOK = false;
      break;
    }

    // BigIntExprs should have been lowered to computed names at parse
    // time, and so should be excluded above.
    MOZ_ASSERT(!key->isKind(ParseNodeKind::BigIntExpr));

    // Numeric keys OK as long as they are integers and in range.
    if (key->isKind(ParseNodeKind::NumberExpr)) {
      double numValue = key->as<NumericLiteral>().value();
      int32_t i = 0;
      if (!NumberIsInt32(numValue, &i)) {
        keysOK = false;
        break;
      }
      if (!ObjLiteralWriter::arrayIndexInRange(i)) {
        keysOK = false;
        break;
      }
    }

    MOZ_ASSERT(key->isKind(ParseNodeKind::ObjectPropertyName) ||
               key->isKind(ParseNodeKind::StringExpr) ||
               key->isKind(ParseNodeKind::NumberExpr));

    AccessorType accessorType =
        prop->is<PropertyDefinition>()
            ? prop->as<PropertyDefinition>().accessorType()
            : AccessorType::None;
    if (accessorType != AccessorType::None) {
      keysOK = false;
      break;
    }

    if (!isRHSObjLiteralCompatible(value)) {
      valuesOK = false;
    }
  }

  if (propCount > SharedPropMap::MaxPropsForNonDictionary) {
    // JSOp::NewObject cannot accept dictionary-mode objects.
    keysOK = false;
  }

  *withValues = keysOK && valuesOK;
  *withoutValues = keysOK;
}

bool BytecodeEmitter::isArrayObjLiteralCompatible(ListNode* array) {
  for (ParseNode* elem : array->contents()) {
    if (elem->isKind(ParseNodeKind::Spread)) {
      return false;
    }
    if (!isRHSObjLiteralCompatible(elem)) {
      return false;
    }
  }
  return true;
}

bool BytecodeEmitter::emitPropertyList(ListNode* obj, PropertyEmitter& pe,
                                       PropListType type) {
  //                [stack] CTOR? OBJ

  size_t curFieldKeyIndex = 0;
  size_t curStaticFieldKeyIndex = 0;
  for (ParseNode* propdef : obj->contents()) {
    if (propdef->is<ClassField>()) {
      MOZ_ASSERT(type == ClassBody);
      // Only handle computing field keys here: the .initializers lambda array
      // is created elsewhere.
      ClassField* field = &propdef->as<ClassField>();
      if (field->name().getKind() == ParseNodeKind::ComputedName) {
        auto fieldKeys =
            field->isStatic()
                ? TaggedParserAtomIndex::WellKnown::dotStaticFieldKeys()
                : TaggedParserAtomIndex::WellKnown::dotFieldKeys();
        if (!emitGetName(fieldKeys)) {
          //        [stack] CTOR OBJ ARRAY
          return false;
        }

        ParseNode* nameExpr = field->name().as<UnaryNode>().kid();

        if (!emitTree(nameExpr, ValueUsage::WantValue)) {
          //        [stack] CTOR OBJ ARRAY KEY
          return false;
        }

        if (!emit1(JSOp::ToPropertyKey)) {
          //        [stack] CTOR OBJ ARRAY KEY
          return false;
        }

        size_t fieldKeysIndex;
        if (field->isStatic()) {
          fieldKeysIndex = curStaticFieldKeyIndex++;
        } else {
          fieldKeysIndex = curFieldKeyIndex++;
        }

        if (!emitUint32Operand(JSOp::InitElemArray, fieldKeysIndex)) {
          //        [stack] CTOR OBJ ARRAY
          return false;
        }

        if (!emit1(JSOp::Pop)) {
          //        [stack] CTOR OBJ
          return false;
        }
      }
      continue;
    }

    if (propdef->isKind(ParseNodeKind::StaticClassBlock)) {
      // Static class blocks are emitted as part of
      // emitCreateMemberInitializers.
      continue;
    }

    if (propdef->is<LexicalScopeNode>()) {
      // Constructors are sometimes wrapped in LexicalScopeNodes. As we
      // already handled emitting the constructor, skip it.
      MOZ_ASSERT(
          propdef->as<LexicalScopeNode>().scopeBody()->is<ClassMethod>());
      continue;
    }

    // Handle __proto__: v specially because *only* this form, and no other
    // involving "__proto__", performs [[Prototype]] mutation.
    if (propdef->isKind(ParseNodeKind::MutateProto)) {
      //            [stack] OBJ
      MOZ_ASSERT(type == ObjectLiteral);
      if (!pe.prepareForProtoValue(propdef->pn_pos.begin)) {
        //          [stack] OBJ
        return false;
      }
      if (!emitTree(propdef->as<UnaryNode>().kid())) {
        //          [stack] OBJ PROTO
        return false;
      }
      if (!pe.emitMutateProto()) {
        //          [stack] OBJ
        return false;
      }
      continue;
    }

    if (propdef->isKind(ParseNodeKind::Spread)) {
      MOZ_ASSERT(type == ObjectLiteral);
      //            [stack] OBJ
      if (!pe.prepareForSpreadOperand(propdef->pn_pos.begin)) {
        //          [stack] OBJ OBJ
        return false;
      }
      if (!emitTree(propdef->as<UnaryNode>().kid())) {
        //          [stack] OBJ OBJ VAL
        return false;
      }
      if (!pe.emitSpread()) {
        //          [stack] OBJ
        return false;
      }
      continue;
    }

    BinaryNode* prop = &propdef->as<BinaryNode>();

    ParseNode* key = prop->left();
    AccessorType accessorType;
    if (prop->is<ClassMethod>()) {
      ClassMethod& method = prop->as<ClassMethod>();
      accessorType = method.accessorType();

      if (!method.isStatic() && key->isKind(ParseNodeKind::PrivateName) &&
          accessorType != AccessorType::None) {
        // Private non-static accessors are stamped onto instances from
        // initializers; see emitCreateMemberInitializers.
        continue;
      }
    } else if (prop->is<PropertyDefinition>()) {
      accessorType = prop->as<PropertyDefinition>().accessorType();
    } else {
      accessorType = AccessorType::None;
    }

    auto emitValue = [this, &key, &prop, accessorType, &pe]() {
      //            [stack] CTOR? OBJ CTOR? KEY?

      ParseNode* propVal = prop->right();
      if (propVal->isDirectRHSAnonFunction()) {
        // The following branches except for the last `else` clause emit the
        // cases handled in NameResolver::resolveFun (see NameFunctions.cpp)
        if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
            key->isKind(ParseNodeKind::StringExpr)) {
          auto keyAtom = key->as<NameNode>().atom();
          if (!emitAnonymousFunctionWithName(propVal, keyAtom)) {
            //      [stack] CTOR? OBJ CTOR? VAL
            return false;
          }
        } else if (key->isKind(ParseNodeKind::NumberExpr)) {
          MOZ_ASSERT(accessorType == AccessorType::None);

          auto keyAtom = key->as<NumericLiteral>().toAtom(fc, parserAtoms());
          if (!keyAtom) {
            return false;
          }
          if (!emitAnonymousFunctionWithName(propVal, keyAtom)) {
            //      [stack] CTOR? OBJ CTOR? KEY VAL
            return false;
          }
        } else if (key->isKind(ParseNodeKind::ComputedName) &&
                   (key->as<UnaryNode>().kid()->isKind(
                        ParseNodeKind::NumberExpr) ||
                    key->as<UnaryNode>().kid()->isKind(
                        ParseNodeKind::StringExpr)) &&
                   accessorType == AccessorType::None) {
          ParseNode* keyKid = key->as<UnaryNode>().kid();
          if (keyKid->isKind(ParseNodeKind::NumberExpr)) {
            auto keyAtom =
                keyKid->as<NumericLiteral>().toAtom(fc, parserAtoms());
            if (!keyAtom) {
              return false;
            }
            if (!emitAnonymousFunctionWithName(propVal, keyAtom)) {
              //    [stack] CTOR? OBJ CTOR? KEY VAL
              return false;
            }
          } else {
            MOZ_ASSERT(keyKid->isKind(ParseNodeKind::StringExpr));
            auto keyAtom = keyKid->as<NameNode>().atom();
            if (!emitAnonymousFunctionWithName(propVal, keyAtom)) {
              //    [stack] CTOR? OBJ CTOR? KEY VAL
              return false;
            }
          }
        } else {
          // Either a proper computed property name or a synthetic computed
          // property name for BigInt keys.
          MOZ_ASSERT(key->isKind(ParseNodeKind::ComputedName));

          FunctionPrefixKind prefix =
              accessorType == AccessorType::None     ? FunctionPrefixKind::None
              : accessorType == AccessorType::Getter ? FunctionPrefixKind::Get
                                                     : FunctionPrefixKind::Set;

          if (!emitAnonymousFunctionWithComputedName(propVal, prefix)) {
            //      [stack] CTOR? OBJ CTOR? KEY VAL
            return false;
          }
        }
      } else {
        if (!emitTree(propVal)) {
          //        [stack] CTOR? OBJ CTOR? KEY? VAL
          return false;
        }
      }

      if (propVal->is<FunctionNode>() &&
          propVal->as<FunctionNode>().funbox()->needsHomeObject()) {
        if (!pe.emitInitHomeObject()) {
          //        [stack] CTOR? OBJ CTOR? KEY? FUN
          return false;
        }
      }

#ifdef ENABLE_DECORATORS
      if (prop->is<ClassMethod>()) {
        ClassMethod& method = prop->as<ClassMethod>();
        if (method.decorators() && !method.decorators()->empty()) {
          DecoratorEmitter::Kind kind;
          switch (method.accessorType()) {
            case AccessorType::Getter:
              kind = DecoratorEmitter::Getter;
              break;
            case AccessorType::Setter:
              kind = DecoratorEmitter::Setter;
              break;
            case AccessorType::None:
              kind = DecoratorEmitter::Method;
              break;
          }

          // The decorators are applied to the current value on the stack,
          // possibly replacing it.
          DecoratorEmitter de(this);
          if (!de.emitApplyDecoratorsToElementDefinition(
                  kind, key, method.decorators(), method.isStatic())) {
            //        [stack] CTOR? OBJ CTOR? KEY? VAL
            return false;
          }
        }
      }
#endif

      return true;
    };

    PropertyEmitter::Kind kind =
        (type == ClassBody && propdef->as<ClassMethod>().isStatic())
            ? PropertyEmitter::Kind::Static
            : PropertyEmitter::Kind::Prototype;
    if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
        key->isKind(ParseNodeKind::StringExpr)) {
      //            [stack] CTOR? OBJ

      auto keyAtom = key->as<NameNode>().atom();

      // emitClass took care of constructor already.
      if (type == ClassBody &&
          keyAtom == TaggedParserAtomIndex::WellKnown::constructor() &&
          !propdef->as<ClassMethod>().isStatic()) {
        continue;
      }

      if (!pe.prepareForPropValue(propdef->pn_pos.begin, kind)) {
        //          [stack] CTOR? OBJ CTOR?
        return false;
      }

      if (!emitValue()) {
        //          [stack] CTOR? OBJ CTOR? VAL
        return false;
      }

      if (!pe.emitInit(accessorType, keyAtom)) {
        //          [stack] CTOR? OBJ
        return false;
      }

      continue;
    }

    if (key->isKind(ParseNodeKind::NumberExpr)) {
      //            [stack] CTOR? OBJ
      if (!pe.prepareForIndexPropKey(propdef->pn_pos.begin, kind)) {
        //          [stack] CTOR? OBJ CTOR?
        return false;
      }
      if (!emitNumberOp(key->as<NumericLiteral>().value())) {
        //        [stack] CTOR? OBJ CTOR? KEY
        return false;
      }
      if (!pe.prepareForIndexPropValue()) {
        //          [stack] CTOR? OBJ CTOR? KEY
        return false;
      }
      if (!emitValue()) {
        //          [stack] CTOR? OBJ CTOR? KEY VAL
        return false;
      }

      if (!pe.emitInitIndexOrComputed(accessorType)) {
        //          [stack] CTOR? OBJ
        return false;
      }

      continue;
    }

    if (key->isKind(ParseNodeKind::ComputedName)) {
      // Either a proper computed property name or a synthetic computed property
      // name for BigInt keys.

      //            [stack] CTOR? OBJ

      if (!pe.prepareForComputedPropKey(propdef->pn_pos.begin, kind)) {
        //          [stack] CTOR? OBJ CTOR?
        return false;
      }
      if (!emitTree(key->as<UnaryNode>().kid())) {
        //          [stack] CTOR? OBJ CTOR? KEY
        return false;
      }
      if (!pe.prepareForComputedPropValue()) {
        //          [stack] CTOR? OBJ CTOR? KEY
        return false;
      }
      if (!emitValue()) {
        //          [stack] CTOR? OBJ CTOR? KEY VAL
        return false;
      }

      if (!pe.emitInitIndexOrComputed(accessorType)) {
        //          [stack] CTOR? OBJ
        return false;
      }

      continue;
    }

    MOZ_ASSERT(key->isKind(ParseNodeKind::PrivateName));
    MOZ_ASSERT(type == ClassBody);

    auto* privateName = &key->as<NameNode>();

    if (kind == PropertyEmitter::Kind::Prototype) {
      MOZ_ASSERT(accessorType == AccessorType::None);
      if (!pe.prepareForPrivateMethod()) {
        //          [stack] CTOR OBJ
        return false;
      }
      NameOpEmitter noe(this, privateName->atom(),
                        NameOpEmitter::Kind::SimpleAssignment);

      // Ensure the NameOp emitter doesn't push an environment onto the stack,
      // because that would change the stack location of the home object.
      MOZ_ASSERT(noe.loc().kind() == NameLocation::Kind::FrameSlot ||
                 noe.loc().kind() == NameLocation::Kind::EnvironmentCoordinate);

      if (!noe.prepareForRhs()) {
        //          [stack] CTOR OBJ
        return false;
      }
      if (!emitValue()) {
        //          [stack] CTOR OBJ METHOD
        return false;
      }
      if (!noe.emitAssignment()) {
        //          [stack] CTOR OBJ METHOD
        return false;
      }
      if (!emit1(JSOp::Pop)) {
        //          [stack] CTOR OBJ
        return false;
      }
      if (!pe.skipInit()) {
        //          [stack] CTOR OBJ
        return false;
      }
      continue;
    }

    MOZ_ASSERT(kind == PropertyEmitter::Kind::Static);

    //              [stack] CTOR OBJ

    if (!pe.prepareForPrivateStaticMethod(propdef->pn_pos.begin)) {
      //            [stack] CTOR OBJ CTOR
      return false;
    }
    if (!emitGetPrivateName(privateName)) {
      //            [stack] CTOR OBJ CTOR KEY
      return false;
    }
    if (!emitValue()) {
      //            [stack] CTOR OBJ CTOR KEY VAL
      return false;
    }

    if (!pe.emitPrivateStaticMethod(accessorType)) {
      //            [stack] CTOR OBJ
      return false;
    }

    if (privateName->privateNameKind() == PrivateNameKind::Setter) {
      if (!emitDupAt(1)) {
        //          [stack] CTOR OBJ CTOR
        return false;
      }
      if (!emitGetPrivateName(privateName)) {
        //          [stack] CTOR OBJ CTOR NAME
        return false;
      }
      if (!emitAtomOp(JSOp::GetIntrinsic,
                      TaggedParserAtomIndex::WellKnown::NoPrivateGetter())) {
        //          [stack] CTOR OBJ CTOR NAME FUN
        return false;
      }
      if (!emit1(JSOp::InitHiddenElemGetter)) {
        //          [stack] CTOR OBJ CTOR
        return false;
      }
      if (!emit1(JSOp::Pop)) {
        //          [stack] CTOR OBJ
        return false;
      }
    }
  }

  return true;
}

bool BytecodeEmitter::emitPropertyListObjLiteral(ListNode* obj, JSOp op,
                                                 bool useObjLiteralValues) {
  ObjLiteralWriter writer;

#ifdef DEBUG
  // In self-hosted JS, we check duplication only on debug build.
  mozilla::Maybe<mozilla::HashSet<frontend::TaggedParserAtomIndex,
                                  frontend::TaggedParserAtomIndexHasher>>
      selfHostedPropNames;
  if (emitterMode == BytecodeEmitter::SelfHosting) {
    selfHostedPropNames.emplace();
  }
#endif

  if (op == JSOp::Object) {
    writer.beginObject(op);
  } else {
    MOZ_ASSERT(op == JSOp::NewObject);
    writer.beginShape(op);
  }

  for (ParseNode* propdef : obj->contents()) {
    BinaryNode* prop = &propdef->as<BinaryNode>();
    ParseNode* key = prop->left();

    if (key->is<NameNode>()) {
      if (emitterMode == BytecodeEmitter::SelfHosting) {
        auto propName = key->as<NameNode>().atom();
#ifdef DEBUG
        // Self-hosted JS shouldn't contain duplicate properties.
        auto p = selfHostedPropNames->lookupForAdd(propName);
        MOZ_ASSERT(!p);
        if (!selfHostedPropNames->add(p, propName)) {
          js::ReportOutOfMemory(fc);
          return false;
        }
#endif
        writer.setPropNameNoDuplicateCheck(parserAtoms(), propName);
      } else {
        if (!writer.setPropName(parserAtoms(), key->as<NameNode>().atom())) {
          return false;
        }
      }
    } else {
      double numValue = key->as<NumericLiteral>().value();
      int32_t i = 0;
      DebugOnly<bool> numIsInt =
          NumberIsInt32(numValue, &i);  // checked previously.
      MOZ_ASSERT(numIsInt);
      MOZ_ASSERT(
          ObjLiteralWriter::arrayIndexInRange(i));  // checked previously.

      // Ignore indexed properties if we're not storing property values, and
      // rely on InitElem ops to define those. These properties will be either
      // dense elements (not possible to represent in the literal's shape) or
      // sparse elements (enumerated separately, so this doesn't affect property
      // iteration order).
      if (!useObjLiteralValues) {
        continue;
      }

      writer.setPropIndex(i);
    }

    if (useObjLiteralValues) {
      MOZ_ASSERT(op == JSOp::Object);
      ParseNode* value = prop->right();
      if (!emitObjLiteralValue(writer, value)) {
        return false;
      }
    } else {
      if (!writer.propWithUndefinedValue(fc)) {
        return false;
      }
    }
  }

  GCThingIndex index;
  if (!addObjLiteralData(writer, &index)) {
    return false;
  }

  // JSOp::Object may only be used by (top-level) run-once scripts.
  MOZ_ASSERT_IF(op == JSOp::Object,
                sc->isTopLevelContext() && sc->treatAsRunOnce());

  if (!emitGCIndexOp(op, index)) {
    //              [stack] OBJ
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitDestructuringRestExclusionSetObjLiteral(
    ListNode* pattern) {
  // Note: if we want to squeeze out a little more performance, we could switch
  // to the `JSOp::Object` opcode, because the exclusion set object is never
  // exposed to the user, so it's safe to bake the object into the bytecode.
  constexpr JSOp op = JSOp::NewObject;

  ObjLiteralWriter writer;
  writer.beginShape(op);

  for (ParseNode* member : pattern->contents()) {
    if (member->isKind(ParseNodeKind::Spread)) {
      MOZ_ASSERT(!member->pn_next, "unexpected trailing element after spread");
      break;
    }

    TaggedParserAtomIndex atom;
    if (member->isKind(ParseNodeKind::MutateProto)) {
      atom = TaggedParserAtomIndex::WellKnown::proto();
    } else {
      ParseNode* key = member->as<BinaryNode>().left();
      atom = key->as<NameNode>().atom();
    }

    if (!writer.setPropName(parserAtoms(), atom)) {
      return false;
    }

    if (!writer.propWithUndefinedValue(fc)) {
      return false;
    }
  }

  GCThingIndex index;
  if (!addObjLiteralData(writer, &index)) {
    return false;
  }

  if (!emitGCIndexOp(op, index)) {
    //              [stack] OBJ
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitObjLiteralArray(ListNode* array) {
  MOZ_ASSERT(checkSingletonContext());

  constexpr JSOp op = JSOp::Object;

  ObjLiteralWriter writer;
  writer.beginArray(op);

  writer.beginDenseArrayElements();
  for (ParseNode* elem : array->contents()) {
    if (!emitObjLiteralValue(writer, elem)) {
      return false;
    }
  }

  GCThingIndex index;
  if (!addObjLiteralData(writer, &index)) {
    return false;
  }

  if (!emitGCIndexOp(op, index)) {
    //              [stack] OBJ
    return false;
  }

  return true;
}

bool BytecodeEmitter::isRHSObjLiteralCompatible(ParseNode* value) {
  return value->isKind(ParseNodeKind::NumberExpr) ||
         value->isKind(ParseNodeKind::TrueExpr) ||
         value->isKind(ParseNodeKind::FalseExpr) ||
         value->isKind(ParseNodeKind::NullExpr) ||
         value->isKind(ParseNodeKind::RawUndefinedExpr) ||
         value->isKind(ParseNodeKind::StringExpr) ||
         value->isKind(ParseNodeKind::TemplateStringExpr);
}

bool BytecodeEmitter::emitObjLiteralValue(ObjLiteralWriter& writer,
                                          ParseNode* value) {
  MOZ_ASSERT(isRHSObjLiteralCompatible(value));
  if (value->isKind(ParseNodeKind::NumberExpr)) {
    double numValue = value->as<NumericLiteral>().value();
    int32_t i = 0;
    js::Value v;
    if (NumberIsInt32(numValue, &i)) {
      v.setInt32(i);
    } else {
      v.setDouble(numValue);
    }
    if (!writer.propWithConstNumericValue(fc, v)) {
      return false;
    }
  } else if (value->isKind(ParseNodeKind::TrueExpr)) {
    if (!writer.propWithTrueValue(fc)) {
      return false;
    }
  } else if (value->isKind(ParseNodeKind::FalseExpr)) {
    if (!writer.propWithFalseValue(fc)) {
      return false;
    }
  } else if (value->isKind(ParseNodeKind::NullExpr)) {
    if (!writer.propWithNullValue(fc)) {
      return false;
    }
  } else if (value->isKind(ParseNodeKind::RawUndefinedExpr)) {
    if (!writer.propWithUndefinedValue(fc)) {
      return false;
    }
  } else if (value->isKind(ParseNodeKind::StringExpr) ||
             value->isKind(ParseNodeKind::TemplateStringExpr)) {
    if (!writer.propWithAtomValue(fc, parserAtoms(),
                                  value->as<NameNode>().atom())) {
      return false;
    }
  } else {
    MOZ_CRASH("Unexpected parse node");
  }
  return true;
}

static bool NeedsPrivateBrand(ParseNode* member) {
  return member->is<ClassMethod>() &&
         member->as<ClassMethod>().name().isKind(ParseNodeKind::PrivateName) &&
         !member->as<ClassMethod>().isStatic();
}

mozilla::Maybe<MemberInitializers> BytecodeEmitter::setupMemberInitializers(
    ListNode* classMembers, FieldPlacement placement) {
  bool isStatic = placement == FieldPlacement::Static;

  size_t numFields = 0;
  size_t numPrivateInitializers = 0;
  bool hasPrivateBrand = false;
  for (ParseNode* member : classMembers->contents()) {
    if (NeedsFieldInitializer(member, isStatic)) {
      numFields++;
    } else if (NeedsAccessorInitializer(member, isStatic)) {
      numPrivateInitializers++;
      hasPrivateBrand = true;
    } else if (NeedsPrivateBrand(member)) {
      hasPrivateBrand = true;
    }
  }

  // If there are more initializers than can be represented, return invalid.
  if (numFields + numPrivateInitializers >
      MemberInitializers::MaxInitializers) {
    return Nothing();
  }
  return Some(
      MemberInitializers(hasPrivateBrand, numFields + numPrivateInitializers));
}

// Purpose of .fieldKeys:
// Computed field names (`["x"] = 2;`) must be ran at class-evaluation time,
// not object construction time. The transformation to do so is roughly as
// follows:
//
// class C {
//   [keyExpr] = valueExpr;
// }
// -->
// let .fieldKeys = [keyExpr];
// let .initializers = [
//   () => {
//     this[.fieldKeys[0]] = valueExpr;
//   }
// ];
// class C {
//   constructor() {
//     .initializers[0]();
//   }
// }
//
// BytecodeEmitter::emitCreateFieldKeys does `let .fieldKeys = [...];`
// BytecodeEmitter::emitPropertyList fills in the elements of the array.
// See GeneralParser::fieldInitializer for the `this[.fieldKeys[0]]` part.
bool BytecodeEmitter::emitCreateFieldKeys(ListNode* obj,
                                          FieldPlacement placement) {
  bool isStatic = placement == FieldPlacement::Static;
  auto isFieldWithComputedName = [isStatic](ParseNode* propdef) {
    return propdef->is<ClassField>() &&
           propdef->as<ClassField>().isStatic() == isStatic &&
           propdef->as<ClassField>().name().getKind() ==
               ParseNodeKind::ComputedName;
  };

  size_t numFieldKeys = std::count_if(
      obj->contents().begin(), obj->contents().end(), isFieldWithComputedName);
  if (numFieldKeys == 0) {
    return true;
  }

  auto fieldKeys = isStatic
                       ? TaggedParserAtomIndex::WellKnown::dotStaticFieldKeys()
                       : TaggedParserAtomIndex::WellKnown::dotFieldKeys();
  NameOpEmitter noe(this, fieldKeys, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    return false;
  }

  if (!emitUint32Operand(JSOp::NewArray, numFieldKeys)) {
    //              [stack] ARRAY
    return false;
  }

  if (!noe.emitAssignment()) {
    //              [stack] ARRAY
    return false;
  }

  if (!emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  return true;
}

static bool HasInitializer(ParseNode* node, bool isStaticContext) {
  return (node->is<ClassField>() &&
          node->as<ClassField>().isStatic() == isStaticContext) ||
         (isStaticContext && node->is<StaticClassBlock>());
}

static FunctionNode* GetInitializer(ParseNode* node, bool isStaticContext) {
  MOZ_ASSERT(HasInitializer(node, isStaticContext));
  MOZ_ASSERT_IF(!node->is<ClassField>(), isStaticContext);
  return node->is<ClassField>() ? node->as<ClassField>().initializer()
                                : node->as<StaticClassBlock>().function();
}

bool BytecodeEmitter::emitCreateMemberInitializers(ClassEmitter& ce,
                                                   ListNode* obj,
                                                   FieldPlacement placement) {
  // FieldPlacement::Instance
  //                [stack] HOMEOBJ HERITAGE?
  //
  // FieldPlacement::Static
  //                [stack] CTOR HOMEOBJ
  mozilla::Maybe<MemberInitializers> memberInitializers =
      setupMemberInitializers(obj, placement);
  if (!memberInitializers) {
    ReportAllocationOverflow(fc);
    return false;
  }

  size_t numInitializers = memberInitializers->numMemberInitializers;
  if (numInitializers == 0) {
    return true;
  }

  bool isStatic = placement == FieldPlacement::Static;
  if (!ce.prepareForMemberInitializers(numInitializers, isStatic)) {
    //              [stack] HOMEOBJ HERITAGE? ARRAY
    // or:
    //              [stack] CTOR HOMEOBJ ARRAY
    return false;
  }

  // Private accessors could be used in the field initializers, so make sure
  // accessor initializers appear earlier in the .initializers array so they
  // run first. Static private methods are not initialized using initializers
  // (emitPropertyList emits bytecode to stamp them onto the constructor), so
  // skip this step if isStatic.
  if (!isStatic) {
    if (!emitPrivateMethodInitializers(ce, obj)) {
      return false;
    }
  }

  for (ParseNode* propdef : obj->contents()) {
    if (!HasInitializer(propdef, isStatic)) {
      continue;
    }

    FunctionNode* initializer = GetInitializer(propdef, isStatic);

    if (!ce.prepareForMemberInitializer()) {
      return false;
    }
    if (!emitTree(initializer)) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY LAMBDA
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY LAMBDA
      return false;
    }
    if (initializer->funbox()->needsHomeObject()) {
      MOZ_ASSERT(initializer->funbox()->allowSuperProperty());
      if (!ce.emitMemberInitializerHomeObject(isStatic)) {
        //          [stack] HOMEOBJ HERITAGE? ARRAY LAMBDA
        // or:
        //          [stack] CTOR HOMEOBJ ARRAY LAMBDA
        return false;
      }
    }
    if (!ce.emitStoreMemberInitializer()) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY
      return false;
    }
  }

#ifdef ENABLE_DECORATORS
  // Index to use to append new initializers returned by decorators to the array
  if (!emitNumberOp(numInitializers)) {
    //            [stack] HOMEOBJ HERITAGE? ARRAY INDEX
    // or:
    //            [stack] CTOR HOMEOBJ ARRAY INDEX
    return false;
  }

  for (ParseNode* propdef : obj->contents()) {
    if (!propdef->is<ClassField>()) {
      continue;
    }
    ClassField* field = &propdef->as<ClassField>();
    if (placement == FieldPlacement::Static && !field->isStatic()) {
      continue;
    }
    if (field->decorators() && !field->decorators()->empty()) {
      DecoratorEmitter de(this);
      if (!de.emitApplyDecoratorsToFieldDefinition(
              &field->name(), field->decorators(), field->isStatic())) {
        //                [stack] HOMEOBJ HERITAGE? ARRAY INDEX INITIALIZERS
        // or:
        //                [stack] CTOR HOMEOBJ ARRAY INDEX INITIALIZERS
        return false;
      }

      if (!emit1(JSOp::InitElemInc)) {
        //                [stack] HOMEOBJ HERITAGE? ARRAY INDEX
        // or:
        //                [stack] CTOR HOMEOBJ ARRAY INDEX
        return false;
      }
    }
  }

  // Pop INDEX
  if (!emitPopN(1)) {
    //                [stack] HOMEOBJ HERITAGE? ARRAY
    // or:
    //                [stack] CTOR HOMEOBJ ARRAY
    return false;
  }
#endif

  if (!ce.emitMemberInitializersEnd()) {
    //              [stack] HOMEOBJ HERITAGE?
    // or:
    //              [stack] CTOR HOMEOBJ
    return false;
  }

  return true;
}

static bool IsPrivateInstanceAccessor(const ClassMethod* classMethod) {
  return !classMethod->isStatic() &&
         classMethod->name().isKind(ParseNodeKind::PrivateName) &&
         classMethod->accessorType() != AccessorType::None;
}

bool BytecodeEmitter::emitPrivateMethodInitializers(ClassEmitter& ce,
                                                    ListNode* obj) {
  for (ParseNode* propdef : obj->contents()) {
    if (!propdef->is<ClassMethod>()) {
      continue;
    }
    auto* classMethod = &propdef->as<ClassMethod>();

    // Skip over anything which isn't a private instance accessor.
    if (!IsPrivateInstanceAccessor(classMethod)) {
      continue;
    }

    if (!ce.prepareForMemberInitializer()) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY
      return false;
    }

    // Synthesize a name for the lexical variable that will store the
    // private method body.
    TaggedParserAtomIndex name = classMethod->name().as<NameNode>().atom();
    AccessorType accessorType = classMethod->accessorType();
    StringBuffer storedMethodName(fc);
    if (!storedMethodName.append(parserAtoms(), name)) {
      return false;
    }
    if (!((accessorType == AccessorType::Getter)
              ? storedMethodName.append(".getter")
              : storedMethodName.append(".setter"))) {
      return false;
    }
    auto storedMethodAtom =
        storedMethodName.finishParserAtom(parserAtoms(), fc);

    // Emit the private method body and store it as a lexical var.
    if (!emitFunction(&classMethod->method())) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY METHOD
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY METHOD
      return false;
    }
    // The private method body needs to access the home object,
    // and the CE knows where that is on the stack.
    if (classMethod->method().funbox()->needsHomeObject()) {
      if (!ce.emitMemberInitializerHomeObject(false)) {
        //          [stack] HOMEOBJ HERITAGE? ARRAY METHOD
        // or:
        //          [stack] CTOR HOMEOBJ ARRAY METHOD
        return false;
      }
    }
    if (!emitLexicalInitialization(storedMethodAtom)) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY METHOD
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY METHOD
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY
      return false;
    }

    if (!emitPrivateMethodInitializer(classMethod, storedMethodAtom)) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY
      return false;
    }

    // Store the emitted initializer function into the .initializers array.
    if (!ce.emitStoreMemberInitializer()) {
      //            [stack] HOMEOBJ HERITAGE? ARRAY
      // or:
      //            [stack] CTOR HOMEOBJ ARRAY
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitPrivateMethodInitializer(
    ClassMethod* classMethod, TaggedParserAtomIndex storedMethodAtom) {
  MOZ_ASSERT(IsPrivateInstanceAccessor(classMethod));

  auto* name = &classMethod->name().as<NameNode>();

  // Emit the synthesized initializer function.
  FunctionNode* funNode = classMethod->initializerIfPrivate();
  MOZ_ASSERT(funNode);
  FunctionBox* funbox = funNode->funbox();
  FunctionEmitter fe(this, funbox, funNode->syntaxKind(),
                     FunctionEmitter::IsHoisted::No);
  if (!fe.prepareForNonLazy()) {
    //              [stack]
    return false;
  }

  BytecodeEmitter bce2(this, funbox);
  if (!bce2.init(funNode->pn_pos)) {
    return false;
  }
  ParamsBodyNode* paramsBody = funNode->body();
  FunctionScriptEmitter fse(&bce2, funbox, Nothing(), Nothing());
  if (!fse.prepareForParameters()) {
    //              [stack]
    return false;
  }
  if (!bce2.emitFunctionFormalParameters(paramsBody)) {
    //              [stack]
    return false;
  }
  if (!fse.prepareForBody()) {
    //              [stack]
    return false;
  }

  if (!bce2.emit1(JSOp::FunctionThis)) {
    //              [stack] THIS
    return false;
  }
  if (!bce2.emitGetPrivateName(name)) {
    //              [stack] THIS NAME
    return false;
  }
  if (!bce2.emitGetName(storedMethodAtom)) {
    //              [stack] THIS NAME METHOD
    return false;
  }

  switch (name->privateNameKind()) {
    case PrivateNameKind::Setter:
      if (!bce2.emit1(JSOp::InitHiddenElemSetter)) {
        //          [stack] THIS
        return false;
      }
      if (!bce2.emitGetPrivateName(name)) {
        //          [stack] THIS NAME
        return false;
      }
      if (!bce2.emitAtomOp(
              JSOp::GetIntrinsic,
              TaggedParserAtomIndex::WellKnown::NoPrivateGetter())) {
        //          [stack] THIS NAME FUN
        return false;
      }
      if (!bce2.emit1(JSOp::InitHiddenElemGetter)) {
        //          [stack] THIS
        return false;
      }
      break;
    case PrivateNameKind::Getter:
    case PrivateNameKind::GetterSetter:
      if (classMethod->accessorType() == AccessorType::Getter) {
        if (!bce2.emit1(JSOp::InitHiddenElemGetter)) {
          //        [stack] THIS
          return false;
        }
      } else {
        if (!bce2.emit1(JSOp::InitHiddenElemSetter)) {
          //        [stack] THIS
          return false;
        }
      }
      break;
    default:
      MOZ_CRASH("Invalid op");
  }

  // Pop remaining THIS.
  if (!bce2.emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  if (!fse.emitEndBody()) {
    //              [stack]
    return false;
  }
  if (!fse.intoStencil()) {
    return false;
  }

  if (!fe.emitNonLazyEnd()) {
    //              [stack] HOMEOBJ HERITAGE? ARRAY FUN
    // or:
    //              [stack] CTOR HOMEOBJ ARRAY FUN
    return false;
  }

  return true;
}

const MemberInitializers& BytecodeEmitter::findMemberInitializersForCall() {
  for (BytecodeEmitter* current = this; current; current = current->parent) {
    if (current->sc->isFunctionBox()) {
      FunctionBox* funbox = current->sc->asFunctionBox();

      if (funbox->isArrow()) {
        continue;
      }

      // If we found a non-arrow / non-constructor we were never allowed to
      // expect fields in the first place.
      MOZ_RELEASE_ASSERT(funbox->isClassConstructor());

      return funbox->useMemberInitializers() ? funbox->memberInitializers()
                                             : MemberInitializers::Empty();
    }
  }

  MOZ_RELEASE_ASSERT(compilationState.scopeContext.memberInitializers);
  return *compilationState.scopeContext.memberInitializers;
}

bool BytecodeEmitter::emitInitializeInstanceMembers(
    bool isDerivedClassConstructor) {
  const MemberInitializers& memberInitializers =
      findMemberInitializersForCall();
  MOZ_ASSERT(memberInitializers.valid);

  if (memberInitializers.hasPrivateBrand) {
    // This is guaranteed to run after super(), so we don't need TDZ checks.
    if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
      //            [stack] THIS
      return false;
    }
    if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotPrivateBrand())) {
      //            [stack] THIS BRAND
      return false;
    }
    if (isDerivedClassConstructor) {
      if (!emitCheckPrivateField(ThrowCondition::ThrowHas,
                                 ThrowMsgKind::PrivateBrandDoubleInit)) {
        //          [stack] THIS BRAND BOOL
        return false;
      }
      if (!emit1(JSOp::Pop)) {
        //          [stack] THIS BRAND
        return false;
      }
    }
    if (!emit1(JSOp::Null)) {
      //            [stack] THIS BRAND NULL
      return false;
    }
    if (!emit1(JSOp::InitHiddenElem)) {
      //            [stack] THIS
      return false;
    }
    if (!emit1(JSOp::Pop)) {
      //            [stack]
      return false;
    }
  }

  size_t numInitializers = memberInitializers.numMemberInitializers;
  if (numInitializers > 0) {
    if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotInitializers())) {
      //              [stack] ARRAY
      return false;
    }

    for (size_t index = 0; index < numInitializers; index++) {
      if (index < numInitializers - 1) {
        // We Dup to keep the array around (it is consumed in the bytecode
        // below) for next iterations of this loop, except for the last
        // iteration, which avoids an extra Pop at the end of the loop.
        if (!emit1(JSOp::Dup)) {
          //          [stack] ARRAY ARRAY
          return false;
        }
      }

      if (!emitNumberOp(index)) {
        //            [stack] ARRAY? ARRAY INDEX
        return false;
      }

      if (!emit1(JSOp::GetElem)) {
        //            [stack] ARRAY? FUNC
        return false;
      }

      // This is guaranteed to run after super(), so we don't need TDZ checks.
      if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
        //            [stack] ARRAY? FUNC THIS
        return false;
      }

      // Callee is always internal function.
      if (!emitCall(JSOp::CallIgnoresRv, 0)) {
        //            [stack] ARRAY? RVAL
        return false;
      }

      if (!emit1(JSOp::Pop)) {
        //            [stack] ARRAY?
        return false;
      }
    }
#ifdef ENABLE_DECORATORS
    // Decorators Proposal
    // https://arai-a.github.io/ecma262-compare/?pr=2417&id=sec-initializeinstanceelements
    // 4. For each element e of elements, do
    //     4.a. If elementRecord.[[Kind]] is field or accessor, then
    //         4.a.i. Perform ? InitializeFieldOrAccessor(O, elementRecord).
    //

    // TODO: (See Bug 1817993) At the moment, we're applying the initialization
    // logic in two steps. The pre-decorator initialization code runs, stores
    // the initial value, and then we retrieve it here and apply the
    // initializers added by decorators. We should unify these two steps.
    if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotInitializers())) {
      //              [stack] ARRAY
      return false;
    }

    if (!emit1(JSOp::Dup)) {
      //          [stack] ARRAY ARRAY
      return false;
    }

    if (!emitAtomOp(JSOp::GetProp,
                    TaggedParserAtomIndex::WellKnown::length())) {
      //          [stack] ARRAY LENGTH
      return false;
    }

    if (!emitNumberOp(static_cast<double>(numInitializers))) {
      //          [stack] ARRAY LENGTH INDEX
      return false;
    }

    WhileEmitter wh(this);
    // At this point, we have no context to determine offsets in the
    // code for this while statement. Ideally, it would correspond to
    // the field we're initializing.
    if (!wh.emitCond(0, 0, 0)) {
      //          [stack] ARRAY LENGTH INDEX
      return false;
    }

    if (!emit1(JSOp::Dup)) {
      //          [stack] ARRAY LENGTH INDEX INDEX
      return false;
    }

    if (!emitDupAt(2)) {
      //          [stack] ARRAY LENGTH INDEX INDEX LENGTH
      return false;
    }

    if (!emit1(JSOp::Lt)) {
      //          [stack] ARRAY LENGTH INDEX BOOL
      return false;
    }

    if (!wh.emitBody()) {
      //          [stack] ARRAY LENGTH INDEX
      return false;
    }

    if (!emitDupAt(2)) {
      //          [stack] ARRAY LENGTH INDEX ARRAY
      return false;
    }

    if (!emitDupAt(1)) {
      //          [stack] ARRAY LENGTH INDEX ARRAY INDEX
      return false;
    }

    // Retrieve initializers for this field
    if (!emit1(JSOp::GetElem)) {
      //            [stack] ARRAY LENGTH INDEX INITIALIZERS
      return false;
    }

    // This is guaranteed to run after super(), so we don't need TDZ checks.
    if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotThis())) {
      //            [stack] ARRAY LENGTH INDEX INITIALIZERS THIS
      return false;
    }

    if (!emit1(JSOp::Swap)) {
      //            [stack] ARRAY LENGTH INDEX THIS INITIALIZERS
      return false;
    }

    DecoratorEmitter de(this);
    if (!de.emitInitializeFieldOrAccessor()) {
      //            [stack] ARRAY LENGTH INDEX
      return false;
    }

    if (!emit1(JSOp::Inc)) {
      //            [stack] ARRAY LENGTH INDEX
      return false;
    }

    if (!wh.emitEnd()) {
      //          [stack] ARRAY LENGTH INDEX
      return false;
    }

    if (!emitPopN(3)) {
      //            [stack]
      return false;
    }
    // 5. Return unused.
#endif
  }
  return true;
}

bool BytecodeEmitter::emitInitializeStaticFields(ListNode* classMembers) {
  auto isStaticField = [](ParseNode* propdef) {
    return HasInitializer(propdef, true);
  };
  size_t numFields =
      std::count_if(classMembers->contents().begin(),
                    classMembers->contents().end(), isStaticField);

  if (numFields == 0) {
    return true;
  }

  if (!emitGetName(TaggedParserAtomIndex::WellKnown::dotStaticInitializers())) {
    //              [stack] CTOR ARRAY
    return false;
  }

  for (size_t fieldIndex = 0; fieldIndex < numFields; fieldIndex++) {
    bool hasNext = fieldIndex < numFields - 1;
    if (hasNext) {
      // We Dup to keep the array around (it is consumed in the bytecode below)
      // for next iterations of this loop, except for the last iteration, which
      // avoids an extra Pop at the end of the loop.
      if (!emit1(JSOp::Dup)) {
        //          [stack] CTOR ARRAY ARRAY
        return false;
      }
    }

    if (!emitNumberOp(fieldIndex)) {
      //            [stack] CTOR ARRAY? ARRAY INDEX
      return false;
    }

    if (!emit1(JSOp::GetElem)) {
      //            [stack] CTOR ARRAY? FUNC
      return false;
    }

    if (!emitDupAt(1 + hasNext)) {
      //            [stack] CTOR ARRAY? FUNC CTOR
      return false;
    }

    // Callee is always internal function.
    if (!emitCall(JSOp::CallIgnoresRv, 0)) {
      //            [stack] CTOR ARRAY? RVAL
      return false;
    }

    if (!emit1(JSOp::Pop)) {
      //            [stack] CTOR ARRAY?
      return false;
    }
  }

  // Overwrite |.staticInitializers| and |.staticFieldKeys| with undefined to
  // avoid keeping the arrays alive indefinitely.
  auto clearStaticFieldSlot = [&](TaggedParserAtomIndex name) {
    NameOpEmitter noe(this, name, NameOpEmitter::Kind::SimpleAssignment);
    if (!noe.prepareForRhs()) {
      //            [stack] ENV? VAL?
      return false;
    }

    if (!emit1(JSOp::Undefined)) {
      //            [stack] ENV? VAL? UNDEFINED
      return false;
    }

    if (!noe.emitAssignment()) {
      //            [stack] VAL
      return false;
    }

    if (!emit1(JSOp::Pop)) {
      //            [stack]
      return false;
    }

    return true;
  };

  if (!clearStaticFieldSlot(
          TaggedParserAtomIndex::WellKnown::dotStaticInitializers())) {
    return false;
  }

  auto isStaticFieldWithComputedName = [](ParseNode* propdef) {
    return propdef->is<ClassField>() && propdef->as<ClassField>().isStatic() &&
           propdef->as<ClassField>().name().getKind() ==
               ParseNodeKind::ComputedName;
  };

  if (std::any_of(classMembers->contents().begin(),
                  classMembers->contents().end(),
                  isStaticFieldWithComputedName)) {
    if (!clearStaticFieldSlot(
            TaggedParserAtomIndex::WellKnown::dotStaticFieldKeys())) {
      return false;
    }
  }

  return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool BytecodeEmitter::emitObject(ListNode* objNode) {
  // Note: this method uses the ObjLiteralWriter and emits ObjLiteralStencil
  // objects into the GCThingList, which will evaluate them into real GC objects
  // or shapes during JSScript::fullyInitFromEmitter. Eventually we want
  // OBJLITERAL to be a real opcode, but for now, performance constraints limit
  // us to evaluating object literals at the end of parse, when we're allowed to
  // allocate GC things.
  //
  // There are four cases here, in descending order of preference:
  //
  // 1. The list of property names is "normal" and constant (no computed
  //    values, no integer indices), the values are all simple constants
  //    (numbers, booleans, strings), *and* this occurs in a run-once
  //    (singleton) context. In this case, we can emit ObjLiteral
  //    instructions to build an object with values, and the object will be
  //    attached to a JSOp::Object opcode, whose semantics are for the backend
  //    to simply steal the object from the script.
  //
  // 2. The list of property names is "normal" and constant as above, *and* this
  //    occurs in a run-once (singleton) context, but some values are complex
  //    (computed expressions, sub-objects, functions, etc.). In this case, we
  //    can still use JSOp::Object (because singleton context), but the object
  //    has |undefined| property values and InitProp ops are emitted to set the
  //    values.
  //
  // 3. The list of property names is "normal" and constant as above, but this
  //    occurs in a non-run-once (non-singleton) context. In this case, we can
  //    use the ObjLiteral functionality to describe an *empty* object (all
  //    values left undefined) with the right fields, which will become a
  //    JSOp::NewObject opcode using the object's shape to speed up the creation
  //    of the object each time it executes. The emitted bytecode still needs
  //    InitProp ops to set the values in this case.
  //
  // 4. Any other case. As a fallback, we use NewInit to create a new, empty
  //    object (i.e., `{}`) and then emit bytecode to initialize its properties
  //    one-by-one.

  bool useObjLiteral = false;
  bool useObjLiteralValues = false;
  isPropertyListObjLiteralCompatible(objNode, &useObjLiteralValues,
                                     &useObjLiteral);

  //                [stack]
  //
  ObjectEmitter oe(this);
  if (useObjLiteral) {
    bool singleton = checkSingletonContext() &&
                     !objNode->hasNonConstInitializer() && objNode->head();
    JSOp op;
    if (singleton) {
      // Case 1 or 2.
      op = JSOp::Object;
    } else {
      // Case 3.
      useObjLiteralValues = false;
      op = JSOp::NewObject;
    }

    // Use an ObjLiteral op. This will record ObjLiteral insns in the
    // objLiteralWriter's buffer and add a fixup to the list of ObjLiteral
    // fixups so that at GC-publish time at the end of parse, the full object
    // (case 1 or 2) or shape (case 3) can be allocated and the bytecode can be
    // patched to refer to it.
    if (!emitPropertyListObjLiteral(objNode, op, useObjLiteralValues)) {
      //            [stack] OBJ
      return false;
    }
    // Put the ObjectEmitter in the right state. This tells it that there will
    // already be an object on the stack as a result of the (eventual)
    // NewObject or Object op, and prepares it to emit values if needed.
    if (!oe.emitObjectWithTemplateOnStack()) {
      //            [stack] OBJ
      return false;
    }
    if (!useObjLiteralValues) {
      // Case 2 or 3 above: we still need to emit bytecode to fill in the
      // object's property values.
      if (!emitPropertyList(objNode, oe, ObjectLiteral)) {
        //          [stack] OBJ
        return false;
      }
    }
  } else {
    // Case 4 above: no ObjLiteral use, just bytecode to build the object from
    // scratch.
    if (!oe.emitObject(objNode->count())) {
      //            [stack] OBJ
      return false;
    }
    if (!emitPropertyList(objNode, oe, ObjectLiteral)) {
      //            [stack] OBJ
      return false;
    }
  }

  if (!oe.emitEnd()) {
    //              [stack] OBJ
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitArrayLiteral(ListNode* array) {
  // Emit JSOp::Object if the array consists entirely of primitive values and we
  // are in a singleton context.
  if (checkSingletonContext() && !array->hasNonConstInitializer() &&
      !array->empty() && isArrayObjLiteralCompatible(array)) {
    return emitObjLiteralArray(array);
  }

  return emitArray(array);
}

bool BytecodeEmitter::emitArray(ListNode* array) {
  /*
   * Emit code for [a, b, c] that is equivalent to constructing a new
   * array and in source order evaluating each element value and adding
   * it to the array, without invoking latent setters.  We use the
   * JSOp::NewInit and JSOp::InitElemArray bytecodes to ignore setters and
   * to avoid dup'ing and popping the array as each element is added, as
   * JSOp::SetElem/JSOp::SetProp would do.
   */

  uint32_t nspread = 0;
  for (ParseNode* elem : array->contents()) {
    if (elem->isKind(ParseNodeKind::Spread)) {
      nspread++;
    }
  }

  // Array literal's length is limited to NELEMENTS_LIMIT in parser.
  static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                "array literals' maximum length must not exceed limits "
                "required by BaselineCompiler::emit_NewArray, "
                "BaselineCompiler::emit_InitElemArray, "
                "and DoSetElemFallback's handling of JSOp::InitElemArray");

  uint32_t count = array->count();
  MOZ_ASSERT(count >= nspread);
  MOZ_ASSERT(count <= NativeObject::MAX_DENSE_ELEMENTS_COUNT,
             "the parser must throw an error if the array exceeds maximum "
             "length");

  // For arrays with spread, this is a very pessimistic allocation, the
  // minimum possible final size.
  if (!emitUint32Operand(JSOp::NewArray, count - nspread)) {
    //              [stack] ARRAY
    return false;
  }

  uint32_t index = 0;
  bool afterSpread = false;
  for (ParseNode* elem : array->contents()) {
    if (elem->isKind(ParseNodeKind::Spread)) {
      if (!afterSpread) {
        afterSpread = true;
        if (!emitNumberOp(index)) {
          //        [stack] ARRAY INDEX
          return false;
        }
      }

      ParseNode* expr = elem->as<UnaryNode>().kid();
      SelfHostedIter selfHostedIter = getSelfHostedIterFor(expr);

      if (!updateSourceCoordNotes(elem->pn_pos.begin)) {
        return false;
      }
      if (!emitTree(expr, ValueUsage::WantValue)) {
        //          [stack] ARRAY INDEX VALUE
        return false;
      }
      if (!emitIterator(selfHostedIter)) {
        //          [stack] ARRAY INDEX NEXT ITER
        return false;
      }
      if (!emit2(JSOp::Pick, 3)) {
        //          [stack] INDEX NEXT ITER ARRAY
        return false;
      }
      if (!emit2(JSOp::Pick, 3)) {
        //          [stack] NEXT ITER ARRAY INDEX
        return false;
      }
      if (!emitSpread(selfHostedIter)) {
        //          [stack] ARRAY INDEX
        return false;
      }
    } else {
      if (!updateSourceCoordNotes(elem->pn_pos.begin)) {
        return false;
      }
      if (elem->isKind(ParseNodeKind::Elision)) {
        if (!emit1(JSOp::Hole)) {
          return false;
        }
      } else {
        if (!emitTree(elem, ValueUsage::WantValue)) {
          //        [stack] ARRAY INDEX? VALUE
          return false;
        }
      }

      if (afterSpread) {
        if (!emit1(JSOp::InitElemInc)) {
          //        [stack] ARRAY (INDEX+1)
          return false;
        }
      } else {
        if (!emitUint32Operand(JSOp::InitElemArray, index)) {
          //        [stack] ARRAY
          return false;
        }
      }
    }

    index++;
  }
  MOZ_ASSERT(index == count);
  if (afterSpread) {
    if (!emit1(JSOp::Pop)) {
      //            [stack] ARRAY
      return false;
    }
  }
  return true;
}

bool BytecodeEmitter::emitSpreadIntoArray(UnaryNode* elem) {
  MOZ_ASSERT(elem->isKind(ParseNodeKind::Spread));

  if (!updateSourceCoordNotes(elem->pn_pos.begin)) {
    //              [stack] VALUE
    return false;
  }

  SelfHostedIter selfHostedIter = getSelfHostedIterFor(elem->kid());
  if (!emitIterator(selfHostedIter)) {
    //              [stack] NEXT ITER
    return false;
  }

  if (!emitUint32Operand(JSOp::NewArray, 0)) {
    //              [stack] NEXT ITER ARRAY
    return false;
  }

  if (!emitNumberOp(0)) {
    //              [stack] NEXT ITER ARRAY INDEX
    return false;
  }

  if (!emitSpread(selfHostedIter)) {
    //              [stack] ARRAY INDEX
    return false;
  }

  if (!emit1(JSOp::Pop)) {
    //              [stack] ARRAY
    return false;
  }
  return true;
}

#ifdef ENABLE_RECORD_TUPLE
bool BytecodeEmitter::emitRecordLiteral(ListNode* record) {
  if (!emitUint32Operand(JSOp::InitRecord, record->count())) {
    //              [stack] RECORD
    return false;
  }

  for (ParseNode* propdef : record->contents()) {
    if (propdef->isKind(ParseNodeKind::Spread)) {
      if (!emitTree(propdef->as<UnaryNode>().kid())) {
        //          [stack] RECORD SPREADEE
        return false;
      }
      if (!emit1(JSOp::AddRecordSpread)) {
        //          [stack] RECORD
        return false;
      }
    } else {
      BinaryNode* prop = &propdef->as<BinaryNode>();

      ParseNode* key = prop->left();
      ParseNode* value = prop->right();

      switch (key->getKind()) {
        case ParseNodeKind::ObjectPropertyName:
          if (!emitStringOp(JSOp::String, key->as<NameNode>().atom())) {
            return false;
          }
          break;
        case ParseNodeKind::ComputedName:
          if (!emitTree(key->as<UnaryNode>().kid())) {
            return false;
          }
          break;
        default:
          MOZ_ASSERT(key->isKind(ParseNodeKind::StringExpr) ||
                     key->isKind(ParseNodeKind::NumberExpr) ||
                     key->isKind(ParseNodeKind::BigIntExpr));
          if (!emitTree(key)) {
            return false;
          }
          break;
      }
      //            [stack] RECORD KEY

      if (!emitTree(value)) {
        //          [stack] RECORD KEY VALUE
        return false;
      }

      if (!emit1(JSOp::AddRecordProperty)) {
        //          [stack] RECORD
        return false;
      }
    }
  }

  if (!emit1(JSOp::FinishRecord)) {
    //              [stack] RECORD
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitTupleLiteral(ListNode* tuple) {
  if (!emitUint32Operand(JSOp::InitTuple, tuple->count())) {
    //              [stack] TUPLE
    return false;
  }

  for (ParseNode* elt : tuple->contents()) {
    if (elt->isKind(ParseNodeKind::Spread)) {
      ParseNode* expr = elt->as<UnaryNode>().kid();

      if (!emitTree(expr)) {
        //          [stack] TUPLE VALUE
        return false;
      }
      if (!emitIterator()) {
        //          [stack] TUPLE NEXT ITER
        return false;
      }
      if (!emit2(JSOp::Pick, 2)) {
        //          [stack] NEXT ITER TUPLE
        return false;
      }
      if (!emitSpread(getSelfHostedIterFor(expr), /* spreadeeStackItems = */ 1,
                      JSOp::AddTupleElement)) {
        //          [stack] TUPLE
        return false;
      }
    } else {
      if (!emitTree(elt)) {
        //          [stack] TUPLE VALUE
        return false;
      }

      // Update location to throw errors about non-primitive elements
      // in the correct position.
      if (!updateSourceCoordNotes(elt->pn_pos.begin)) {
        return false;
      }

      if (!emit1(JSOp::AddTupleElement)) {
        //          [stack] TUPLE
        return false;
      }
    }
  }

  if (!emit1(JSOp::FinishTuple)) {
    //              [stack] TUPLE
    return false;
  }

  return true;
}
#endif

static inline JSOp UnaryOpParseNodeKindToJSOp(ParseNodeKind pnk) {
  switch (pnk) {
    case ParseNodeKind::ThrowStmt:
      return JSOp::Throw;
    case ParseNodeKind::VoidExpr:
      return JSOp::Void;
    case ParseNodeKind::NotExpr:
      return JSOp::Not;
    case ParseNodeKind::BitNotExpr:
      return JSOp::BitNot;
    case ParseNodeKind::PosExpr:
      return JSOp::Pos;
    case ParseNodeKind::NegExpr:
      return JSOp::Neg;
    default:
      MOZ_CRASH("unexpected unary op");
  }
}

bool BytecodeEmitter::emitUnary(UnaryNode* unaryNode) {
  if (!updateSourceCoordNotes(unaryNode->pn_pos.begin)) {
    return false;
  }

  JSOp op = UnaryOpParseNodeKindToJSOp(unaryNode->getKind());
  ValueUsage valueUsage =
      op == JSOp::Void ? ValueUsage::IgnoreValue : ValueUsage::WantValue;
  if (!emitTree(unaryNode->kid(), valueUsage)) {
    return false;
  }
  return emit1(op);
}

bool BytecodeEmitter::emitTypeof(UnaryNode* typeofNode, JSOp op) {
  MOZ_ASSERT(op == JSOp::Typeof || op == JSOp::TypeofExpr);

  if (!updateSourceCoordNotes(typeofNode->pn_pos.begin)) {
    return false;
  }

  if (!emitTree(typeofNode->kid())) {
    return false;
  }

  return emit1(op);
}

bool BytecodeEmitter::emitFunctionFormalParameters(ParamsBodyNode* paramsBody) {
  FunctionBox* funbox = sc->asFunctionBox();

  bool hasRest = funbox->hasRest();

  FunctionParamsEmitter fpe(this, funbox);
  for (ParseNode* arg : paramsBody->parameters()) {
    ParseNode* bindingElement = arg;
    ParseNode* initializer = nullptr;
    if (arg->isKind(ParseNodeKind::AssignExpr)) {
      bindingElement = arg->as<BinaryNode>().left();
      initializer = arg->as<BinaryNode>().right();
    }
    bool hasInitializer = !!initializer;
    bool isRest =
        hasRest && arg->pn_next == *std::end(paramsBody->parameters());
    bool isDestructuring = !bindingElement->isKind(ParseNodeKind::Name);

    // Left-hand sides are either simple names or destructuring patterns.
    MOZ_ASSERT(bindingElement->isKind(ParseNodeKind::Name) ||
               bindingElement->isKind(ParseNodeKind::ArrayExpr) ||
               bindingElement->isKind(ParseNodeKind::ObjectExpr));

    auto emitDefaultInitializer = [this, &initializer, &bindingElement]() {
      //            [stack]

      if (!this->emitInitializer(initializer, bindingElement)) {
        //          [stack] DEFAULT
        return false;
      }
      return true;
    };

    auto emitDestructuring = [this, &bindingElement]() {
      //            [stack] ARG

      if (!this->emitDestructuringOps(&bindingElement->as<ListNode>(),
                                      DestructuringFlavor::Declaration)) {
        //          [stack] ARG
        return false;
      }

      return true;
    };

    if (isRest) {
      if (isDestructuring) {
        if (!fpe.prepareForDestructuringRest()) {
          //        [stack]
          return false;
        }
        if (!emitDestructuring()) {
          //        [stack]
          return false;
        }
        if (!fpe.emitDestructuringRestEnd()) {
          //        [stack]
          return false;
        }
      } else {
        auto paramName = bindingElement->as<NameNode>().name();
        if (!fpe.emitRest(paramName)) {
          //        [stack]
          return false;
        }
      }

      continue;
    }

    if (isDestructuring) {
      if (hasInitializer) {
        if (!fpe.prepareForDestructuringDefaultInitializer()) {
          //        [stack]
          return false;
        }
        if (!emitDefaultInitializer()) {
          //        [stack]
          return false;
        }
        if (!fpe.prepareForDestructuringDefault()) {
          //        [stack]
          return false;
        }
        if (!emitDestructuring()) {
          //        [stack]
          return false;
        }
        if (!fpe.emitDestructuringDefaultEnd()) {
          //        [stack]
          return false;
        }
      } else {
        if (!fpe.prepareForDestructuring()) {
          //        [stack]
          return false;
        }
        if (!emitDestructuring()) {
          //        [stack]
          return false;
        }
        if (!fpe.emitDestructuringEnd()) {
          //        [stack]
          return false;
        }
      }

      continue;
    }

    if (hasInitializer) {
      if (!fpe.prepareForDefault()) {
        //          [stack]
        return false;
      }
      if (!emitDefaultInitializer()) {
        //          [stack]
        return false;
      }
      auto paramName = bindingElement->as<NameNode>().name();
      if (!fpe.emitDefaultEnd(paramName)) {
        //          [stack]
        return false;
      }

      continue;
    }

    auto paramName = bindingElement->as<NameNode>().name();
    if (!fpe.emitSimple(paramName)) {
      //            [stack]
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitInitializeFunctionSpecialNames() {
  FunctionBox* funbox = sc->asFunctionBox();

  //                [stack]

  auto emitInitializeFunctionSpecialName =
      [](BytecodeEmitter* bce, TaggedParserAtomIndex name, JSOp op) {
        // A special name must be slotful, either on the frame or on the
        // call environment.
        MOZ_ASSERT(bce->lookupName(name).hasKnownSlot());

        NameOpEmitter noe(bce, name, NameOpEmitter::Kind::Initialize);
        if (!noe.prepareForRhs()) {
          //        [stack]
          return false;
        }
        if (!bce->emit1(op)) {
          //        [stack] THIS/ARGUMENTS/NEW.TARGET
          return false;
        }
        if (!noe.emitAssignment()) {
          //        [stack] THIS/ARGUMENTS/NEW.TARGET
          return false;
        }
        if (!bce->emit1(JSOp::Pop)) {
          //        [stack]
          return false;
        }

        return true;
      };

  // Do nothing if the function doesn't have an arguments binding.
  if (funbox->needsArgsObj()) {
    // Self-hosted code should use the more efficient ArgumentsLength and
    // GetArgument intrinsics instead of `arguments`.
    MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
    if (!emitInitializeFunctionSpecialName(
            this, TaggedParserAtomIndex::WellKnown::arguments(),
            JSOp::Arguments)) {
      //            [stack]
      return false;
    }
  }

  // Do nothing if the function doesn't have a this-binding (this
  // happens for instance if it doesn't use this/eval or if it's an
  // arrow function).
  if (funbox->functionHasThisBinding()) {
    if (!emitInitializeFunctionSpecialName(
            this, TaggedParserAtomIndex::WellKnown::dotThis(),
            JSOp::FunctionThis)) {
      return false;
    }
  }

  // Do nothing if the function doesn't have a new.target-binding (this happens
  // for instance if it doesn't use new.target/eval or if it's an arrow
  // function).
  if (funbox->functionHasNewTargetBinding()) {
    if (!emitInitializeFunctionSpecialName(
            this, TaggedParserAtomIndex::WellKnown::dotNewTarget(),
            JSOp::NewTarget)) {
      return false;
    }
  }

  // Do nothing if the function doesn't implicitly return a promise result.
  if (funbox->needsPromiseResult()) {
    if (!emitInitializeFunctionSpecialName(
            this, TaggedParserAtomIndex::WellKnown::dotGenerator(),
            JSOp::Generator)) {
      //            [stack]
      return false;
    }
  }
  return true;
}

bool BytecodeEmitter::emitLexicalInitialization(NameNode* name) {
  return emitLexicalInitialization(name->name());
}

bool BytecodeEmitter::emitLexicalInitialization(TaggedParserAtomIndex name) {
  NameOpEmitter noe(this, name, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    return false;
  }

  // The caller has pushed the RHS to the top of the stack. Assert that the
  // binding can be initialized without a binding object on the stack, and that
  // no BIND[G]NAME ops were emitted.
  MOZ_ASSERT(noe.loc().isLexical() || noe.loc().isSynthetic() ||
             noe.loc().isPrivateMethod());
  MOZ_ASSERT(!noe.emittedBindOp());

  if (!noe.emitAssignment()) {
    return false;
  }

  return true;
}

static MOZ_ALWAYS_INLINE ParseNode* FindConstructor(ListNode* classMethods) {
  for (ParseNode* classElement : classMethods->contents()) {
    ParseNode* unwrappedElement = classElement;
    if (unwrappedElement->is<LexicalScopeNode>()) {
      unwrappedElement = unwrappedElement->as<LexicalScopeNode>().scopeBody();
    }
    if (unwrappedElement->is<ClassMethod>()) {
      ClassMethod& method = unwrappedElement->as<ClassMethod>();
      ParseNode& methodName = method.name();
      if (!method.isStatic() &&
          (methodName.isKind(ParseNodeKind::ObjectPropertyName) ||
           methodName.isKind(ParseNodeKind::StringExpr)) &&
          methodName.as<NameNode>().atom() ==
              TaggedParserAtomIndex::WellKnown::constructor()) {
        return classElement;
      }
    }
  }
  return nullptr;
}

bool BytecodeEmitter::emitNewPrivateName(TaggedParserAtomIndex bindingName,
                                         TaggedParserAtomIndex symbolName) {
  if (!emitAtomOp(JSOp::NewPrivateName, symbolName)) {
    //              [stack] HERITAGE PRIVATENAME
    return false;
  }

  // Add a binding for #name => privatename
  if (!emitLexicalInitialization(bindingName)) {
    //              [stack] HERITAGE PRIVATENAME
    return false;
  }

  // Pop Private name off the stack.
  if (!emit1(JSOp::Pop)) {
    //              [stack] HERITAGE
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitNewPrivateNames(
    TaggedParserAtomIndex privateBrandName, ListNode* classMembers) {
  bool hasPrivateBrand = false;

  for (ParseNode* classElement : classMembers->contents()) {
    ParseNode* elementName;
    if (classElement->is<ClassMethod>()) {
      elementName = &classElement->as<ClassMethod>().name();
    } else if (classElement->is<ClassField>()) {
      elementName = &classElement->as<ClassField>().name();
    } else {
      continue;
    }

    if (!elementName->isKind(ParseNodeKind::PrivateName)) {
      continue;
    }

    // Non-static private methods' private names are optimized away.
    bool isOptimized = false;
    if (classElement->is<ClassMethod>() &&
        !classElement->as<ClassMethod>().isStatic()) {
      hasPrivateBrand = true;
      if (classElement->as<ClassMethod>().accessorType() ==
          AccessorType::None) {
        isOptimized = true;
      }
    }

    if (!isOptimized) {
      auto privateName = elementName->as<NameNode>().name();
      if (!emitNewPrivateName(privateName, privateName)) {
        return false;
      }
    }
  }

  if (hasPrivateBrand) {
    // We don't make a private name for every optimized method, but we need one
    // private name per class, the `.privateBrand`.
    if (!emitNewPrivateName(TaggedParserAtomIndex::WellKnown::dotPrivateBrand(),
                            privateBrandName)) {
      return false;
    }
  }
  return true;
}

// This follows ES6 14.5.14 (ClassDefinitionEvaluation) and ES6 14.5.15
// (BindingClassDeclarationEvaluation).
bool BytecodeEmitter::emitClass(
    ClassNode* classNode,
    ClassNameKind nameKind /* = ClassNameKind::BindingName */,
    TaggedParserAtomIndex
        nameForAnonymousClass /* = TaggedParserAtomIndex::null() */) {
  MOZ_ASSERT((nameKind == ClassNameKind::InferredName) ==
             bool(nameForAnonymousClass));

  ParseNode* heritageExpression = classNode->heritage();
  ListNode* classMembers = classNode->memberList();
  ParseNode* constructor = FindConstructor(classMembers);

  // If |nameKind != ClassNameKind::ComputedName|
  //                [stack]
  // Else
  //                [stack] NAME

  ClassEmitter ce(this);
  TaggedParserAtomIndex innerName;
  ClassEmitter::Kind kind = ClassEmitter::Kind::Expression;
  if (ClassNames* names = classNode->names()) {
    MOZ_ASSERT(nameKind == ClassNameKind::BindingName);
    innerName = names->innerBinding()->name();
    MOZ_ASSERT(innerName);

    if (names->outerBinding()) {
      MOZ_ASSERT(names->outerBinding()->name());
      MOZ_ASSERT(names->outerBinding()->name() == innerName);
      kind = ClassEmitter::Kind::Declaration;
    }
  }

  if (LexicalScopeNode* scopeBindings = classNode->scopeBindings()) {
    if (!ce.emitScope(scopeBindings->scopeBindings())) {
      //            [stack]
      return false;
    }
  }

  bool isDerived = !!heritageExpression;
  if (isDerived) {
    if (!updateSourceCoordNotes(classNode->pn_pos.begin)) {
      return false;
    }
    if (!markStepBreakpoint()) {
      return false;
    }
    if (!emitTree(heritageExpression)) {
      //            [stack] HERITAGE
      return false;
    }
  }

  // The class body scope holds any private names. Those mustn't be visible in
  // the heritage expression and hence the scope must be emitted after the
  // heritage expression.
  if (ClassBodyScopeNode* bodyScopeBindings = classNode->bodyScopeBindings()) {
    if (!ce.emitBodyScope(bodyScopeBindings->scopeBindings())) {
      //            [stack] HERITAGE
      return false;
    }

    // The spec does not say anything about private brands being symbols.  It's
    // an implementation detail. So we can give the special private brand
    // symbol any description we want and users won't normally see it. For
    // debugging, use the class name.
    auto privateBrandName = innerName;
    if (!innerName) {
      privateBrandName = nameForAnonymousClass
                             ? nameForAnonymousClass
                             : TaggedParserAtomIndex::WellKnown::anonymous();
    }
    if (!emitNewPrivateNames(privateBrandName, classMembers)) {
      return false;
    }
  }

  bool hasNameOnStack = nameKind == ClassNameKind::ComputedName;
  if (isDerived) {
    if (!ce.emitDerivedClass(innerName, nameForAnonymousClass,
                             hasNameOnStack)) {
      //            [stack] HERITAGE HOMEOBJ
      return false;
    }
  } else {
    if (!ce.emitClass(innerName, nameForAnonymousClass, hasNameOnStack)) {
      //            [stack] HOMEOBJ
      return false;
    }
  }

  // Stack currently has HOMEOBJ followed by optional HERITAGE. When HERITAGE
  // is not used, an implicit value of %FunctionPrototype% is implied.

  // See |Parser::classMember(...)| for the reason why |.initializers| is
  // created within its own scope.
  Maybe<LexicalScopeEmitter> lse;
  FunctionNode* ctor;
  if (constructor->is<LexicalScopeNode>()) {
    LexicalScopeNode* constructorScope = &constructor->as<LexicalScopeNode>();

    // The constructor scope should only contain the |.initializers| binding.
    MOZ_ASSERT(!constructorScope->isEmptyScope());
    MOZ_ASSERT(constructorScope->scopeBindings()->length == 1);
    MOZ_ASSERT(GetScopeDataTrailingNames(constructorScope->scopeBindings())[0]
                   .name() ==
               TaggedParserAtomIndex::WellKnown::dotInitializers());

    auto needsInitializer = [](ParseNode* propdef) {
      return NeedsFieldInitializer(propdef, false) ||
             NeedsAccessorInitializer(propdef, false);
    };

    // As an optimization omit the |.initializers| binding when no instance
    // fields or private methods are present.
    bool needsInitializers =
        std::any_of(classMembers->contents().begin(),
                    classMembers->contents().end(), needsInitializer);
    if (needsInitializers) {
      lse.emplace(this);
      if (!lse->emitScope(ScopeKind::Lexical,
                          constructorScope->scopeBindings())) {
        return false;
      }

      // Any class with field initializers will have a constructor
      if (!emitCreateMemberInitializers(ce, classMembers,
                                        FieldPlacement::Instance)) {
        return false;
      }
    }

    ctor = &constructorScope->scopeBody()->as<ClassMethod>().method();
  } else {
    // The |.initializers| binding is never emitted when in self-hosting mode.
    MOZ_ASSERT(emitterMode == BytecodeEmitter::SelfHosting);
    ctor = &constructor->as<ClassMethod>().method();
  }

  bool needsHomeObject = ctor->funbox()->needsHomeObject();
  // HERITAGE is consumed inside emitFunction.
  if (nameKind == ClassNameKind::InferredName) {
    if (!setFunName(ctor->funbox(), nameForAnonymousClass)) {
      return false;
    }
  }
  if (!emitFunction(ctor, isDerived)) {
    //              [stack] HOMEOBJ CTOR
    return false;
  }
  if (lse.isSome()) {
    if (!lse->emitEnd()) {
      return false;
    }
    lse.reset();
  }
  if (!ce.emitInitConstructor(needsHomeObject)) {
    //              [stack] CTOR HOMEOBJ
    return false;
  }

  if (!emitCreateFieldKeys(classMembers, FieldPlacement::Instance)) {
    return false;
  }

  if (!emitCreateMemberInitializers(ce, classMembers, FieldPlacement::Static)) {
    return false;
  }

  if (!emitCreateFieldKeys(classMembers, FieldPlacement::Static)) {
    return false;
  }

  if (!emitPropertyList(classMembers, ce, ClassBody)) {
    //              [stack] CTOR HOMEOBJ
    return false;
  }

  if (!ce.emitBinding()) {
    //              [stack] CTOR
    return false;
  }

  if (!emitInitializeStaticFields(classMembers)) {
    //              [stack] CTOR
    return false;
  }

  if (!ce.emitEnd(kind)) {
    //              [stack] # class declaration
    //              [stack]
    //              [stack] # class expression
    //              [stack] CTOR
    return false;
  }

  return true;
}

bool BytecodeEmitter::emitExportDefault(BinaryNode* exportNode) {
  MOZ_ASSERT(exportNode->isKind(ParseNodeKind::ExportDefaultStmt));

  ParseNode* valueNode = exportNode->left();
  if (valueNode->isDirectRHSAnonFunction()) {
    MOZ_ASSERT(exportNode->right());

    if (!emitAnonymousFunctionWithName(
            valueNode, TaggedParserAtomIndex::WellKnown::default_())) {
      return false;
    }
  } else {
    if (!emitTree(valueNode)) {
      return false;
    }
  }

  if (ParseNode* binding = exportNode->right()) {
    if (!emitLexicalInitialization(&binding->as<NameNode>())) {
      return false;
    }

    if (!emit1(JSOp::Pop)) {
      return false;
    }
  }

  return true;
}

bool BytecodeEmitter::emitTree(
    ParseNode* pn, ValueUsage valueUsage /* = ValueUsage::WantValue */,
    EmitLineNumberNote emitLineNote /* = EMIT_LINENOTE */) {
  AutoCheckRecursionLimit recursion(fc);
  if (!recursion.check(fc)) {
    return false;
  }

  /* Emit notes to tell the current bytecode's source line number.
     However, a couple trees require special treatment; see the
     relevant emitter functions for details. */
  if (emitLineNote == EMIT_LINENOTE &&
      !ParseNodeRequiresSpecialLineNumberNotes(pn)) {
    if (!updateLineNumberNotes(pn->pn_pos.begin)) {
      return false;
    }
  }

  switch (pn->getKind()) {
    case ParseNodeKind::Function:
      if (!emitFunction(&pn->as<FunctionNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ParamsBody:
      MOZ_ASSERT_UNREACHABLE(
          "ParamsBody should be handled in emitFunctionScript.");
      break;

    case ParseNodeKind::IfStmt:
      if (!emitIf(&pn->as<TernaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::SwitchStmt:
      if (!emitSwitch(&pn->as<SwitchStatement>())) {
        return false;
      }
      break;

    case ParseNodeKind::WhileStmt:
      if (!emitWhile(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::DoWhileStmt:
      if (!emitDo(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ForStmt:
      if (!emitFor(&pn->as<ForNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::BreakStmt:
      // Ensure that the column of the 'break' is set properly.
      if (!updateSourceCoordNotes(pn->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }

      if (!emitBreak(pn->as<BreakStatement>().label())) {
        return false;
      }
      break;

    case ParseNodeKind::ContinueStmt:
      // Ensure that the column of the 'continue' is set properly.
      if (!updateSourceCoordNotes(pn->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }

      if (!emitContinue(pn->as<ContinueStatement>().label())) {
        return false;
      }
      break;

    case ParseNodeKind::WithStmt:
      if (!emitWith(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::TryStmt:
      if (!emitTry(&pn->as<TryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::Catch:
      if (!emitCatch(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::VarStmt:
      if (!emitDeclarationList(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ReturnStmt:
      if (!emitReturn(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::YieldStarExpr:
      if (!emitYieldStar(pn->as<UnaryNode>().kid())) {
        return false;
      }
      break;

    case ParseNodeKind::Generator:
      if (!emit1(JSOp::Generator)) {
        return false;
      }
      break;

    case ParseNodeKind::InitialYield:
      if (!emitInitialYield(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::YieldExpr:
      if (!emitYield(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::AwaitExpr:
      if (!emitAwaitInInnermostScope(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::StatementList:
      if (!emitStatementList(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::EmptyStmt:
      break;

    case ParseNodeKind::ExpressionStmt:
      if (!emitExpressionStatement(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::LabelStmt:
      if (!emitLabeledStatement(&pn->as<LabeledStatement>())) {
        return false;
      }
      break;

    case ParseNodeKind::CommaExpr:
      if (!emitSequenceExpr(&pn->as<ListNode>(), valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::InitExpr:
    case ParseNodeKind::AssignExpr:
    case ParseNodeKind::AddAssignExpr:
    case ParseNodeKind::SubAssignExpr:
    case ParseNodeKind::BitOrAssignExpr:
    case ParseNodeKind::BitXorAssignExpr:
    case ParseNodeKind::BitAndAssignExpr:
    case ParseNodeKind::LshAssignExpr:
    case ParseNodeKind::RshAssignExpr:
    case ParseNodeKind::UrshAssignExpr:
    case ParseNodeKind::MulAssignExpr:
    case ParseNodeKind::DivAssignExpr:
    case ParseNodeKind::ModAssignExpr:
    case ParseNodeKind::PowAssignExpr: {
      BinaryNode* assignNode = &pn->as<BinaryNode>();
      if (!emitAssignmentOrInit(assignNode->getKind(), assignNode->left(),
                                assignNode->right())) {
        return false;
      }
      break;
    }

    case ParseNodeKind::CoalesceAssignExpr:
    case ParseNodeKind::OrAssignExpr:
    case ParseNodeKind::AndAssignExpr:
      if (!emitShortCircuitAssignment(&pn->as<AssignmentNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ConditionalExpr:
      if (!emitConditionalExpression(pn->as<ConditionalExpression>(),
                                     valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::OrExpr:
    case ParseNodeKind::CoalesceExpr:
    case ParseNodeKind::AndExpr:
      if (!emitShortCircuit(&pn->as<ListNode>(), valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::StrictEqExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::StrictNeExpr:
    case ParseNodeKind::NeExpr:
    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::InExpr:
    case ParseNodeKind::InstanceOfExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
    case ParseNodeKind::MulExpr:
    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
      if (!emitLeftAssociative(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::PrivateInExpr:
      if (!emitPrivateInExpr(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::PowExpr:
      if (!emitRightAssociative(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::TypeOfNameExpr:
      if (!emitTypeof(&pn->as<UnaryNode>(), JSOp::Typeof)) {
        return false;
      }
      break;

    case ParseNodeKind::TypeOfExpr:
      if (!emitTypeof(&pn->as<UnaryNode>(), JSOp::TypeofExpr)) {
        return false;
      }
      break;

    case ParseNodeKind::ThrowStmt:
      if (!updateSourceCoordNotes(pn->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }
      [[fallthrough]];
    case ParseNodeKind::VoidExpr:
    case ParseNodeKind::NotExpr:
    case ParseNodeKind::BitNotExpr:
    case ParseNodeKind::PosExpr:
    case ParseNodeKind::NegExpr:
      if (!emitUnary(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::PreIncrementExpr:
    case ParseNodeKind::PreDecrementExpr:
    case ParseNodeKind::PostIncrementExpr:
    case ParseNodeKind::PostDecrementExpr:
      if (!emitIncOrDec(&pn->as<UnaryNode>(), valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::DeleteNameExpr:
      if (!emitDeleteName(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::DeletePropExpr:
      if (!emitDeleteProperty(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::DeleteElemExpr:
      if (!emitDeleteElement(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::DeleteExpr:
      if (!emitDeleteExpression(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::DeleteOptionalChainExpr:
      if (!emitDeleteOptionalChain(&pn->as<UnaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::OptionalChain:
      if (!emitOptionalChain(&pn->as<UnaryNode>(), valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::DotExpr: {
      PropertyAccess* prop = &pn->as<PropertyAccess>();
      bool isSuper = prop->isSuper();
      PropOpEmitter poe(this, PropOpEmitter::Kind::Get,
                        isSuper ? PropOpEmitter::ObjKind::Super
                                : PropOpEmitter::ObjKind::Other);
      if (!poe.prepareForObj()) {
        return false;
      }
      if (isSuper) {
        UnaryNode* base = &prop->expression().as<UnaryNode>();
        if (!emitGetThisForSuperBase(base)) {
          //        [stack] THIS
          return false;
        }
      } else {
        if (!emitPropLHS(prop)) {
          //        [stack] OBJ
          return false;
        }
      }
      if (!poe.emitGet(prop->key().atom())) {
        //          [stack] PROP
        return false;
      }
      break;
    }

    case ParseNodeKind::ElemExpr: {
      PropertyByValue* elem = &pn->as<PropertyByValue>();
      bool isSuper = elem->isSuper();
      MOZ_ASSERT(!elem->key().isKind(ParseNodeKind::PrivateName));
      ElemOpEmitter eoe(this, ElemOpEmitter::Kind::Get,
                        isSuper ? ElemOpEmitter::ObjKind::Super
                                : ElemOpEmitter::ObjKind::Other);
      if (!emitElemObjAndKey(elem, isSuper, eoe)) {
        //          [stack] # if Super
        //          [stack] THIS KEY
        //          [stack] # otherwise
        //          [stack] OBJ KEY
        return false;
      }
      if (!eoe.emitGet()) {
        //          [stack] ELEM
        return false;
      }

      break;
    }

    case ParseNodeKind::PrivateMemberExpr: {
      PrivateMemberAccess* privateExpr = &pn->as<PrivateMemberAccess>();
      PrivateOpEmitter xoe(this, PrivateOpEmitter::Kind::Get,
                           privateExpr->privateName().name());
      if (!emitTree(&privateExpr->expression())) {
        //          [stack] OBJ
        return false;
      }
      if (!xoe.emitReference()) {
        //          [stack] OBJ NAME
        return false;
      }
      if (!xoe.emitGet()) {
        //          [stack] VALUE
        return false;
      }

      break;
    }

    case ParseNodeKind::NewExpr:
    case ParseNodeKind::TaggedTemplateExpr:
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::SuperCallExpr:
      if (!emitCallOrNew(&pn->as<CallNode>(), valueUsage)) {
        return false;
      }
      break;

    case ParseNodeKind::LexicalScope:
      if (!emitLexicalScope(&pn->as<LexicalScopeNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ConstDecl:
    case ParseNodeKind::LetDecl:
      if (!emitDeclarationList(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ImportDecl:
      MOZ_ASSERT(sc->isModuleContext());
      break;

    case ParseNodeKind::ExportStmt: {
      MOZ_ASSERT(sc->isModuleContext());
      UnaryNode* node = &pn->as<UnaryNode>();
      ParseNode* decl = node->kid();
      if (decl->getKind() != ParseNodeKind::ExportSpecList) {
        if (!emitTree(decl)) {
          return false;
        }
      }
      break;
    }

    case ParseNodeKind::ExportDefaultStmt:
      MOZ_ASSERT(sc->isModuleContext());
      if (!emitExportDefault(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ExportFromStmt:
      MOZ_ASSERT(sc->isModuleContext());
      break;

    case ParseNodeKind::CallSiteObj:
      if (!emitCallSiteObject(&pn->as<CallSiteNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ArrayExpr:
      if (!emitArrayLiteral(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ObjectExpr:
      if (!emitObject(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::Name:
      if (!emitGetName(&pn->as<NameNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::PrivateName:
      if (!emitGetPrivateName(&pn->as<NameNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::TemplateStringListExpr:
      if (!emitTemplateString(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::StringExpr:
      if (!emitStringOp(JSOp::String, pn->as<NameNode>().atom())) {
        return false;
      }
      break;

    case ParseNodeKind::NumberExpr:
      if (!emitNumberOp(pn->as<NumericLiteral>().value())) {
        return false;
      }
      break;

    case ParseNodeKind::BigIntExpr:
      if (!emitBigIntOp(&pn->as<BigIntLiteral>())) {
        return false;
      }
      break;

    case ParseNodeKind::RegExpExpr: {
      GCThingIndex index;
      if (!perScriptData().gcThingList().append(&pn->as<RegExpLiteral>(),
                                                &index)) {
        return false;
      }
      if (!emitRegExp(index)) {
        return false;
      }
      break;
    }

    case ParseNodeKind::TrueExpr:
      if (!emit1(JSOp::True)) {
        return false;
      }
      break;
    case ParseNodeKind::FalseExpr:
      if (!emit1(JSOp::False)) {
        return false;
      }
      break;
    case ParseNodeKind::NullExpr:
      if (!emit1(JSOp::Null)) {
        return false;
      }
      break;
    case ParseNodeKind::RawUndefinedExpr:
      if (!emit1(JSOp::Undefined)) {
        return false;
      }
      break;

    case ParseNodeKind::ThisExpr:
      if (!emitThisLiteral(&pn->as<ThisLiteral>())) {
        return false;
      }
      break;

    case ParseNodeKind::DebuggerStmt:
      if (!updateSourceCoordNotes(pn->pn_pos.begin)) {
        return false;
      }
      if (!markStepBreakpoint()) {
        return false;
      }
      if (!emit1(JSOp::Debugger)) {
        return false;
      }
      break;

    case ParseNodeKind::ClassDecl:
      if (!emitClass(&pn->as<ClassNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::NewTargetExpr:
      if (!emitNewTarget(&pn->as<NewTargetNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::ImportMetaExpr:
      if (!emit1(JSOp::ImportMeta)) {
        return false;
      }
      break;

    case ParseNodeKind::CallImportExpr: {
      BinaryNode* spec = &pn->as<BinaryNode>().right()->as<BinaryNode>();

      if (!emitTree(spec->left())) {
        //          [stack] specifier
        return false;
      }

      if (!spec->right()->isKind(ParseNodeKind::PosHolder)) {
        //          [stack] specifier options
        if (!emitTree(spec->right())) {
          return false;
        }
      } else {
        //          [stack] specifier undefined
        if (!emit1(JSOp::Undefined)) {
          return false;
        }
      }

      if (!emit1(JSOp::DynamicImport)) {
        return false;
      }

      break;
    }

    case ParseNodeKind::SetThis:
      if (!emitSetThis(&pn->as<BinaryNode>())) {
        return false;
      }
      break;

#ifdef ENABLE_RECORD_TUPLE
    case ParseNodeKind::RecordExpr:
      if (!emitRecordLiteral(&pn->as<ListNode>())) {
        return false;
      }
      break;

    case ParseNodeKind::TupleExpr:
      if (!emitTupleLiteral(&pn->as<ListNode>())) {
        return false;
      }
      break;
#endif

    case ParseNodeKind::PropertyNameExpr:
    case ParseNodeKind::PosHolder:
      MOZ_FALLTHROUGH_ASSERT(
          "Should never try to emit ParseNodeKind::PosHolder or ::Property");

    default:
      MOZ_ASSERT(0);
  }

  return true;
}

static bool AllocSrcNote(FrontendContext* fc, SrcNotesVector& notes,
                         unsigned size, unsigned* index) {
  size_t oldLength = notes.length();

  if (MOZ_UNLIKELY(oldLength + size > MaxSrcNotesLength)) {
    ReportAllocationOverflow(fc);
    return false;
  }

  if (!notes.growByUninitialized(size)) {
    return false;
  }

  *index = oldLength;
  return true;
}

bool BytecodeEmitter::addTryNote(TryNoteKind kind, uint32_t stackDepth,
                                 BytecodeOffset start, BytecodeOffset end) {
  MOZ_ASSERT(!inPrologue());
  return bytecodeSection().tryNoteList().append(kind, stackDepth, start, end);
}

bool BytecodeEmitter::newSrcNote(SrcNoteType type, unsigned* indexp) {
  // Non-gettable source notes such as column/lineno and debugger should not be
  // emitted for prologue / self-hosted.
  MOZ_ASSERT_IF(skipLocationSrcNotes() || skipBreakpointSrcNotes(),
                type <= SrcNoteType::LastGettable);

  SrcNotesVector& notes = bytecodeSection().notes();
  unsigned index;

  /*
   * Compute delta from the last annotated bytecode's offset.  If it's too
   * big to fit in sn, allocate one or more xdelta notes and reset sn.
   */
  BytecodeOffset offset = bytecodeSection().offset();
  ptrdiff_t delta = (offset - bytecodeSection().lastNoteOffset()).value();
  bytecodeSection().setLastNoteOffset(offset);

  auto allocator = [&](unsigned size) -> SrcNote* {
    if (!AllocSrcNote(fc, notes, size, &index)) {
      return nullptr;
    }
    return &notes[index];
  };

  if (!SrcNoteWriter::writeNote(type, delta, allocator)) {
    return false;
  }

  if (indexp) {
    *indexp = index;
  }
  return true;
}

bool BytecodeEmitter::newSrcNote2(SrcNoteType type, ptrdiff_t offset,
                                  unsigned* indexp) {
  unsigned index;
  if (!newSrcNote(type, &index)) {
    return false;
  }
  if (!newSrcNoteOperand(offset)) {
    return false;
  }
  if (indexp) {
    *indexp = index;
  }
  return true;
}

bool BytecodeEmitter::newSrcNoteOperand(ptrdiff_t operand) {
  if (!SrcNote::isRepresentableOperand(operand)) {
    reportError(nullptr, JSMSG_NEED_DIET, js_script_str);
    return false;
  }

  SrcNotesVector& notes = bytecodeSection().notes();

  auto allocator = [&](unsigned size) -> SrcNote* {
    unsigned index;
    if (!AllocSrcNote(fc, notes, size, &index)) {
      return nullptr;
    }
    return &notes[index];
  };

  return SrcNoteWriter::writeOperand(operand, allocator);
}

bool BytecodeEmitter::intoScriptStencil(ScriptIndex scriptIndex) {
  js::UniquePtr<ImmutableScriptData> immutableScriptData =
      createImmutableScriptData();
  if (!immutableScriptData) {
    return false;
  }

  MOZ_ASSERT(outermostScope().hasNonSyntacticScopeOnChain() ==
             sc->hasNonSyntacticScope());

  auto& things = perScriptData().gcThingList().objects();
  if (!compilationState.appendGCThings(fc, scriptIndex, things)) {
    return false;
  }

  // Hand over the ImmutableScriptData instance generated by BCE.
  auto* sharedData =
      SharedImmutableScriptData::createWith(fc, std::move(immutableScriptData));
  if (!sharedData) {
    return false;
  }

  // De-duplicate the bytecode within the runtime.
  if (!compilationState.sharedData.addAndShare(fc, scriptIndex, sharedData)) {
    return false;
  }

  ScriptStencil& script = compilationState.scriptData[scriptIndex];
  script.setHasSharedData();

  // Update flags specific to functions.
  if (sc->isFunctionBox()) {
    FunctionBox* funbox = sc->asFunctionBox();
    MOZ_ASSERT(&script == &funbox->functionStencil());
    funbox->copyUpdatedImmutableFlags();
    MOZ_ASSERT(script.isFunction());
  } else {
    ScriptStencilExtra& scriptExtra = compilationState.scriptExtra[scriptIndex];
    sc->copyScriptExtraFields(scriptExtra);
  }

  return true;
}

SelfHostedIter BytecodeEmitter::getSelfHostedIterFor(ParseNode* parseNode) {
  if (emitterMode == BytecodeEmitter::SelfHosting &&
      parseNode->isKind(ParseNodeKind::CallExpr) &&
      (parseNode->as<BinaryNode>().left()->isName(
           TaggedParserAtomIndex::WellKnown::allowContentIter()) ||
       parseNode->as<BinaryNode>().left()->isName(
           TaggedParserAtomIndex::WellKnown::allowContentIterWith()))) {
    return SelfHostedIter::Allow;
  }

  return SelfHostedIter::Deny;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void BytecodeEmitter::dumpAtom(TaggedParserAtomIndex index) const {
  parserAtoms().dump(index);
}
#endif
