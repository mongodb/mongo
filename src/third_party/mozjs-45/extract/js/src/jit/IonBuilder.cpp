/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonBuilder.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/SizePrintfMacros.h"

#include "builtin/Eval.h"
#include "builtin/TypedObject.h"
#include "frontend/SourceNotes.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineInspector.h"
#include "jit/Ion.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitSpewer.h"
#include "jit/Lowering.h"
#include "jit/MIRGraph.h"
#include "vm/ArgumentsObject.h"
#include "vm/Opcodes.h"
#include "vm/RegExpStatics.h"
#include "vm/TraceLogging.h"

#include "jsopcodeinlines.h"
#include "jsscriptinlines.h"

#include "jit/CompileInfo-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectGroup-inl.h"
#include "vm/ScopeObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::AssertedCast;
using mozilla::DebugOnly;
using mozilla::Maybe;

using JS::TrackedStrategy;
using JS::TrackedOutcome;
using JS::TrackedTypeSite;

class jit::BaselineFrameInspector
{
  public:
    TypeSet::Type thisType;
    JSObject* singletonScopeChain;

    Vector<TypeSet::Type, 4, JitAllocPolicy> argTypes;
    Vector<TypeSet::Type, 4, JitAllocPolicy> varTypes;

    explicit BaselineFrameInspector(TempAllocator* temp)
      : thisType(TypeSet::UndefinedType()),
        singletonScopeChain(nullptr),
        argTypes(*temp),
        varTypes(*temp)
    {}
};

BaselineFrameInspector*
jit::NewBaselineFrameInspector(TempAllocator* temp, BaselineFrame* frame, CompileInfo* info)
{
    MOZ_ASSERT(frame);

    BaselineFrameInspector* inspector = temp->lifoAlloc()->new_<BaselineFrameInspector>(temp);
    if (!inspector)
        return nullptr;

    // Note: copying the actual values into a temporary structure for use
    // during compilation could capture nursery pointers, so the values' types
    // are recorded instead.

    if (frame->isNonEvalFunctionFrame())
        inspector->thisType = TypeSet::GetMaybeUntrackedValueType(frame->thisArgument());

    if (frame->scopeChain()->isSingleton())
        inspector->singletonScopeChain = frame->scopeChain();

    JSScript* script = frame->script();

    if (script->functionNonDelazifying()) {
        if (!inspector->argTypes.reserve(frame->numFormalArgs()))
            return nullptr;
        for (size_t i = 0; i < frame->numFormalArgs(); i++) {
            if (script->formalIsAliased(i)) {
                inspector->argTypes.infallibleAppend(TypeSet::UndefinedType());
            } else if (!script->argsObjAliasesFormals()) {
                TypeSet::Type type =
                    TypeSet::GetMaybeUntrackedValueType(frame->unaliasedFormal(i));
                inspector->argTypes.infallibleAppend(type);
            } else if (frame->hasArgsObj()) {
                TypeSet::Type type =
                    TypeSet::GetMaybeUntrackedValueType(frame->argsObj().arg(i));
                inspector->argTypes.infallibleAppend(type);
            } else {
                inspector->argTypes.infallibleAppend(TypeSet::UndefinedType());
            }
        }
    }

    if (!inspector->varTypes.reserve(frame->script()->nfixed()))
        return nullptr;
    for (size_t i = 0; i < frame->script()->nfixed(); i++) {
        if (info->isSlotAliasedAtOsr(i + info->firstLocalSlot())) {
            inspector->varTypes.infallibleAppend(TypeSet::UndefinedType());
        } else {
            TypeSet::Type type = TypeSet::GetMaybeUntrackedValueType(frame->unaliasedLocal(i));
            inspector->varTypes.infallibleAppend(type);
        }
    }

    return inspector;
}

IonBuilder::IonBuilder(JSContext* analysisContext, CompileCompartment* comp,
                       const JitCompileOptions& options, TempAllocator* temp,
                       MIRGraph* graph, CompilerConstraintList* constraints,
                       BaselineInspector* inspector, CompileInfo* info,
                       const OptimizationInfo* optimizationInfo,
                       BaselineFrameInspector* baselineFrame, size_t inliningDepth,
                       uint32_t loopDepth)
  : MIRGenerator(comp, options, temp, graph, info, optimizationInfo),
    backgroundCodegen_(nullptr),
    actionableAbortScript_(nullptr),
    actionableAbortPc_(nullptr),
    actionableAbortMessage_(nullptr),
    analysisContext(analysisContext),
    baselineFrame_(baselineFrame),
    constraints_(constraints),
    analysis_(*temp, info->script()),
    thisTypes(nullptr),
    argTypes(nullptr),
    typeArray(nullptr),
    typeArrayHint(0),
    bytecodeTypeMap(nullptr),
    loopDepth_(loopDepth),
    trackedOptimizationSites_(*temp),
    lexicalCheck_(nullptr),
    callerResumePoint_(nullptr),
    callerBuilder_(nullptr),
    cfgStack_(*temp),
    loops_(*temp),
    switches_(*temp),
    labels_(*temp),
    iterators_(*temp),
    loopHeaders_(*temp),
    inspector(inspector),
    inliningDepth_(inliningDepth),
    inlinedBytecodeLength_(0),
    numLoopRestarts_(0),
    failedBoundsCheck_(info->script()->failedBoundsCheck()),
    failedShapeGuard_(info->script()->failedShapeGuard()),
    failedLexicalCheck_(info->script()->failedLexicalCheck()),
    nonStringIteration_(false),
    lazyArguments_(nullptr),
    inlineCallInfo_(nullptr),
    maybeFallbackFunctionGetter_(nullptr)
{
    script_ = info->script();
    scriptHasIonScript_ = script_->hasIonScript();
    pc = info->startPC();
    abortReason_ = AbortReason_Disable;

    MOZ_ASSERT(script()->hasBaselineScript() == (info->analysisMode() != Analysis_ArgumentsUsage));
    MOZ_ASSERT(!!analysisContext == (info->analysisMode() == Analysis_DefiniteProperties));

    if (!info->isAnalysis())
        script()->baselineScript()->setIonCompiledOrInlined();
}

void
IonBuilder::clearForBackEnd()
{
    MOZ_ASSERT(!analysisContext);
    baselineFrame_ = nullptr;

    // The caches below allocate data from the malloc heap. Release this before
    // later phases of compilation to avoid leaks, as the top level IonBuilder
    // is not explicitly destroyed. Note that builders for inner scripts are
    // constructed on the stack and will release this memory on destruction.
    gsn.purge();
    scopeCoordinateNameCache.purge();
}

bool
IonBuilder::abort(const char* message, ...)
{
    // Don't call PCToLineNumber in release builds.
#ifdef JS_JITSPEW
    va_list ap;
    va_start(ap, message);
    abortFmt(message, ap);
    va_end(ap);
# ifdef DEBUG
    JitSpew(JitSpew_IonAbort, "aborted @ %s:%d", script()->filename(), PCToLineNumber(script(), pc));
# else
    JitSpew(JitSpew_IonAbort, "aborted @ %s", script()->filename());
# endif
#endif
    trackActionableAbort(message);
    return false;
}

IonBuilder*
IonBuilder::outermostBuilder()
{
    IonBuilder* builder = this;
    while (builder->callerBuilder_)
        builder = builder->callerBuilder_;
    return builder;
}

void
IonBuilder::trackActionableAbort(const char* message)
{
    if (!isOptimizationTrackingEnabled())
        return;

    IonBuilder* topBuilder = outermostBuilder();
    if (topBuilder->hadActionableAbort())
        return;

    topBuilder->actionableAbortScript_ = script();
    topBuilder->actionableAbortPc_ = pc;
    topBuilder->actionableAbortMessage_ = message;
}

void
IonBuilder::spew(const char* message)
{
    // Don't call PCToLineNumber in release builds.
#ifdef DEBUG
    JitSpew(JitSpew_IonMIR, "%s @ %s:%d", message, script()->filename(), PCToLineNumber(script(), pc));
#endif
}

static inline int32_t
GetJumpOffset(jsbytecode* pc)
{
    MOZ_ASSERT(CodeSpec[JSOp(*pc)].type() == JOF_JUMP);
    return GET_JUMP_OFFSET(pc);
}

IonBuilder::CFGState
IonBuilder::CFGState::If(jsbytecode* join, MTest* test)
{
    CFGState state;
    state.state = IF_TRUE;
    state.stopAt = join;
    state.branch.ifFalse = test->ifFalse();
    state.branch.test = test;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::IfElse(jsbytecode* trueEnd, jsbytecode* falseEnd, MTest* test)
{
    MBasicBlock* ifFalse = test->ifFalse();

    CFGState state;
    // If the end of the false path is the same as the start of the
    // false path, then the "else" block is empty and we can devolve
    // this to the IF_TRUE case. We handle this here because there is
    // still an extra GOTO on the true path and we want stopAt to point
    // there, whereas the IF_TRUE case does not have the GOTO.
    state.state = (falseEnd == ifFalse->pc())
                  ? IF_TRUE_EMPTY_ELSE
                  : IF_ELSE_TRUE;
    state.stopAt = trueEnd;
    state.branch.falseEnd = falseEnd;
    state.branch.ifFalse = ifFalse;
    state.branch.test = test;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::AndOr(jsbytecode* join, MBasicBlock* lhs)
{
    CFGState state;
    state.state = AND_OR;
    state.stopAt = join;
    state.branch.ifFalse = lhs;
    state.branch.test = nullptr;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::TableSwitch(jsbytecode* exitpc, MTableSwitch* ins)
{
    CFGState state;
    state.state = TABLE_SWITCH;
    state.stopAt = exitpc;
    state.tableswitch.exitpc = exitpc;
    state.tableswitch.breaks = nullptr;
    state.tableswitch.ins = ins;
    state.tableswitch.currentBlock = 0;
    return state;
}

JSFunction*
IonBuilder::getSingleCallTarget(TemporaryTypeSet* calleeTypes)
{
    if (!calleeTypes)
        return nullptr;

    JSObject* obj = calleeTypes->maybeSingleton();
    if (!obj || !obj->is<JSFunction>())
        return nullptr;

    return &obj->as<JSFunction>();
}

bool
IonBuilder::getPolyCallTargets(TemporaryTypeSet* calleeTypes, bool constructing,
                               ObjectVector& targets, uint32_t maxTargets)
{
    MOZ_ASSERT(targets.empty());

    if (!calleeTypes)
        return true;

    if (calleeTypes->baseFlags() != 0)
        return true;

    unsigned objCount = calleeTypes->getObjectCount();

    if (objCount == 0 || objCount > maxTargets)
        return true;

    if (!targets.reserve(objCount))
        return false;
    for (unsigned i = 0; i < objCount; i++) {
        JSObject* obj = calleeTypes->getSingleton(i);
        if (obj) {
            MOZ_ASSERT(obj->isSingleton());
        } else {
            ObjectGroup* group = calleeTypes->getGroup(i);
            if (!group)
                continue;

            obj = group->maybeInterpretedFunction();
            if (!obj) {
                targets.clear();
                return true;
            }

            MOZ_ASSERT(!obj->isSingleton());
        }

        // Don't optimize if the callee is not callable or constructable per
        // the manner it is being invoked, so that CallKnown does not have to
        // handle these cases (they will always throw).
        if (constructing ? !obj->isConstructor() : !obj->isCallable()) {
            targets.clear();
            return true;
        }

        targets.infallibleAppend(obj);
    }

    return true;
}

IonBuilder::InliningDecision
IonBuilder::DontInline(JSScript* targetScript, const char* reason)
{
    if (targetScript) {
        JitSpew(JitSpew_Inlining, "Cannot inline %s:%" PRIuSIZE ": %s",
                targetScript->filename(), targetScript->lineno(), reason);
    } else {
        JitSpew(JitSpew_Inlining, "Cannot inline: %s", reason);
    }

    return InliningDecision_DontInline;
}

/*
 * |hasCommonInliningPath| determines whether the current inlining path has been
 * seen before based on the sequence of scripts in the chain of |IonBuilder|s.
 *
 * An inlining path for a function |f| is the sequence of functions whose
 * inlinings precede |f| up to any previous occurrences of |f|.
 * So, if we have the chain of inlinings
 *
 *          f1 -> f2 -> f -> f3 -> f4 -> f5 -> f
 *          --------         --------------
 *
 * the inlining paths for |f| are [f2, f1] and [f5, f4, f3].
 * When attempting to inline |f|, we find all existing inlining paths for |f|
 * and check whether they share a common prefix with the path created were |f|
 * inlined.
 *
 * For example, given mutually recursive functions |f| and |g|, a possible
 * inlining is
 *
 *                           +---- Inlining stopped here...
 *                           |
 *                           v
 *          a -> f -> g -> f \ -> g -> f -> g -> ...
 *
 * where the vertical bar denotes the termination of inlining.
 * Inlining is terminated because we have already observed the inlining path
 * [f] when inlining function |g|. Note that this will inline recursive
 * functions such as |fib| only one level, as |fib| has a zero length inlining
 * path which trivially prefixes all inlining paths.
 *
 */
bool
IonBuilder::hasCommonInliningPath(const JSScript* scriptToInline)
{
    // Find all previous inlinings of the |scriptToInline| and check for common
    // inlining paths with the top of the inlining stack.
    for (IonBuilder* it = this->callerBuilder_; it; it = it->callerBuilder_) {
        if (it->script() != scriptToInline)
            continue;

        // This only needs to check the top of each stack for a match,
        // as a match of length one ensures a common prefix.
        IonBuilder* path = it->callerBuilder_;
        if (!path || this->script() == path->script())
            return true;
    }

    return false;
}

IonBuilder::InliningDecision
IonBuilder::canInlineTarget(JSFunction* target, CallInfo& callInfo)
{
    if (!optimizationInfo().inlineInterpreted()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineGeneric);
        return InliningDecision_DontInline;
    }

    if (TraceLogTextIdEnabled(TraceLogger_InlinedScripts)) {
        return DontInline(nullptr, "Tracelogging of inlined scripts is enabled"
                                   "but Tracelogger cannot do that yet.");
    }

    if (!target->isInterpreted()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNotInterpreted);
        return DontInline(nullptr, "Non-interpreted target");
    }

    if (info().analysisMode() != Analysis_DefiniteProperties) {
        // If |this| or an argument has an empty resultTypeSet, don't bother
        // inlining, as the call is currently unreachable due to incomplete type
        // information. This does not apply to the definite properties analysis,
        // in that case we want to inline anyway.

        if (callInfo.thisArg()->emptyResultTypeSet()) {
            trackOptimizationOutcome(TrackedOutcome::CantInlineUnreachable);
            return DontInline(nullptr, "Empty TypeSet for |this|");
        }

        for (size_t i = 0; i < callInfo.argc(); i++) {
            if (callInfo.getArg(i)->emptyResultTypeSet()) {
                trackOptimizationOutcome(TrackedOutcome::CantInlineUnreachable);
                return DontInline(nullptr, "Empty TypeSet for argument");
            }
        }
    }

    // Allow constructing lazy scripts when performing the definite properties
    // analysis, as baseline has not been used to warm the caller up yet.
    if (target->isInterpreted() && info().analysisMode() == Analysis_DefiniteProperties) {
        RootedScript script(analysisContext, target->getOrCreateScript(analysisContext));
        if (!script)
            return InliningDecision_Error;

        if (!script->hasBaselineScript() && script->canBaselineCompile()) {
            MethodStatus status = BaselineCompile(analysisContext, script);
            if (status == Method_Error)
                return InliningDecision_Error;
            if (status != Method_Compiled) {
                trackOptimizationOutcome(TrackedOutcome::CantInlineNoBaseline);
                return InliningDecision_DontInline;
            }
        }
    }

    if (!target->hasScript()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineLazy);
        return DontInline(nullptr, "Lazy script");
    }

    JSScript* inlineScript = target->nonLazyScript();
    if (callInfo.constructing() && !target->isConstructor()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNotConstructor);
        return DontInline(inlineScript, "Callee is not a constructor");
    }

    if (!callInfo.constructing() && target->isClassConstructor()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineClassConstructor);
        return DontInline(inlineScript, "Not constructing class constructor");
    }

    AnalysisMode analysisMode = info().analysisMode();
    if (!CanIonCompile(inlineScript, analysisMode)) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineDisabledIon);
        return DontInline(inlineScript, "Disabled Ion compilation");
    }

    // Don't inline functions which don't have baseline scripts.
    if (!inlineScript->hasBaselineScript()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNoBaseline);
        return DontInline(inlineScript, "No baseline jitcode");
    }

    if (TooManyFormalArguments(target->nargs())) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineTooManyArgs);
        return DontInline(inlineScript, "Too many args");
    }

    // We check the number of actual arguments against the maximum number of
    // formal arguments as we do not want to encode all actual arguments in the
    // callerResumePoint.
    if (TooManyFormalArguments(callInfo.argc())) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineTooManyArgs);
        return DontInline(inlineScript, "Too many actual args");
    }

    if (hasCommonInliningPath(inlineScript)) {
        trackOptimizationOutcome(TrackedOutcome::HasCommonInliningPath);
        return DontInline(inlineScript, "Common inlining path");
    }

    if (inlineScript->uninlineable()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineGeneric);
        return DontInline(inlineScript, "Uninlineable script");
    }

    if (inlineScript->needsArgsObj()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNeedsArgsObj);
        return DontInline(inlineScript, "Script that needs an arguments object");
    }

    if (inlineScript->isDebuggee()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineDebuggee);
        return DontInline(inlineScript, "Script is debuggee");
    }

    TypeSet::ObjectKey* targetKey = TypeSet::ObjectKey::get(target);
    if (targetKey->unknownProperties()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineUnknownProps);
        return DontInline(inlineScript, "Target type has unknown properties");
    }

    return InliningDecision_Inline;
}

void
IonBuilder::popCfgStack()
{
    if (cfgStack_.back().isLoop())
        loops_.popBack();
    if (cfgStack_.back().state == CFGState::LABEL)
        labels_.popBack();
    cfgStack_.popBack();
}

bool
IonBuilder::analyzeNewLoopTypes(MBasicBlock* entry, jsbytecode* start, jsbytecode* end)
{
    // The phi inputs at the loop head only reflect types for variables that
    // were present at the start of the loop. If the variable changes to a new
    // type within the loop body, and that type is carried around to the loop
    // head, then we need to know about the new type up front.
    //
    // Since SSA information hasn't been constructed for the loop body yet, we
    // need a separate analysis to pick out the types that might flow around
    // the loop header. This is a best-effort analysis that may either over-
    // or under-approximate the set of such types.
    //
    // Over-approximating the types may lead to inefficient generated code, and
    // under-approximating the types will cause the loop body to be analyzed
    // multiple times as the correct types are deduced (see finishLoop).

    // If we restarted processing of an outer loop then get loop header types
    // directly from the last time we have previously processed this loop. This
    // both avoids repeated work from the bytecode traverse below, and will
    // also pick up types discovered while previously building the loop body.
    for (size_t i = 0; i < loopHeaders_.length(); i++) {
        if (loopHeaders_[i].pc == start) {
            MBasicBlock* oldEntry = loopHeaders_[i].header;

            // If this block has been discarded, its resume points will have
            // already discarded their operands.
            if (!oldEntry->isDead()) {
                MResumePoint* oldEntryRp = oldEntry->entryResumePoint();
                size_t stackDepth = oldEntryRp->stackDepth();
                for (size_t slot = 0; slot < stackDepth; slot++) {
                    MDefinition* oldDef = oldEntryRp->getOperand(slot);
                    if (!oldDef->isPhi()) {
                        MOZ_ASSERT(oldDef->block()->id() < oldEntry->id());
                        MOZ_ASSERT(oldDef == entry->getSlot(slot));
                        continue;
                    }
                    MPhi* oldPhi = oldDef->toPhi();
                    MPhi* newPhi = entry->getSlot(slot)->toPhi();
                    if (!newPhi->addBackedgeType(oldPhi->type(), oldPhi->resultTypeSet()))
                        return false;
                }
            }

            // Update the most recent header for this loop encountered, in case
            // new types flow to the phis and the loop is processed at least
            // three times.
            loopHeaders_[i].header = entry;
            return true;
        }
    }
    if (!loopHeaders_.append(LoopHeader(start, entry)))
        return false;

    jsbytecode* last = nullptr;
    jsbytecode* earlier = nullptr;
    for (jsbytecode* pc = start; pc != end; earlier = last, last = pc, pc += GetBytecodeLength(pc)) {
        uint32_t slot;
        if (*pc == JSOP_SETLOCAL)
            slot = info().localSlot(GET_LOCALNO(pc));
        else if (*pc == JSOP_SETARG)
            slot = info().argSlotUnchecked(GET_ARGNO(pc));
        else
            continue;
        if (slot >= info().firstStackSlot())
            continue;
        if (!analysis().maybeInfo(pc))
            continue;
        if (!last)
            continue;

        MPhi* phi = entry->getSlot(slot)->toPhi();

        if (*last == JSOP_POS)
            last = earlier;

        if (CodeSpec[*last].format & JOF_TYPESET) {
            TemporaryTypeSet* typeSet = bytecodeTypes(last);
            if (!typeSet->empty()) {
                MIRType type = typeSet->getKnownMIRType();
                if (!phi->addBackedgeType(type, typeSet))
                    return false;
            }
        } else if (*last == JSOP_GETLOCAL || *last == JSOP_GETARG) {
            uint32_t slot = (*last == JSOP_GETLOCAL)
                            ? info().localSlot(GET_LOCALNO(last))
                            : info().argSlotUnchecked(GET_ARGNO(last));
            if (slot < info().firstStackSlot()) {
                MPhi* otherPhi = entry->getSlot(slot)->toPhi();
                if (otherPhi->hasBackedgeType()) {
                    if (!phi->addBackedgeType(otherPhi->type(), otherPhi->resultTypeSet()))
                        return false;
                }
            }
        } else {
            MIRType type = MIRType_None;
            switch (*last) {
              case JSOP_VOID:
              case JSOP_UNDEFINED:
                type = MIRType_Undefined;
                break;
              case JSOP_GIMPLICITTHIS:
                if (!script()->hasNonSyntacticScope())
                    type = MIRType_Undefined;
                break;
              case JSOP_NULL:
                type = MIRType_Null;
                break;
              case JSOP_ZERO:
              case JSOP_ONE:
              case JSOP_INT8:
              case JSOP_INT32:
              case JSOP_UINT16:
              case JSOP_UINT24:
              case JSOP_BITAND:
              case JSOP_BITOR:
              case JSOP_BITXOR:
              case JSOP_BITNOT:
              case JSOP_RSH:
              case JSOP_LSH:
              case JSOP_URSH:
                type = MIRType_Int32;
                break;
              case JSOP_FALSE:
              case JSOP_TRUE:
              case JSOP_EQ:
              case JSOP_NE:
              case JSOP_LT:
              case JSOP_LE:
              case JSOP_GT:
              case JSOP_GE:
              case JSOP_NOT:
              case JSOP_STRICTEQ:
              case JSOP_STRICTNE:
              case JSOP_IN:
              case JSOP_INSTANCEOF:
                type = MIRType_Boolean;
                break;
              case JSOP_DOUBLE:
                type = MIRType_Double;
                break;
              case JSOP_STRING:
              case JSOP_TOSTRING:
              case JSOP_TYPEOF:
              case JSOP_TYPEOFEXPR:
                type = MIRType_String;
                break;
              case JSOP_SYMBOL:
                type = MIRType_Symbol;
                break;
              case JSOP_ADD:
              case JSOP_SUB:
              case JSOP_MUL:
              case JSOP_DIV:
              case JSOP_MOD:
              case JSOP_NEG:
                type = inspector->expectedResultType(last);
              default:
                break;
            }
            if (type != MIRType_None) {
                if (!phi->addBackedgeType(type, nullptr))
                    return false;
            }
        }
    }
    return true;
}

bool
IonBuilder::pushLoop(CFGState::State initial, jsbytecode* stopAt, MBasicBlock* entry, bool osr,
                     jsbytecode* loopHead, jsbytecode* initialPc,
                     jsbytecode* bodyStart, jsbytecode* bodyEnd,
                     jsbytecode* exitpc, jsbytecode* continuepc)
{
    ControlFlowInfo loop(cfgStack_.length(), continuepc);
    if (!loops_.append(loop))
        return false;

    CFGState state;
    state.state = initial;
    state.stopAt = stopAt;
    state.loop.bodyStart = bodyStart;
    state.loop.bodyEnd = bodyEnd;
    state.loop.exitpc = exitpc;
    state.loop.continuepc = continuepc;
    state.loop.entry = entry;
    state.loop.osr = osr;
    state.loop.successor = nullptr;
    state.loop.breaks = nullptr;
    state.loop.continues = nullptr;
    state.loop.initialState = initial;
    state.loop.initialPc = initialPc;
    state.loop.initialStopAt = stopAt;
    state.loop.loopHead = loopHead;
    return cfgStack_.append(state);
}

bool
IonBuilder::init()
{
    if (!TypeScript::FreezeTypeSets(constraints(), script(), &thisTypes, &argTypes, &typeArray))
        return false;

    if (inlineCallInfo_) {
        // If we're inlining, the actual this/argument types are not necessarily
        // a subset of the script's observed types. |argTypes| is never accessed
        // for inlined scripts, so we just null it.
        thisTypes = inlineCallInfo_->thisArg()->resultTypeSet();
        argTypes = nullptr;
    }

    if (!analysis().init(alloc(), gsn))
        return false;

    // The baseline script normally has the bytecode type map, but compute
    // it ourselves if we do not have a baseline script.
    if (script()->hasBaselineScript()) {
        bytecodeTypeMap = script()->baselineScript()->bytecodeTypeMap();
    } else {
        bytecodeTypeMap = alloc_->lifoAlloc()->newArrayUninitialized<uint32_t>(script()->nTypeSets());
        if (!bytecodeTypeMap)
            return false;
        FillBytecodeTypeMap(script(), bytecodeTypeMap);
    }

    return true;
}

bool
IonBuilder::build()
{
    if (!init())
        return false;

    if (script()->hasBaselineScript())
        script()->baselineScript()->resetMaxInliningDepth();

    if (!setCurrentAndSpecializePhis(newBlock(pc)))
        return false;
    if (!current)
        return false;

#ifdef JS_JITSPEW
    if (info().isAnalysis()) {
        JitSpew(JitSpew_IonScripts, "Analyzing script %s:%" PRIuSIZE " (%p) %s",
                script()->filename(), script()->lineno(), (void*)script(),
                AnalysisModeString(info().analysisMode()));
    } else {
        JitSpew(JitSpew_IonScripts, "%sompiling script %s:%" PRIuSIZE " (%p) (warmup-counter=%" PRIu32 ", level=%s)",
                (script()->hasIonScript() ? "Rec" : "C"),
                script()->filename(), script()->lineno(), (void*)script(),
                script()->getWarmUpCount(), OptimizationLevelString(optimizationInfo().level()));
    }
#endif

    initParameters();
    initLocals();

    // Initialize something for the scope chain. We can bail out before the
    // start instruction, but the snapshot is encoded *at* the start
    // instruction, which means generating any code that could load into
    // registers is illegal.
    MInstruction* scope = MConstant::New(alloc(), UndefinedValue());
    current->add(scope);
    current->initSlot(info().scopeChainSlot(), scope);

    // Initialize the return value.
    MInstruction* returnValue = MConstant::New(alloc(), UndefinedValue());
    current->add(returnValue);
    current->initSlot(info().returnValueSlot(), returnValue);

    // Initialize the arguments object slot to undefined if necessary.
    if (info().hasArguments()) {
        MInstruction* argsObj = MConstant::New(alloc(), UndefinedValue());
        current->add(argsObj);
        current->initSlot(info().argsObjSlot(), argsObj);
    }

    // Emit the start instruction, so we can begin real instructions.
    current->add(MStart::New(alloc(), MStart::StartType_Default));

    // Guard against over-recursion. Do this before we start unboxing, since
    // this will create an OSI point that will read the incoming argument
    // values, which is nice to do before their last real use, to minimize
    // register/stack pressure.
    MCheckOverRecursed* check = MCheckOverRecursed::New(alloc());
    current->add(check);
    MResumePoint* entryRpCopy = MResumePoint::Copy(alloc(), current->entryResumePoint());
    if (!entryRpCopy)
        return false;
    check->setResumePoint(entryRpCopy);

    // Parameters have been checked to correspond to the typeset, now we unbox
    // what we can in an infallible manner.
    rewriteParameters();

    // Check for redeclaration errors for global scripts.
    if (!info().funMaybeLazy() && !info().module() &&
        script()->bindings.numBodyLevelLocals() > 0)
    {
        MGlobalNameConflictsCheck* redeclCheck = MGlobalNameConflictsCheck::New(alloc());
        current->add(redeclCheck);
        MResumePoint* entryRpCopy = MResumePoint::Copy(alloc(), current->entryResumePoint());
        if (!entryRpCopy)
            return false;
        redeclCheck->setResumePoint(entryRpCopy);
    }

    // It's safe to start emitting actual IR, so now build the scope chain.
    if (!initScopeChain())
        return false;

    if (info().needsArgsObj() && !initArgumentsObject())
        return false;

    // The type analysis phase attempts to insert unbox operations near
    // definitions of values. It also attempts to replace uses in resume points
    // with the narrower, unboxed variants. However, we must prevent this
    // replacement from happening on values in the entry snapshot. Otherwise we
    // could get this:
    //
    //       v0 = MParameter(0)
    //       v1 = MParameter(1)
    //       --   ResumePoint(v2, v3)
    //       v2 = Unbox(v0, INT32)
    //       v3 = Unbox(v1, INT32)
    //
    // So we attach the initial resume point to each parameter, which the type
    // analysis explicitly checks (this is the same mechanism used for
    // effectful operations).
    for (uint32_t i = 0; i < info().endArgSlot(); i++) {
        MInstruction* ins = current->getEntrySlot(i)->toInstruction();
        if (ins->type() != MIRType_Value)
            continue;

        MResumePoint* entryRpCopy = MResumePoint::Copy(alloc(), current->entryResumePoint());
        if (!entryRpCopy)
            return false;
        ins->setResumePoint(entryRpCopy);
    }

    // lazyArguments should never be accessed in |argsObjAliasesFormals| scripts.
    if (info().hasArguments() && !info().argsObjAliasesFormals()) {
        lazyArguments_ = MConstant::New(alloc(), MagicValue(JS_OPTIMIZED_ARGUMENTS));
        current->add(lazyArguments_);
    }

    insertRecompileCheck();

    if (!traverseBytecode())
        return false;

    // Discard unreferenced & pre-allocated resume points.
    replaceMaybeFallbackFunctionGetter(nullptr);

    if (script_->hasBaselineScript() &&
        inlinedBytecodeLength_ > script_->baselineScript()->inlinedBytecodeLength())
    {
        script_->baselineScript()->setInlinedBytecodeLength(inlinedBytecodeLength_);
    }

    if (!maybeAddOsrTypeBarriers())
        return false;

    if (!processIterators())
        return false;

    if (!info().isAnalysis() && !abortedPreliminaryGroups().empty()) {
        abortReason_ = AbortReason_PreliminaryObjects;
        return false;
    }

    if (shouldForceAbort()) {
        abortReason_ = AbortReason_Disable;
        return false;
    }

    MOZ_ASSERT(loopDepth_ == 0);
    abortReason_ = AbortReason_NoAbort;
    return true;
}

bool
IonBuilder::processIterators()
{
    // Find phis that must directly hold an iterator live.
    Vector<MPhi*, 0, SystemAllocPolicy> worklist;
    for (size_t i = 0; i < iterators_.length(); i++) {
        MInstruction* ins = iterators_[i];
        for (MUseDefIterator iter(ins); iter; iter++) {
            if (iter.def()->isPhi()) {
                if (!worklist.append(iter.def()->toPhi()))
                    return false;
            }
        }
    }

    // Propagate the iterator and live status of phis to all other connected
    // phis.
    while (!worklist.empty()) {
        MPhi* phi = worklist.popCopy();
        phi->setIterator();
        phi->setImplicitlyUsedUnchecked();

        for (MUseDefIterator iter(phi); iter; iter++) {
            if (iter.def()->isPhi()) {
                MPhi* other = iter.def()->toPhi();
                if (!other->isIterator() && !worklist.append(other))
                    return false;
            }
        }
    }

    return true;
}

bool
IonBuilder::buildInline(IonBuilder* callerBuilder, MResumePoint* callerResumePoint,
                        CallInfo& callInfo)
{
    inlineCallInfo_ = &callInfo;

    if (!init())
        return false;

    JitSpew(JitSpew_IonScripts, "Inlining script %s:%" PRIuSIZE " (%p)",
            script()->filename(), script()->lineno(), (void*)script());

    callerBuilder_ = callerBuilder;
    callerResumePoint_ = callerResumePoint;

    if (callerBuilder->failedBoundsCheck_)
        failedBoundsCheck_ = true;

    if (callerBuilder->failedShapeGuard_)
        failedShapeGuard_ = true;

    if (callerBuilder->failedLexicalCheck_)
        failedLexicalCheck_ = true;

    safeForMinorGC_ = callerBuilder->safeForMinorGC_;

    // Generate single entrance block.
    if (!setCurrentAndSpecializePhis(newBlock(pc)))
        return false;
    if (!current)
        return false;

    current->setCallerResumePoint(callerResumePoint);

    // Connect the entrance block to the last block in the caller's graph.
    MBasicBlock* predecessor = callerBuilder->current;
    MOZ_ASSERT(predecessor == callerResumePoint->block());

    predecessor->end(MGoto::New(alloc(), current));
    if (!current->addPredecessorWithoutPhis(predecessor))
        return false;

    // Initialize scope chain slot to Undefined.  It's set later by |initScopeChain|.
    MInstruction* scope = MConstant::New(alloc(), UndefinedValue());
    current->add(scope);
    current->initSlot(info().scopeChainSlot(), scope);

    // Initialize |return value| slot.
    MInstruction* returnValue = MConstant::New(alloc(), UndefinedValue());
    current->add(returnValue);
    current->initSlot(info().returnValueSlot(), returnValue);

    // Initialize |arguments| slot.
    if (info().hasArguments()) {
        MInstruction* argsObj = MConstant::New(alloc(), UndefinedValue());
        current->add(argsObj);
        current->initSlot(info().argsObjSlot(), argsObj);
    }

    // Initialize |this| slot.
    current->initSlot(info().thisSlot(), callInfo.thisArg());

    JitSpew(JitSpew_Inlining, "Initializing %u arg slots", info().nargs());

    // NB: Ion does not inline functions which |needsArgsObj|.  So using argSlot()
    // instead of argSlotUnchecked() below is OK
    MOZ_ASSERT(!info().needsArgsObj());

    // Initialize actually set arguments.
    uint32_t existing_args = Min<uint32_t>(callInfo.argc(), info().nargs());
    for (size_t i = 0; i < existing_args; ++i) {
        MDefinition* arg = callInfo.getArg(i);
        current->initSlot(info().argSlot(i), arg);
    }

    // Pass Undefined for missing arguments
    for (size_t i = callInfo.argc(); i < info().nargs(); ++i) {
        MConstant* arg = MConstant::New(alloc(), UndefinedValue());
        current->add(arg);
        current->initSlot(info().argSlot(i), arg);
    }

    // Initialize the scope chain now that args are initialized.
    if (!initScopeChain(callInfo.fun()))
        return false;

    JitSpew(JitSpew_Inlining, "Initializing %u local slots; fixed lexicals begin at %u",
            info().nlocals(), info().fixedLexicalBegin());

    initLocals();

    JitSpew(JitSpew_Inlining, "Inline entry block MResumePoint %p, %u stack slots",
            (void*) current->entryResumePoint(), current->entryResumePoint()->stackDepth());

    // +2 for the scope chain and |this|, maybe another +1 for arguments object slot.
    MOZ_ASSERT(current->entryResumePoint()->stackDepth() == info().totalSlots());

    if (script_->argumentsHasVarBinding()) {
        lazyArguments_ = MConstant::New(alloc(), MagicValue(JS_OPTIMIZED_ARGUMENTS));
        current->add(lazyArguments_);
    }

    insertRecompileCheck();

    if (!traverseBytecode())
        return false;

    // Discard unreferenced & pre-allocated resume points.
    replaceMaybeFallbackFunctionGetter(nullptr);

    if (!info().isAnalysis() && !abortedPreliminaryGroups().empty()) {
        abortReason_ = AbortReason_PreliminaryObjects;
        return false;
    }

    if (shouldForceAbort()) {
        abortReason_ = AbortReason_Disable;
        return false;
    }

    return true;
}

void
IonBuilder::rewriteParameter(uint32_t slotIdx, MDefinition* param, int32_t argIndex)
{
    MOZ_ASSERT(param->isParameter() || param->isGetArgumentsObjectArg());

    TemporaryTypeSet* types = param->resultTypeSet();
    MDefinition* actual = ensureDefiniteType(param, types->getKnownMIRType());
    if (actual == param)
        return;

    // Careful! We leave the original MParameter in the entry resume point. The
    // arguments still need to be checked unless proven otherwise at the call
    // site, and these checks can bailout. We can end up:
    //   v0 = Parameter(0)
    //   v1 = Unbox(v0, INT32)
    //   --   ResumePoint(v0)
    //
    // As usual, it would be invalid for v1 to be captured in the initial
    // resume point, rather than v0.
    current->rewriteSlot(slotIdx, actual);
}

// Apply Type Inference information to parameters early on, unboxing them if
// they have a definitive type. The actual guards will be emitted by the code
// generator, explicitly, as part of the function prologue.
void
IonBuilder::rewriteParameters()
{
    MOZ_ASSERT(info().scopeChainSlot() == 0);

    if (!info().funMaybeLazy())
        return;

    for (uint32_t i = info().startArgSlot(); i < info().endArgSlot(); i++) {
        MDefinition* param = current->getSlot(i);
        rewriteParameter(i, param, param->toParameter()->index());
    }
}

void
IonBuilder::initParameters()
{
    if (!info().funMaybeLazy())
        return;

    // If we are doing OSR on a frame which initially executed in the
    // interpreter and didn't accumulate type information, try to use that OSR
    // frame to determine possible initial types for 'this' and parameters.

    if (thisTypes->empty() && baselineFrame_) {
        TypeSet::Type type = baselineFrame_->thisType;
        if (type.isSingletonUnchecked())
            checkNurseryObject(type.singleton());
        thisTypes->addType(type, alloc_->lifoAlloc());
    }

    MParameter* param = MParameter::New(alloc(), MParameter::THIS_SLOT, thisTypes);
    current->add(param);
    current->initSlot(info().thisSlot(), param);

    for (uint32_t i = 0; i < info().nargs(); i++) {
        TemporaryTypeSet* types = &argTypes[i];
        if (types->empty() && baselineFrame_ &&
            !script_->baselineScript()->modifiesArguments())
        {
            TypeSet::Type type = baselineFrame_->argTypes[i];
            if (type.isSingletonUnchecked())
                checkNurseryObject(type.singleton());
            types->addType(type, alloc_->lifoAlloc());
        }

        param = MParameter::New(alloc(), i, types);
        current->add(param);
        current->initSlot(info().argSlotUnchecked(i), param);
    }
}

void
IonBuilder::initLocals()
{
    if (info().nlocals() == 0)
        return;

    MConstant* undef = nullptr;
    if (info().fixedLexicalBegin() > 0) {
        undef = MConstant::New(alloc(), UndefinedValue());
        current->add(undef);
    }

    MConstant* uninitLexical = nullptr;
    if (info().fixedLexicalBegin() < info().nlocals()) {
        uninitLexical = MConstant::New(alloc(), MagicValue(JS_UNINITIALIZED_LEXICAL));
        current->add(uninitLexical);
    }

    for (uint32_t i = 0; i < info().nlocals(); i++) {
        current->initSlot(info().localSlot(i), (i < info().fixedLexicalBegin()
                                                ? undef
                                                : uninitLexical));
    }
}

bool
IonBuilder::initScopeChain(MDefinition* callee)
{
    MInstruction* scope = nullptr;

    // If the script doesn't use the scopechain, then it's already initialized
    // from earlier.  However, always make a scope chain when |needsArgsObj| is true
    // for the script, since arguments object construction requires the scope chain
    // to be passed in.
    if (!info().needsArgsObj() && !analysis().usesScopeChain())
        return true;

    // The scope chain is only tracked in scripts that have NAME opcodes which
    // will try to access the scope. For other scripts, the scope instructions
    // will be held live by resume points and code will still be generated for
    // them, so just use a constant undefined value.

    if (JSFunction* fun = info().funMaybeLazy()) {
        if (!callee) {
            MCallee* calleeIns = MCallee::New(alloc());
            current->add(calleeIns);
            callee = calleeIns;
        }
        scope = MFunctionEnvironment::New(alloc(), callee);
        current->add(scope);

        // This reproduce what is done in CallObject::createForFunction. Skip
        // this for analyses, as the script might not have a baseline script
        // with template objects yet.
        if (fun->needsCallObject() && !info().isAnalysis()) {
            if (fun->isNamedLambda()) {
                scope = createDeclEnvObject(callee, scope);
                if (!scope)
                    return false;
            }

            scope = createCallObject(callee, scope);
            if (!scope)
                return false;
        }
    } else if (ModuleObject* module = info().module()) {
        // Modules use a pre-created scope object.
        scope = constant(ObjectValue(module->initialEnvironment()));
    } else {
        // For global scripts without a non-syntactic global scope, the scope
        // chain is the global lexical scope.
        MOZ_ASSERT(!script()->isForEval());
        MOZ_ASSERT(!script()->hasNonSyntacticScope());
        scope = constant(ObjectValue(script()->global().lexicalScope()));
    }

    current->setScopeChain(scope);
    return true;
}

bool
IonBuilder::initArgumentsObject()
{
    JitSpew(JitSpew_IonMIR, "%s:%" PRIuSIZE " - Emitting code to initialize arguments object! block=%p",
                              script()->filename(), script()->lineno(), current);
    MOZ_ASSERT(info().needsArgsObj());
    MCreateArgumentsObject* argsObj = MCreateArgumentsObject::New(alloc(), current->scopeChain());
    current->add(argsObj);
    current->setArgumentsObject(argsObj);
    return true;
}

bool
IonBuilder::addOsrValueTypeBarrier(uint32_t slot, MInstruction** def_,
                                   MIRType type, TemporaryTypeSet* typeSet)
{
    MInstruction*& def = *def_;
    MBasicBlock* osrBlock = def->block();

    // Clear bogus type information added in newOsrPreheader().
    def->setResultType(MIRType_Value);
    def->setResultTypeSet(nullptr);

    if (typeSet && !typeSet->unknown()) {
        MInstruction* barrier = MTypeBarrier::New(alloc(), def, typeSet);
        osrBlock->insertBefore(osrBlock->lastIns(), barrier);
        osrBlock->rewriteSlot(slot, barrier);
        def = barrier;
    } else if (type == MIRType_Null ||
               type == MIRType_Undefined ||
               type == MIRType_MagicOptimizedArguments)
    {
        // No unbox instruction will be added below, so check the type by
        // adding a type barrier for a singleton type set.
        TypeSet::Type ntype = TypeSet::PrimitiveType(ValueTypeFromMIRType(type));
        LifoAlloc* lifoAlloc = alloc().lifoAlloc();
        typeSet = lifoAlloc->new_<TemporaryTypeSet>(lifoAlloc, ntype);
        if (!typeSet)
            return false;
        MInstruction* barrier = MTypeBarrier::New(alloc(), def, typeSet);
        osrBlock->insertBefore(osrBlock->lastIns(), barrier);
        osrBlock->rewriteSlot(slot, barrier);
        def = barrier;
    }

    switch (type) {
      case MIRType_Boolean:
      case MIRType_Int32:
      case MIRType_Double:
      case MIRType_String:
      case MIRType_Symbol:
      case MIRType_Object:
        if (type != def->type()) {
            MUnbox* unbox = MUnbox::New(alloc(), def, type, MUnbox::Fallible);
            osrBlock->insertBefore(osrBlock->lastIns(), unbox);
            osrBlock->rewriteSlot(slot, unbox);
            def = unbox;
        }
        break;

      case MIRType_Null:
      {
        MConstant* c = MConstant::New(alloc(), NullValue());
        osrBlock->insertBefore(osrBlock->lastIns(), c);
        osrBlock->rewriteSlot(slot, c);
        def = c;
        break;
      }

      case MIRType_Undefined:
      {
        MConstant* c = MConstant::New(alloc(), UndefinedValue());
        osrBlock->insertBefore(osrBlock->lastIns(), c);
        osrBlock->rewriteSlot(slot, c);
        def = c;
        break;
      }

      case MIRType_MagicOptimizedArguments:
        MOZ_ASSERT(lazyArguments_);
        osrBlock->rewriteSlot(slot, lazyArguments_);
        def = lazyArguments_;
        break;

      default:
        break;
    }

    MOZ_ASSERT(def == osrBlock->getSlot(slot));
    return true;
}

bool
IonBuilder::maybeAddOsrTypeBarriers()
{
    if (!info().osrPc())
        return true;

    // The loop has successfully been processed, and the loop header phis
    // have their final type. Add unboxes and type barriers in the OSR
    // block to check that the values have the appropriate type, and update
    // the types in the preheader.

    MBasicBlock* osrBlock = graph().osrBlock();
    if (!osrBlock) {
        // Because IonBuilder does not compile catch blocks, it's possible to
        // end up without an OSR block if the OSR pc is only reachable via a
        // break-statement inside the catch block. For instance:
        //
        //   for (;;) {
        //       try {
        //           throw 3;
        //       } catch(e) {
        //           break;
        //       }
        //   }
        //   while (..) { } // <= OSR here, only reachable via catch block.
        //
        // For now we just abort in this case.
        MOZ_ASSERT(graph().hasTryBlock());
        return abort("OSR block only reachable through catch block");
    }

    MBasicBlock* preheader = osrBlock->getSuccessor(0);
    MBasicBlock* header = preheader->getSuccessor(0);
    static const size_t OSR_PHI_POSITION = 1;
    MOZ_ASSERT(preheader->getPredecessor(OSR_PHI_POSITION) == osrBlock);

    MResumePoint* headerRp = header->entryResumePoint();
    size_t stackDepth = headerRp->stackDepth();
    MOZ_ASSERT(stackDepth == osrBlock->stackDepth());
    for (uint32_t slot = info().startArgSlot(); slot < stackDepth; slot++) {
        // Aliased slots are never accessed, since they need to go through
        // the callobject. The typebarriers are added there and can be
        // discarded here.
        if (info().isSlotAliasedAtOsr(slot))
            continue;

        MInstruction* def = osrBlock->getSlot(slot)->toInstruction();
        MPhi* preheaderPhi = preheader->getSlot(slot)->toPhi();
        MPhi* headerPhi = headerRp->getOperand(slot)->toPhi();

        MIRType type = headerPhi->type();
        TemporaryTypeSet* typeSet = headerPhi->resultTypeSet();

        if (!addOsrValueTypeBarrier(slot, &def, type, typeSet))
            return false;

        preheaderPhi->replaceOperand(OSR_PHI_POSITION, def);
        preheaderPhi->setResultType(type);
        preheaderPhi->setResultTypeSet(typeSet);
    }

    return true;
}

// We try to build a control-flow graph in the order that it would be built as
// if traversing the AST. This leads to a nice ordering and lets us build SSA
// in one pass, since the bytecode is structured.
//
// We traverse the bytecode iteratively, maintaining a current basic block.
// Each basic block has a mapping of local slots to instructions, as well as a
// stack depth. As we encounter instructions we mutate this mapping in the
// current block.
//
// Things get interesting when we encounter a control structure. This can be
// either an IFEQ, downward GOTO, or a decompiler hint stashed away in source
// notes. Once we encounter such an opcode, we recover the structure of the
// control flow (its branches and bounds), and push it on a stack.
//
// As we continue traversing the bytecode, we look for points that would
// terminate the topmost control flow path pushed on the stack. These are:
//  (1) The bounds of the current structure (end of a loop or join/edge of a
//      branch).
//  (2) A "return", "break", or "continue" statement.
//
// For (1), we expect that there is a current block in the progress of being
// built, and we complete the necessary edges in the CFG. For (2), we expect
// that there is no active block.
//
// For normal diamond join points, we construct Phi nodes as we add
// predecessors. For loops, care must be taken to propagate Phi nodes back
// through uses in the loop body.
bool
IonBuilder::traverseBytecode()
{
    for (;;) {
        MOZ_ASSERT(pc < info().limitPC());

        for (;;) {
            if (!alloc().ensureBallast())
                return false;

            // Check if we've hit an expected join point or edge in the bytecode.
            // Leaving one control structure could place us at the edge of another,
            // thus |while| instead of |if| so we don't skip any opcodes.
            if (!cfgStack_.empty() && cfgStack_.back().stopAt == pc) {
                ControlStatus status = processCfgStack();
                if (status == ControlStatus_Error)
                    return false;
                if (status == ControlStatus_Abort)
                    return abort("Aborted while processing control flow");
                if (!current)
                    return true;
                continue;
            }

            // Some opcodes need to be handled early because they affect control
            // flow, terminating the current basic block and/or instructing the
            // traversal algorithm to continue from a new pc.
            //
            //   (1) If the opcode does not affect control flow, then the opcode
            //       is inspected and transformed to IR. This is the process_opcode
            //       label.
            //   (2) A loop could be detected via a forward GOTO. In this case,
            //       we don't want to process the GOTO, but the following
            //       instruction.
            //   (3) A RETURN, STOP, BREAK, or CONTINUE may require processing the
            //       CFG stack to terminate open branches.
            //
            // Similar to above, snooping control flow could land us at another
            // control flow point, so we iterate until it's time to inspect a real
            // opcode.
            ControlStatus status;
            if ((status = snoopControlFlow(JSOp(*pc))) == ControlStatus_None)
                break;
            if (status == ControlStatus_Error)
                return false;
            if (status == ControlStatus_Abort)
                return abort("Aborted while processing control flow");
            if (!current)
                return true;
        }

#ifdef DEBUG
        // In debug builds, after compiling this op, check that all values
        // popped by this opcode either:
        //
        //   (1) Have the ImplicitlyUsed flag set on them.
        //   (2) Have more uses than before compiling this op (the value is
        //       used as operand of a new MIR instruction).
        //
        // This is used to catch problems where IonBuilder pops a value without
        // adding any SSA uses and doesn't call setImplicitlyUsedUnchecked on it.
        Vector<MDefinition*, 4, JitAllocPolicy> popped(alloc());
        Vector<size_t, 4, JitAllocPolicy> poppedUses(alloc());
        unsigned nuses = GetUseCount(script_, script_->pcToOffset(pc));

        for (unsigned i = 0; i < nuses; i++) {
            MDefinition* def = current->peek(-int32_t(i + 1));
            if (!popped.append(def) || !poppedUses.append(def->defUseCount()))
                return false;
        }
#endif

        // Nothing in inspectOpcode() is allowed to advance the pc.
        JSOp op = JSOp(*pc);
        if (!inspectOpcode(op))
            return false;

#ifdef DEBUG
        for (size_t i = 0; i < popped.length(); i++) {
            switch (op) {
              case JSOP_POP:
              case JSOP_POPN:
              case JSOP_DUPAT:
              case JSOP_DUP:
              case JSOP_DUP2:
              case JSOP_PICK:
              case JSOP_SWAP:
              case JSOP_SETARG:
              case JSOP_SETLOCAL:
              case JSOP_INITLEXICAL:
              case JSOP_SETRVAL:
              case JSOP_VOID:
                // Don't require SSA uses for values popped by these ops.
                break;

              case JSOP_POS:
              case JSOP_TOID:
              case JSOP_TOSTRING:
                // These ops may leave their input on the stack without setting
                // the ImplicitlyUsed flag. If this value will be popped immediately,
                // we may replace it with |undefined|, but the difference is
                // not observable.
                MOZ_ASSERT(i == 0);
                if (current->peek(-1) == popped[0])
                    break;
                // FALL THROUGH

              default:
                MOZ_ASSERT(popped[i]->isImplicitlyUsed() ||

                           // MNewDerivedTypedObject instances are
                           // often dead unless they escape from the
                           // fn. See IonBuilder::loadTypedObjectData()
                           // for more details.
                           popped[i]->isNewDerivedTypedObject() ||

                           popped[i]->defUseCount() > poppedUses[i]);
                break;
            }
        }
#endif

        pc += CodeSpec[op].length;
        current->updateTrackedSite(bytecodeSite(pc));
    }

    return true;
}

IonBuilder::ControlStatus
IonBuilder::snoopControlFlow(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return maybeLoop(op, info().getNote(gsn, pc));

      case JSOP_POP:
        return maybeLoop(op, info().getNote(gsn, pc));

      case JSOP_RETURN:
      case JSOP_RETRVAL:
        return processReturn(op);

      case JSOP_THROW:
        return processThrow();

      case JSOP_GOTO:
      {
        jssrcnote* sn = info().getNote(gsn, pc);
        switch (sn ? SN_TYPE(sn) : SRC_NULL) {
          case SRC_BREAK:
          case SRC_BREAK2LABEL:
            return processBreak(op, sn);

          case SRC_CONTINUE:
            return processContinue(op);

          case SRC_SWITCHBREAK:
            return processSwitchBreak(op);

          case SRC_WHILE:
          case SRC_FOR_IN:
          case SRC_FOR_OF:
            // while (cond) { }
            return whileOrForInLoop(sn);

          default:
            // Hard assert for now - make an error later.
            MOZ_CRASH("unknown goto case");
        }
        break;
      }

      case JSOP_TABLESWITCH:
        return tableSwitch(op, info().getNote(gsn, pc));

      case JSOP_IFNE:
        // We should never reach an IFNE, it's a stopAt point, which will
        // trigger closing the loop.
        MOZ_CRASH("we should never reach an ifne!");

      default:
        break;
    }
    return ControlStatus_None;
}

bool
IonBuilder::inspectOpcode(JSOp op)
{
    MOZ_ASSERT(analysis_.maybeInfo(pc), "Compiling unreachable op");

    switch (op) {
      case JSOP_NOP:
      case JSOP_LINENO:
      case JSOP_LOOPENTRY:
        return true;

      case JSOP_LABEL:
        return jsop_label();

      case JSOP_UNDEFINED:
        // If this ever changes, change what JSOP_GIMPLICITTHIS does too.
        return pushConstant(UndefinedValue());

      case JSOP_IFEQ:
        return jsop_ifeq(JSOP_IFEQ);

      case JSOP_TRY:
        return jsop_try();

      case JSOP_CONDSWITCH:
        return jsop_condswitch();

      case JSOP_BITNOT:
        return jsop_bitnot();

      case JSOP_BITAND:
      case JSOP_BITOR:
      case JSOP_BITXOR:
      case JSOP_LSH:
      case JSOP_RSH:
      case JSOP_URSH:
        return jsop_bitop(op);

      case JSOP_ADD:
      case JSOP_SUB:
      case JSOP_MUL:
      case JSOP_DIV:
      case JSOP_MOD:
        return jsop_binary_arith(op);

      case JSOP_POW:
        return jsop_pow();

      case JSOP_POS:
        return jsop_pos();

      case JSOP_NEG:
        return jsop_neg();

      case JSOP_TOSTRING:
        return jsop_tostring();

      case JSOP_AND:
      case JSOP_OR:
        return jsop_andor(op);

      case JSOP_DEFVAR:
        return jsop_defvar(GET_UINT32_INDEX(pc));

      case JSOP_DEFLET:
      case JSOP_DEFCONST:
        return jsop_deflexical(GET_UINT32_INDEX(pc));

      case JSOP_DEFFUN:
        return jsop_deffun(GET_UINT32_INDEX(pc));

      case JSOP_EQ:
      case JSOP_NE:
      case JSOP_STRICTEQ:
      case JSOP_STRICTNE:
      case JSOP_LT:
      case JSOP_LE:
      case JSOP_GT:
      case JSOP_GE:
        return jsop_compare(op);

      case JSOP_DOUBLE:
        return pushConstant(info().getConst(pc));

      case JSOP_STRING:
        return pushConstant(StringValue(info().getAtom(pc)));

      case JSOP_SYMBOL: {
        unsigned which = GET_UINT8(pc);
        JS::Symbol* sym = compartment->runtime()->wellKnownSymbols().get(which);
        return pushConstant(SymbolValue(sym));
      }

      case JSOP_ZERO:
        return pushConstant(Int32Value(0));

      case JSOP_ONE:
        return pushConstant(Int32Value(1));

      case JSOP_NULL:
        return pushConstant(NullValue());

      case JSOP_VOID:
        current->pop();
        return pushConstant(UndefinedValue());

      case JSOP_HOLE:
        return pushConstant(MagicValue(JS_ELEMENTS_HOLE));

      case JSOP_FALSE:
        return pushConstant(BooleanValue(false));

      case JSOP_TRUE:
        return pushConstant(BooleanValue(true));

      case JSOP_ARGUMENTS:
        return jsop_arguments();

      case JSOP_RUNONCE:
        return jsop_runonce();

      case JSOP_REST:
        return jsop_rest();

      case JSOP_GETARG:
        if (info().argsObjAliasesFormals()) {
            MGetArgumentsObjectArg* getArg = MGetArgumentsObjectArg::New(alloc(),
                                                                         current->argumentsObject(),
                                                                         GET_ARGNO(pc));
            current->add(getArg);
            current->push(getArg);
        } else {
            current->pushArg(GET_ARGNO(pc));
        }
        return true;

      case JSOP_SETARG:
        return jsop_setarg(GET_ARGNO(pc));

      case JSOP_GETLOCAL:
        current->pushLocal(GET_LOCALNO(pc));
        return true;

      case JSOP_SETLOCAL:
        current->setLocal(GET_LOCALNO(pc));
        return true;

      case JSOP_THROWSETCONST:
      case JSOP_THROWSETALIASEDCONST:
        return jsop_throwsetconst();

      case JSOP_CHECKLEXICAL:
        return jsop_checklexical();

      case JSOP_INITLEXICAL:
        current->setLocal(GET_LOCALNO(pc));
        return true;

      case JSOP_INITGLEXICAL: {
        MOZ_ASSERT(!script()->hasNonSyntacticScope());
        MDefinition* value = current->pop();
        current->push(constant(ObjectValue(script()->global().lexicalScope())));
        current->push(value);
        return jsop_setprop(info().getAtom(pc)->asPropertyName());
      }

      case JSOP_CHECKALIASEDLEXICAL:
        return jsop_checkaliasedlet(ScopeCoordinate(pc));

      case JSOP_INITALIASEDLEXICAL:
        return jsop_setaliasedvar(ScopeCoordinate(pc));

      case JSOP_UNINITIALIZED:
        return pushConstant(MagicValue(JS_UNINITIALIZED_LEXICAL));

      case JSOP_POP:
        current->pop();

        // POP opcodes frequently appear where values are killed, e.g. after
        // SET* opcodes. Place a resume point afterwards to avoid capturing
        // the dead value in later snapshots, except in places where that
        // resume point is obviously unnecessary.
        if (pc[JSOP_POP_LENGTH] == JSOP_POP)
            return true;
        return maybeInsertResume();

      case JSOP_POPN:
        for (uint32_t i = 0, n = GET_UINT16(pc); i < n; i++)
            current->pop();
        return true;

      case JSOP_DUPAT:
        current->pushSlot(current->stackDepth() - 1 - GET_UINT24(pc));
        return true;

      case JSOP_NEWINIT:
        if (GET_UINT8(pc) == JSProto_Array)
            return jsop_newarray(0);
        return jsop_newobject();

      case JSOP_NEWARRAY:
        return jsop_newarray(GET_UINT32(pc));

      case JSOP_NEWARRAY_COPYONWRITE:
        return jsop_newarray_copyonwrite();

      case JSOP_NEWOBJECT:
        return jsop_newobject();

      case JSOP_INITELEM:
      case JSOP_INITHIDDENELEM:
        return jsop_initelem();

      case JSOP_INITELEM_ARRAY:
        return jsop_initelem_array();

      case JSOP_INITPROP:
      case JSOP_INITLOCKEDPROP:
      case JSOP_INITHIDDENPROP:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_initprop(name);
      }

      case JSOP_MUTATEPROTO:
      {
        return jsop_mutateproto();
      }

      case JSOP_INITPROP_GETTER:
      case JSOP_INITHIDDENPROP_GETTER:
      case JSOP_INITPROP_SETTER:
      case JSOP_INITHIDDENPROP_SETTER: {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_initprop_getter_setter(name);
      }

      case JSOP_INITELEM_GETTER:
      case JSOP_INITHIDDENELEM_GETTER:
      case JSOP_INITELEM_SETTER:
      case JSOP_INITHIDDENELEM_SETTER:
        return jsop_initelem_getter_setter();

      case JSOP_FUNCALL:
        return jsop_funcall(GET_ARGC(pc));

      case JSOP_FUNAPPLY:
        return jsop_funapply(GET_ARGC(pc));

      case JSOP_CALL:
      case JSOP_CALLITER:
      case JSOP_NEW:
      case JSOP_SUPERCALL:
        return jsop_call(GET_ARGC(pc), (JSOp)*pc == JSOP_NEW || (JSOp)*pc == JSOP_SUPERCALL);

      case JSOP_EVAL:
      case JSOP_STRICTEVAL:
        return jsop_eval(GET_ARGC(pc));

      case JSOP_INT8:
        return pushConstant(Int32Value(GET_INT8(pc)));

      case JSOP_UINT16:
        return pushConstant(Int32Value(GET_UINT16(pc)));

      case JSOP_GETGNAME:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        if (!script()->hasNonSyntacticScope())
            return jsop_getgname(name);
        return jsop_getname(name);
      }

      case JSOP_SETGNAME:
      case JSOP_STRICTSETGNAME:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        JSObject* obj = nullptr;
        if (!script()->hasNonSyntacticScope())
            obj = testGlobalLexicalBinding(name);
        if (obj)
            return setStaticName(obj, name);
        return jsop_setprop(name);
      }

      case JSOP_GETNAME:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_getname(name);
      }

      case JSOP_GETINTRINSIC:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_intrinsic(name);
      }

      case JSOP_GETIMPORT:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_getimport(name);
      }

      case JSOP_BINDGNAME:
        if (!script()->hasNonSyntacticScope()) {
            if (JSObject* scope = testGlobalLexicalBinding(info().getName(pc)))
                return pushConstant(ObjectValue(*scope));
        }
        // Fall through to JSOP_BINDNAME
      case JSOP_BINDNAME:
        return jsop_bindname(info().getName(pc));

      case JSOP_DUP:
        current->pushSlot(current->stackDepth() - 1);
        return true;

      case JSOP_DUP2:
        return jsop_dup2();

      case JSOP_SWAP:
        current->swapAt(-1);
        return true;

      case JSOP_PICK:
        current->pick(-GET_INT8(pc));
        return true;

      case JSOP_GETALIASEDVAR:
        return jsop_getaliasedvar(ScopeCoordinate(pc));

      case JSOP_SETALIASEDVAR:
        return jsop_setaliasedvar(ScopeCoordinate(pc));

      case JSOP_UINT24:
        return pushConstant(Int32Value(GET_UINT24(pc)));

      case JSOP_INT32:
        return pushConstant(Int32Value(GET_INT32(pc)));

      case JSOP_LOOPHEAD:
        // JSOP_LOOPHEAD is handled when processing the loop header.
        MOZ_CRASH("JSOP_LOOPHEAD outside loop");

      case JSOP_GETELEM:
      case JSOP_CALLELEM:
        if (!jsop_getelem())
            return false;
        if (op == JSOP_CALLELEM && !improveThisTypesForCall())
            return false;
        return true;

      case JSOP_SETELEM:
      case JSOP_STRICTSETELEM:
        return jsop_setelem();

      case JSOP_LENGTH:
        return jsop_length();

      case JSOP_NOT:
        return jsop_not();

      case JSOP_FUNCTIONTHIS:
        return jsop_functionthis();

      case JSOP_GLOBALTHIS:
        return jsop_globalthis();

      case JSOP_CALLEE: {
         MDefinition* callee = getCallee();
         current->push(callee);
         return true;
      }

      case JSOP_GETPROP:
      case JSOP_CALLPROP:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        if (!jsop_getprop(name))
            return false;
        if (op == JSOP_CALLPROP && !improveThisTypesForCall())
            return false;
        return true;
      }

      case JSOP_SETPROP:
      case JSOP_STRICTSETPROP:
      case JSOP_SETNAME:
      case JSOP_STRICTSETNAME:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_setprop(name);
      }

      case JSOP_DELPROP:
      case JSOP_STRICTDELPROP:
      {
        PropertyName* name = info().getAtom(pc)->asPropertyName();
        return jsop_delprop(name);
      }

      case JSOP_DELELEM:
      case JSOP_STRICTDELELEM:
        return jsop_delelem();

      case JSOP_REGEXP:
        return jsop_regexp(info().getRegExp(pc));

      case JSOP_CALLSITEOBJ:
        return pushConstant(ObjectValue(*(info().getObject(pc))));

      case JSOP_OBJECT:
        return jsop_object(info().getObject(pc));

      case JSOP_TYPEOF:
      case JSOP_TYPEOFEXPR:
        return jsop_typeof();

      case JSOP_TOID:
        return jsop_toid();

      case JSOP_LAMBDA:
        return jsop_lambda(info().getFunction(pc));

      case JSOP_LAMBDA_ARROW:
        return jsop_lambda_arrow(info().getFunction(pc));

      case JSOP_ITER:
        return jsop_iter(GET_INT8(pc));

      case JSOP_MOREITER:
        return jsop_itermore();

      case JSOP_ISNOITER:
        return jsop_isnoiter();

      case JSOP_ENDITER:
        return jsop_iterend();

      case JSOP_IN:
        return jsop_in();

      case JSOP_SETRVAL:
        MOZ_ASSERT(!script()->noScriptRval());
        current->setSlot(info().returnValueSlot(), current->pop());
        return true;

      case JSOP_INSTANCEOF:
        return jsop_instanceof();

      case JSOP_DEBUGLEAVEBLOCK:
        return true;

      case JSOP_DEBUGGER:
        return jsop_debugger();

      case JSOP_GIMPLICITTHIS:
        if (!script()->hasNonSyntacticScope())
            return pushConstant(UndefinedValue());

        // Just fall through to the unsupported bytecode case.
        break;

      case JSOP_NEWTARGET:
        return jsop_newtarget();

      case JSOP_CHECKOBJCOERCIBLE:
        return jsop_checkobjcoercible();

#ifdef DEBUG
      case JSOP_PUSHBLOCKSCOPE:
      case JSOP_FRESHENBLOCKSCOPE:
      case JSOP_POPBLOCKSCOPE:
        // These opcodes are currently unhandled by Ion, but in principle
        // there's no reason they couldn't be.  Whenever this happens, OSR will
        // have to consider that JSOP_FRESHENBLOCK mutates the scope chain --
        // right now it caches the scope chain in MBasicBlock::scopeChain().
        // That stale value will have to be updated when JSOP_FRESHENBLOCK is
        // encountered.
#endif
      default:
        break;
    }

    // Track a simpler message, since the actionable abort message is a
    // static string, and the internal opcode name isn't an actionable
    // thing anyways.
    trackActionableAbort("Unsupported bytecode");
#ifdef DEBUG
    return abort("Unsupported opcode: %s", CodeName[op]);
#else
    return abort("Unsupported opcode: %d", op);
#endif
}

// Given that the current control flow structure has ended forcefully,
// via a return, break, or continue (rather than joining), propagate the
// termination up. For example, a return nested 5 loops deep may terminate
// every outer loop at once, if there are no intervening conditionals:
//
// for (...) {
//   for (...) {
//     return x;
//   }
// }
//
// If |current| is nullptr when this function returns, then there is no more
// control flow to be processed.
IonBuilder::ControlStatus
IonBuilder::processControlEnd()
{
    MOZ_ASSERT(!current);

    if (cfgStack_.empty()) {
        // If there is no more control flow to process, then this is the
        // last return in the function.
        return ControlStatus_Ended;
    }

    return processCfgStack();
}

// Processes the top of the CFG stack. This is used from two places:
// (1) processControlEnd(), whereby a break, continue, or return may interrupt
//     an in-progress CFG structure before reaching its actual termination
//     point in the bytecode.
// (2) traverseBytecode(), whereby we reach the last instruction in a CFG
//     structure.
IonBuilder::ControlStatus
IonBuilder::processCfgStack()
{
    ControlStatus status = processCfgEntry(cfgStack_.back());

    // If this terminated a CFG structure, act like processControlEnd() and
    // keep propagating upward.
    while (status == ControlStatus_Ended) {
        popCfgStack();
        if (cfgStack_.empty())
            return status;
        status = processCfgEntry(cfgStack_.back());
    }

    // If some join took place, the current structure is finished.
    if (status == ControlStatus_Joined)
        popCfgStack();

    return status;
}

IonBuilder::ControlStatus
IonBuilder::processCfgEntry(CFGState& state)
{
    switch (state.state) {
      case CFGState::IF_TRUE:
      case CFGState::IF_TRUE_EMPTY_ELSE:
        return processIfEnd(state);

      case CFGState::IF_ELSE_TRUE:
        return processIfElseTrueEnd(state);

      case CFGState::IF_ELSE_FALSE:
        return processIfElseFalseEnd(state);

      case CFGState::DO_WHILE_LOOP_BODY:
        return processDoWhileBodyEnd(state);

      case CFGState::DO_WHILE_LOOP_COND:
        return processDoWhileCondEnd(state);

      case CFGState::WHILE_LOOP_COND:
        return processWhileCondEnd(state);

      case CFGState::WHILE_LOOP_BODY:
        return processWhileBodyEnd(state);

      case CFGState::FOR_LOOP_COND:
        return processForCondEnd(state);

      case CFGState::FOR_LOOP_BODY:
        return processForBodyEnd(state);

      case CFGState::FOR_LOOP_UPDATE:
        return processForUpdateEnd(state);

      case CFGState::TABLE_SWITCH:
        return processNextTableSwitchCase(state);

      case CFGState::COND_SWITCH_CASE:
        return processCondSwitchCase(state);

      case CFGState::COND_SWITCH_BODY:
        return processCondSwitchBody(state);

      case CFGState::AND_OR:
        return processAndOrEnd(state);

      case CFGState::LABEL:
        return processLabelEnd(state);

      case CFGState::TRY:
        return processTryEnd(state);

      default:
        MOZ_CRASH("unknown cfgstate");
    }
}

IonBuilder::ControlStatus
IonBuilder::processIfEnd(CFGState& state)
{
    bool thenBranchTerminated = !current;
    if (!thenBranchTerminated) {
        // Here, the false block is the join point. Create an edge from the
        // current block to the false block. Note that a RETURN opcode
        // could have already ended the block.
        current->end(MGoto::New(alloc(), state.branch.ifFalse));

        if (!state.branch.ifFalse->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    }

    if (!setCurrentAndSpecializePhis(state.branch.ifFalse))
        return ControlStatus_Error;
    graph().moveBlockToEnd(current);
    pc = current->pc();

    if (thenBranchTerminated) {
        // If we can't reach here via the then-branch, we can filter the types
        // after the if-statement based on the if-condition.
        MTest* test = state.branch.test;
        if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
            return ControlStatus_Error;
    }

    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseTrueEnd(CFGState& state)
{
    // We've reached the end of the true branch of an if-else. Don't
    // create an edge yet, just transition to parsing the false branch.
    state.state = CFGState::IF_ELSE_FALSE;
    state.branch.ifTrue = current;
    state.stopAt = state.branch.falseEnd;
    pc = state.branch.ifFalse->pc();
    if (!setCurrentAndSpecializePhis(state.branch.ifFalse))
        return ControlStatus_Error;
    graph().moveBlockToEnd(current);

    MTest* test = state.branch.test;
    if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
        return ControlStatus_Error;

    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseFalseEnd(CFGState& state)
{
    // Update the state to have the latest block from the false path.
    state.branch.ifFalse = current;

    // To create the join node, we need an incoming edge that has not been
    // terminated yet.
    MBasicBlock* pred = state.branch.ifTrue
                        ? state.branch.ifTrue
                        : state.branch.ifFalse;
    MBasicBlock* other = (pred == state.branch.ifTrue) ? state.branch.ifFalse : state.branch.ifTrue;

    if (!pred)
        return ControlStatus_Ended;

    // Create a new block to represent the join.
    MBasicBlock* join = newBlock(pred, state.branch.falseEnd);
    if (!join)
        return ControlStatus_Error;

    // Create edges from the true and false blocks as needed.
    pred->end(MGoto::New(alloc(), join));

    if (other) {
        other->end(MGoto::New(alloc(), join));
        if (!join->addPredecessor(alloc(), other))
            return ControlStatus_Error;
    }

    // Ignore unreachable remainder of false block if existent.
    if (!setCurrentAndSpecializePhis(join))
        return ControlStatus_Error;
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBrokenLoop(CFGState& state)
{
    MOZ_ASSERT(!current);

    MOZ_ASSERT(loopDepth_);
    loopDepth_--;

    // A broken loop is not a real loop (it has no header or backedge), so
    // reset the loop depth.
    for (MBasicBlockIterator i(graph().begin(state.loop.entry)); i != graph().end(); i++) {
        if (i->loopDepth() > loopDepth_)
            i->setLoopDepth(i->loopDepth() - 1);
    }

    // If the loop started with a condition (while/for) then even if the
    // structure never actually loops, the condition itself can still fail and
    // thus we must resume at the successor, if one exists.
    if (!setCurrentAndSpecializePhis(state.loop.successor))
        return ControlStatus_Error;
    if (current) {
        MOZ_ASSERT(current->loopDepth() == loopDepth_);
        graph().moveBlockToEnd(current);
    }

    // Join the breaks together and continue parsing.
    if (state.loop.breaks) {
        MBasicBlock* block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (current) {
            current->end(MGoto::New(alloc(), block));
            if (!block->addPredecessor(alloc(), current))
                return ControlStatus_Error;
        }

        if (!setCurrentAndSpecializePhis(block))
            return ControlStatus_Error;
    }

    // If the loop is not gated on a condition, and has only returns, we'll
    // reach this case. For example:
    // do { ... return; } while ();
    if (!current)
        return ControlStatus_Ended;

    // Otherwise, the loop is gated on a condition and/or has breaks so keep
    // parsing at the successor.
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::finishLoop(CFGState& state, MBasicBlock* successor)
{
    MOZ_ASSERT(current);

    MOZ_ASSERT(loopDepth_);
    loopDepth_--;
    MOZ_ASSERT_IF(successor, successor->loopDepth() == loopDepth_);

    // Compute phis in the loop header and propagate them throughout the loop,
    // including the successor.
    AbortReason r = state.loop.entry->setBackedge(current);
    if (r == AbortReason_Alloc)
        return ControlStatus_Error;
    if (r == AbortReason_Disable) {
        // If there are types for variables on the backedge that were not
        // present at the original loop header, then uses of the variables'
        // phis may have generated incorrect nodes. The new types have been
        // incorporated into the header phis, so remove all blocks for the
        // loop body and restart with the new types.
        return restartLoop(state);
    }

    if (successor) {
        graph().moveBlockToEnd(successor);
        successor->inheritPhis(state.loop.entry);
    }

    if (state.loop.breaks) {
        // Propagate phis placed in the header to individual break exit points.
        DeferredEdge* edge = state.loop.breaks;
        while (edge) {
            edge->block->inheritPhis(state.loop.entry);
            edge = edge->next;
        }

        // Create a catch block to join all break exits.
        MBasicBlock* block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (successor) {
            // Finally, create an unconditional edge from the successor to the
            // catch block.
            successor->end(MGoto::New(alloc(), block));
            if (!block->addPredecessor(alloc(), successor))
                return ControlStatus_Error;
        }
        successor = block;
    }

    if (!setCurrentAndSpecializePhis(successor))
        return ControlStatus_Error;

    // An infinite loop (for (;;) { }) will not have a successor.
    if (!current)
        return ControlStatus_Ended;

    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::restartLoop(CFGState state)
{
    spew("New types at loop header, restarting loop body");

    if (JitOptions.limitScriptSize) {
        if (++numLoopRestarts_ >= MAX_LOOP_RESTARTS)
            return ControlStatus_Abort;
    }

    MBasicBlock* header = state.loop.entry;

    // Discard unreferenced & pre-allocated resume points.
    replaceMaybeFallbackFunctionGetter(nullptr);

    // Remove all blocks in the loop body other than the header, which has phis
    // of the appropriate type and incoming edges to preserve.
    graph().removeBlocksAfter(header);

    // Remove all instructions from the header itself, and all resume points
    // except the entry resume point.
    header->discardAllInstructions();
    header->discardAllResumePoints(/* discardEntry = */ false);
    header->setStackDepth(header->getPredecessor(0)->stackDepth());

    popCfgStack();

    loopDepth_++;

    if (!pushLoop(state.loop.initialState, state.loop.initialStopAt, header, state.loop.osr,
                  state.loop.loopHead, state.loop.initialPc,
                  state.loop.bodyStart, state.loop.bodyEnd,
                  state.loop.exitpc, state.loop.continuepc))
    {
        return ControlStatus_Error;
    }

    CFGState& nstate = cfgStack_.back();

    nstate.loop.condpc = state.loop.condpc;
    nstate.loop.updatepc = state.loop.updatepc;
    nstate.loop.updateEnd = state.loop.updateEnd;

    // Don't specializePhis(), as the header has been visited before and the
    // phis have already had their type set.
    setCurrent(header);

    if (!jsop_loophead(nstate.loop.loopHead))
        return ControlStatus_Error;

    pc = nstate.loop.initialPc;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // No current means control flow cannot reach the condition, so this will
    // never loop.
    if (!current)
        return processBrokenLoop(state);

    MBasicBlock* header = newBlock(current, state.loop.updatepc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(alloc(), header));

    state.state = CFGState::DO_WHILE_LOOP_COND;
    state.stopAt = state.loop.updateEnd;
    pc = state.loop.updatepc;
    if (!setCurrentAndSpecializePhis(header))
        return ControlStatus_Error;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // We're guaranteed a |current|, it's impossible to break or return from
    // inside the conditional expression.
    MOZ_ASSERT(current);

    // Pop the last value, and create the successor block.
    MDefinition* vins = current->pop();
    MBasicBlock* successor = newBlock(current, GetNextPc(pc), loopDepth_ - 1);
    if (!successor)
        return ControlStatus_Error;

    // Test for do {} while(false) and don't create a loop in that case.
    if (vins->isConstantValue() && !vins->constantValue().isMagic()) {
        if (!vins->constantToBoolean()) {
            current->end(MGoto::New(alloc(), successor));
            current = nullptr;

            state.loop.successor = successor;
            return processBrokenLoop(state);
        }
    }

    // Create the test instruction and end the current block.
    MTest* test = newTest(vins, state.loop.entry, successor);
    current->end(test);
    return finishLoop(state, successor);
}

IonBuilder::ControlStatus
IonBuilder::processWhileCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE || JSOp(*pc) == JSOP_IFEQ);

    // Balance the stack past the IFNE.
    MDefinition* ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock* body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest* test;
    if (JSOp(*pc) == JSOP_IFNE)
        test = newTest(ins, body, state.loop.successor);
    else
        test = newTest(ins, state.loop.successor, body);
    current->end(test);

    state.state = CFGState::WHILE_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    if (!setCurrentAndSpecializePhis(body))
        return ControlStatus_Error;

    // Filter the types in the loop body.
    if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
        return ControlStatus_Error;

    // If this is a for-in loop, unbox the current value as string if possible.
    if (ins->isIsNoIter()) {
        MIteratorMore* iterMore = ins->toIsNoIter()->input()->toIteratorMore();
        jsbytecode* iterMorePc = iterMore->resumePoint()->pc();
        MOZ_ASSERT(*iterMorePc == JSOP_MOREITER);

        if (!nonStringIteration_ && !inspector->hasSeenNonStringIterMore(iterMorePc)) {
            MDefinition* val = current->peek(-1);
            MOZ_ASSERT(val == iterMore);
            MInstruction* ins = MUnbox::New(alloc(), val, MIRType_String, MUnbox::Fallible,
                                            Bailout_NonStringInputInvalidate);
            current->add(ins);
            current->rewriteAtDepth(-1, ins);
        }
    }

    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processWhileBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(alloc(), state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

IonBuilder::ControlStatus
IonBuilder::processForCondEnd(CFGState& state)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Balance the stack past the IFNE.
    MDefinition* ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock* body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest* test = newTest(ins, body, state.loop.successor);
    current->end(test);

    state.state = CFGState::FOR_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    if (!setCurrentAndSpecializePhis(body))
        return ControlStatus_Error;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForBodyEnd(CFGState& state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // If there is no updatepc, just go right to processing what would be the
    // end of the update clause. Otherwise, |current| might be nullptr; if this is
    // the case, the udpate is unreachable anyway.
    if (!state.loop.updatepc || !current)
        return processForUpdateEnd(state);

    pc = state.loop.updatepc;

    state.state = CFGState::FOR_LOOP_UPDATE;
    state.stopAt = state.loop.updateEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForUpdateEnd(CFGState& state)
{
    // If there is no current, we couldn't reach the loop edge and there was no
    // update clause.
    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(alloc(), state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

IonBuilder::DeferredEdge*
IonBuilder::filterDeadDeferredEdges(DeferredEdge* edge)
{
    DeferredEdge* head = edge;
    DeferredEdge* prev = nullptr;

    while (edge) {
        if (edge->block->isDead()) {
            if (prev)
                prev->next = edge->next;
            else
                head = edge->next;
        } else {
            prev = edge;
        }
        edge = edge->next;
    }

    // There must be at least one deferred edge from a block that was not
    // deleted; blocks are deleted when restarting processing of a loop, and
    // the final version of the loop body will have edges from live blocks.
    MOZ_ASSERT(head);

    return head;
}

bool
IonBuilder::processDeferredContinues(CFGState& state)
{
    // If there are any continues for this loop, and there is an update block,
    // then we need to create a new basic block to house the update.
    if (state.loop.continues) {
        DeferredEdge* edge = filterDeadDeferredEdges(state.loop.continues);

        MBasicBlock* update = newBlock(edge->block, loops_.back().continuepc);
        if (!update)
            return false;

        if (current) {
            current->end(MGoto::New(alloc(), update));
            if (!update->addPredecessor(alloc(), current))
                return false;
        }

        // No need to use addPredecessor for first edge,
        // because it is already predecessor.
        edge->block->end(MGoto::New(alloc(), update));
        edge = edge->next;

        // Remaining edges
        while (edge) {
            edge->block->end(MGoto::New(alloc(), update));
            if (!update->addPredecessor(alloc(), edge->block))
                return false;
            edge = edge->next;
        }
        state.loop.continues = nullptr;

        if (!setCurrentAndSpecializePhis(update))
            return ControlStatus_Error;
    }

    return true;
}

MBasicBlock*
IonBuilder::createBreakCatchBlock(DeferredEdge* edge, jsbytecode* pc)
{
    edge = filterDeadDeferredEdges(edge);

    // Create block, using the first break statement as predecessor
    MBasicBlock* successor = newBlock(edge->block, pc);
    if (!successor)
        return nullptr;

    // No need to use addPredecessor for first edge,
    // because it is already predecessor.
    edge->block->end(MGoto::New(alloc(), successor));
    edge = edge->next;

    // Finish up remaining breaks.
    while (edge) {
        edge->block->end(MGoto::New(alloc(), successor));
        if (!successor->addPredecessor(alloc(), edge->block))
            return nullptr;
        edge = edge->next;
    }

    return successor;
}

IonBuilder::ControlStatus
IonBuilder::processNextTableSwitchCase(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::TABLE_SWITCH);

    state.tableswitch.currentBlock++;

    // Test if there are still unprocessed successors (cases/default)
    if (state.tableswitch.currentBlock >= state.tableswitch.ins->numBlocks())
        return processSwitchEnd(state.tableswitch.breaks, state.tableswitch.exitpc);

    // Get the next successor
    MBasicBlock* successor = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock);

    // Add current block as predecessor if available.
    // This means the previous case didn't have a break statement.
    // So flow will continue in this block.
    if (current) {
        current->end(MGoto::New(alloc(), successor));
        if (!successor->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    } else {
        // If this is an actual case statement, optimize by replacing the
        // input to the switch case with the actual number of the case.
        // This constant has been emitted when creating the case blocks.
        if (state.tableswitch.ins->getDefault() != successor) {
            MConstant* constant = successor->begin()->toConstant();
            for (uint32_t j = 0; j < successor->stackDepth(); j++) {
                MDefinition* ins = successor->getSlot(j);
                if (ins != state.tableswitch.ins->getOperand(0))
                    continue;

                constant->setDependency(state.tableswitch.ins);
                successor->setSlot(j, constant);
            }
        }
    }

    // Insert successor after the current block, to maintain RPO.
    graph().moveBlockToEnd(successor);

    // If this is the last successor the block should stop at the end of the tableswitch
    // Else it should stop at the start of the next successor
    if (state.tableswitch.currentBlock+1 < state.tableswitch.ins->numBlocks())
        state.stopAt = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock+1)->pc();
    else
        state.stopAt = state.tableswitch.exitpc;

    if (!setCurrentAndSpecializePhis(successor))
        return ControlStatus_Error;
    pc = current->pc();
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processAndOrEnd(CFGState& state)
{
    MOZ_ASSERT(current);
    MBasicBlock* lhs = state.branch.ifFalse;

    // Create a new block to represent the join.
    MBasicBlock* join = newBlock(current, state.stopAt);
    if (!join)
        return ControlStatus_Error;

    // End the rhs.
    current->end(MGoto::New(alloc(), join));

    // End the lhs.
    lhs->end(MGoto::New(alloc(), join));
    if (!join->addPredecessor(alloc(), state.branch.ifFalse))
        return ControlStatus_Error;

    // Set the join path as current path.
    if (!setCurrentAndSpecializePhis(join))
        return ControlStatus_Error;
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processLabelEnd(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::LABEL);

    // If there are no breaks and no current, controlflow is terminated.
    if (!state.label.breaks && !current)
        return ControlStatus_Ended;

    // If there are no breaks to this label, there's nothing to do.
    if (!state.label.breaks)
        return ControlStatus_Joined;

    MBasicBlock* successor = createBreakCatchBlock(state.label.breaks, state.stopAt);
    if (!successor)
        return ControlStatus_Error;

    if (current) {
        current->end(MGoto::New(alloc(), successor));
        if (!successor->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    }

    pc = state.stopAt;
    if (!setCurrentAndSpecializePhis(successor))
        return ControlStatus_Error;
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processTryEnd(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::TRY);

    if (!state.try_.successor) {
        MOZ_ASSERT(!current);
        return ControlStatus_Ended;
    }

    if (current) {
        current->end(MGoto::New(alloc(), state.try_.successor));

        if (!state.try_.successor->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    }

    // Start parsing the code after this try-catch statement.
    if (!setCurrentAndSpecializePhis(state.try_.successor))
        return ControlStatus_Error;
    graph().moveBlockToEnd(current);
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBreak(JSOp op, jssrcnote* sn)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    MOZ_ASSERT(SN_TYPE(sn) == SRC_BREAK ||
               SN_TYPE(sn) == SRC_BREAK2LABEL);

    // Find the break target.
    jsbytecode* target = pc + GetJumpOffset(pc);
    DebugOnly<bool> found = false;

    if (SN_TYPE(sn) == SRC_BREAK2LABEL) {
        for (size_t i = labels_.length() - 1; i < labels_.length(); i--) {
            CFGState& cfg = cfgStack_[labels_[i].cfgEntry];
            MOZ_ASSERT(cfg.state == CFGState::LABEL);
            if (cfg.stopAt == target) {
                cfg.label.breaks = new(alloc()) DeferredEdge(current, cfg.label.breaks);
                found = true;
                break;
            }
        }
    } else {
        for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
            CFGState& cfg = cfgStack_[loops_[i].cfgEntry];
            MOZ_ASSERT(cfg.isLoop());
            if (cfg.loop.exitpc == target) {
                cfg.loop.breaks = new(alloc()) DeferredEdge(current, cfg.loop.breaks);
                found = true;
                break;
            }
        }
    }

    MOZ_ASSERT(found);

    setCurrent(nullptr);
    pc += CodeSpec[op].length;
    return processControlEnd();
}

static inline jsbytecode*
EffectiveContinue(jsbytecode* pc)
{
    if (JSOp(*pc) == JSOP_GOTO)
        return pc + GetJumpOffset(pc);
    return pc;
}

IonBuilder::ControlStatus
IonBuilder::processContinue(JSOp op)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    // Find the target loop.
    CFGState* found = nullptr;
    jsbytecode* target = pc + GetJumpOffset(pc);
    for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
        if (loops_[i].continuepc == target ||
            EffectiveContinue(loops_[i].continuepc) == target)
        {
            found = &cfgStack_[loops_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    MOZ_ASSERT(found);
    CFGState& state = *found;

    state.loop.continues = new(alloc()) DeferredEdge(current, state.loop.continues);

    setCurrent(nullptr);
    pc += CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processSwitchBreak(JSOp op)
{
    MOZ_ASSERT(op == JSOP_GOTO);

    // Find the target switch.
    CFGState* found = nullptr;
    jsbytecode* target = pc + GetJumpOffset(pc);
    for (size_t i = switches_.length() - 1; i < switches_.length(); i--) {
        if (switches_[i].continuepc == target) {
            found = &cfgStack_[switches_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    MOZ_ASSERT(found);
    CFGState& state = *found;

    DeferredEdge** breaks = nullptr;
    switch (state.state) {
      case CFGState::TABLE_SWITCH:
        breaks = &state.tableswitch.breaks;
        break;
      case CFGState::COND_SWITCH_BODY:
        breaks = &state.condswitch.breaks;
        break;
      default:
        MOZ_CRASH("Unexpected switch state.");
    }

    *breaks = new(alloc()) DeferredEdge(current, *breaks);

    setCurrent(nullptr);
    pc += CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processSwitchEnd(DeferredEdge* breaks, jsbytecode* exitpc)
{
    // No break statements, no current.
    // This means that control flow is cut-off from this point
    // (e.g. all cases have return statements).
    if (!breaks && !current)
        return ControlStatus_Ended;

    // Create successor block.
    // If there are breaks, create block with breaks as predecessor
    // Else create a block with current as predecessor
    MBasicBlock* successor = nullptr;
    if (breaks)
        successor = createBreakCatchBlock(breaks, exitpc);
    else
        successor = newBlock(current, exitpc);

    if (!successor)
        return ControlStatus_Ended;

    // If there is current, the current block flows into this one.
    // So current is also a predecessor to this block
    if (current) {
        current->end(MGoto::New(alloc(), successor));
        if (breaks) {
            if (!successor->addPredecessor(alloc(), current))
                return ControlStatus_Error;
        }
    }

    pc = exitpc;
    if (!setCurrentAndSpecializePhis(successor))
        return ControlStatus_Error;
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::maybeLoop(JSOp op, jssrcnote* sn)
{
    // This function looks at the opcode and source note and tries to
    // determine the structure of the loop. For some opcodes, like
    // POP/NOP which are not explicitly control flow, this source note is
    // optional. For opcodes with control flow, like GOTO, an unrecognized
    // or not-present source note is a compilation failure.
    switch (op) {
      case JSOP_POP:
        // for (init; ; update?) ...
        if (sn && SN_TYPE(sn) == SRC_FOR) {
            current->pop();
            return forLoop(op, sn);
        }
        break;

      case JSOP_NOP:
        if (sn) {
            // do { } while (cond)
            if (SN_TYPE(sn) == SRC_WHILE)
                return doWhileLoop(op, sn);
            // Build a mapping such that given a basic block, whose successor
            // has a phi

            // for (; ; update?)
            if (SN_TYPE(sn) == SRC_FOR)
                return forLoop(op, sn);
        }
        break;

      default:
        MOZ_CRASH("unexpected opcode");
    }

    return ControlStatus_None;
}

void
IonBuilder::assertValidLoopHeadOp(jsbytecode* pc)
{
#ifdef DEBUG
    MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPHEAD);

    // Make sure this is the next opcode after the loop header,
    // unless the for loop is unconditional.
    CFGState& state = cfgStack_.back();
    MOZ_ASSERT_IF((JSOp)*(state.loop.entry->pc()) == JSOP_GOTO,
         GetNextPc(state.loop.entry->pc()) == pc);

    // do-while loops have a source note.
    jssrcnote* sn = info().getNote(gsn, pc);
    if (sn) {
        jsbytecode* ifne = pc + GetSrcNoteOffset(sn, 0);

        jsbytecode* expected_ifne;
        switch (state.state) {
          case CFGState::DO_WHILE_LOOP_BODY:
            expected_ifne = state.loop.updateEnd;
            break;

          default:
            MOZ_CRASH("JSOP_LOOPHEAD unexpected source note");
        }

        // Make sure this loop goes to the same ifne as the loop header's
        // source notes or GOTO.
        MOZ_ASSERT(ifne == expected_ifne);
    } else {
        MOZ_ASSERT(state.state != CFGState::DO_WHILE_LOOP_BODY);
    }
#endif
}

IonBuilder::ControlStatus
IonBuilder::doWhileLoop(JSOp op, jssrcnote* sn)
{
    // do { } while() loops have the following structure:
    //    NOP         ; SRC_WHILE (offset to COND)
    //    LOOPHEAD    ; SRC_WHILE (offset to IFNE)
    //    LOOPENTRY
    //    ...         ; body
    //    ...
    //    COND        ; start of condition
    //    ...
    //    IFNE ->     ; goes to LOOPHEAD
    int condition_offset = GetSrcNoteOffset(sn, 0);
    jsbytecode* conditionpc = pc + condition_offset;

    jssrcnote* sn2 = info().getNote(gsn, pc+1);
    int offset = GetSrcNoteOffset(sn2, 0);
    jsbytecode* ifne = pc + offset + 1;
    MOZ_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    jsbytecode* loopHead = GetNextPc(pc);
    MOZ_ASSERT(JSOp(*loopHead) == JSOP_LOOPHEAD);
    MOZ_ASSERT(loopHead == ifne + GetJumpOffset(ifne));

    jsbytecode* loopEntry = GetNextPc(loopHead);
    bool canOsr = LoopEntryCanIonOsr(loopEntry);
    bool osr = info().hasOsrAt(loopEntry);

    if (osr) {
        MBasicBlock* preheader = newOsrPreheader(current, loopEntry, pc);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(alloc(), preheader));
        if (!setCurrentAndSpecializePhis(preheader))
            return ControlStatus_Error;
    }

    unsigned stackPhiCount = 0;
    MBasicBlock* header = newPendingLoopHeader(current, loopEntry, osr, canOsr, stackPhiCount);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(alloc(), header));

    jsbytecode* loophead = GetNextPc(pc);
    jsbytecode* bodyStart = GetNextPc(loophead);
    jsbytecode* bodyEnd = conditionpc;
    jsbytecode* exitpc = GetNextPc(ifne);
    if (!analyzeNewLoopTypes(header, bodyStart, exitpc))
        return ControlStatus_Error;
    if (!pushLoop(CFGState::DO_WHILE_LOOP_BODY, conditionpc, header, osr,
                  loopHead, bodyStart, bodyStart, bodyEnd, exitpc, conditionpc))
    {
        return ControlStatus_Error;
    }

    CFGState& state = cfgStack_.back();
    state.loop.updatepc = conditionpc;
    state.loop.updateEnd = ifne;

    if (!setCurrentAndSpecializePhis(header))
        return ControlStatus_Error;
    if (!jsop_loophead(loophead))
        return ControlStatus_Error;

    pc = bodyStart;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::whileOrForInLoop(jssrcnote* sn)
{
    // while (cond) { } loops have the following structure:
    //    GOTO cond   ; SRC_WHILE (offset to IFNE)
    //    LOOPHEAD
    //    ...
    //  cond:
    //    LOOPENTRY
    //    ...
    //    IFNE        ; goes to LOOPHEAD
    // for (x in y) { } loops are similar; the cond will be a MOREITER.
    MOZ_ASSERT(SN_TYPE(sn) == SRC_FOR_OF || SN_TYPE(sn) == SRC_FOR_IN || SN_TYPE(sn) == SRC_WHILE);
    int ifneOffset = GetSrcNoteOffset(sn, 0);
    jsbytecode* ifne = pc + ifneOffset;
    MOZ_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    MOZ_ASSERT(JSOp(*GetNextPc(pc)) == JSOP_LOOPHEAD);
    MOZ_ASSERT(GetNextPc(pc) == ifne + GetJumpOffset(ifne));

    jsbytecode* loopEntry = pc + GetJumpOffset(pc);
    bool canOsr = LoopEntryCanIonOsr(loopEntry);
    bool osr = info().hasOsrAt(loopEntry);

    if (osr) {
        MBasicBlock* preheader = newOsrPreheader(current, loopEntry, pc);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(alloc(), preheader));
        if (!setCurrentAndSpecializePhis(preheader))
            return ControlStatus_Error;
    }

    unsigned stackPhiCount;
    if (SN_TYPE(sn) == SRC_FOR_OF)
        stackPhiCount = 2;
    else if (SN_TYPE(sn) == SRC_FOR_IN)
        stackPhiCount = 1;
    else
        stackPhiCount = 0;

    MBasicBlock* header = newPendingLoopHeader(current, loopEntry, osr, canOsr, stackPhiCount);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(alloc(), header));

    // Skip past the JSOP_LOOPHEAD for the body start.
    jsbytecode* loopHead = GetNextPc(pc);
    jsbytecode* bodyStart = GetNextPc(loopHead);
    jsbytecode* bodyEnd = pc + GetJumpOffset(pc);
    jsbytecode* exitpc = GetNextPc(ifne);
    jsbytecode* continuepc = pc;
    if (!analyzeNewLoopTypes(header, bodyStart, exitpc))
        return ControlStatus_Error;
    if (!pushLoop(CFGState::WHILE_LOOP_COND, ifne, header, osr,
                  loopHead, bodyEnd, bodyStart, bodyEnd, exitpc, continuepc))
    {
        return ControlStatus_Error;
    }

    // Parse the condition first.
    if (!setCurrentAndSpecializePhis(header))
        return ControlStatus_Error;
    if (!jsop_loophead(loopHead))
        return ControlStatus_Error;

    pc = bodyEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::forLoop(JSOp op, jssrcnote* sn)
{
    // Skip the NOP.
    MOZ_ASSERT(op == JSOP_NOP);
    pc = GetNextPc(pc);

    jsbytecode* condpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* updatepc = pc + GetSrcNoteOffset(sn, 1);
    jsbytecode* ifne = pc + GetSrcNoteOffset(sn, 2);
    jsbytecode* exitpc = GetNextPc(ifne);

    // for loops have the following structures:
    //
    //   NOP or POP
    //   [GOTO cond | NOP]
    //   LOOPHEAD
    // body:
    //    ; [body]
    // [increment:]
    //   [FRESHENBLOCKSCOPE, if needed by a cloned block]
    //    ; [increment]
    // [cond:]
    //   LOOPENTRY
    //   GOTO body
    //
    // If there is a condition (condpc != ifne), this acts similar to a while
    // loop otherwise, it acts like a do-while loop.
    //
    // Note that currently Ion does not compile pushblockscope/popblockscope as
    // necessary prerequisites to freshenblockscope.  So the code below doesn't
    // and needn't consider the implications of freshenblockscope.
    jsbytecode* bodyStart = pc;
    jsbytecode* bodyEnd = updatepc;
    jsbytecode* loopEntry = condpc;
    if (condpc != ifne) {
        MOZ_ASSERT(JSOp(*bodyStart) == JSOP_GOTO);
        MOZ_ASSERT(bodyStart + GetJumpOffset(bodyStart) == condpc);
        bodyStart = GetNextPc(bodyStart);
    } else {
        // No loop condition, such as for(j = 0; ; j++)
        if (op != JSOP_NOP) {
            // If the loop starts with POP, we have to skip a NOP.
            MOZ_ASSERT(JSOp(*bodyStart) == JSOP_NOP);
            bodyStart = GetNextPc(bodyStart);
        }
        loopEntry = GetNextPc(bodyStart);
    }
    jsbytecode* loopHead = bodyStart;
    MOZ_ASSERT(JSOp(*bodyStart) == JSOP_LOOPHEAD);
    MOZ_ASSERT(ifne + GetJumpOffset(ifne) == bodyStart);
    bodyStart = GetNextPc(bodyStart);

    bool osr = info().hasOsrAt(loopEntry);
    bool canOsr = LoopEntryCanIonOsr(loopEntry);

    if (osr) {
        MBasicBlock* preheader = newOsrPreheader(current, loopEntry, pc);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(alloc(), preheader));
        if (!setCurrentAndSpecializePhis(preheader))
            return ControlStatus_Error;
    }

    unsigned stackPhiCount = 0;
    MBasicBlock* header = newPendingLoopHeader(current, loopEntry, osr, canOsr, stackPhiCount);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(alloc(), header));

    // If there is no condition, we immediately parse the body. Otherwise, we
    // parse the condition.
    jsbytecode* stopAt;
    CFGState::State initial;
    if (condpc != ifne) {
        pc = condpc;
        stopAt = ifne;
        initial = CFGState::FOR_LOOP_COND;
    } else {
        pc = bodyStart;
        stopAt = bodyEnd;
        initial = CFGState::FOR_LOOP_BODY;
    }

    if (!analyzeNewLoopTypes(header, bodyStart, exitpc))
        return ControlStatus_Error;
    if (!pushLoop(initial, stopAt, header, osr,
                  loopHead, pc, bodyStart, bodyEnd, exitpc, updatepc))
    {
        return ControlStatus_Error;
    }

    CFGState& state = cfgStack_.back();
    state.loop.condpc = (condpc != ifne) ? condpc : nullptr;
    state.loop.updatepc = (updatepc != condpc) ? updatepc : nullptr;
    if (state.loop.updatepc)
        state.loop.updateEnd = condpc;

    if (!setCurrentAndSpecializePhis(header))
        return ControlStatus_Error;
    if (!jsop_loophead(loopHead))
        return ControlStatus_Error;

    return ControlStatus_Jumped;
}

int
IonBuilder::CmpSuccessors(const void* a, const void* b)
{
    const MBasicBlock* a0 = * (MBasicBlock * const*)a;
    const MBasicBlock* b0 = * (MBasicBlock * const*)b;
    if (a0->pc() == b0->pc())
        return 0;

    return (a0->pc() > b0->pc()) ? 1 : -1;
}

IonBuilder::ControlStatus
IonBuilder::tableSwitch(JSOp op, jssrcnote* sn)
{
    // TableSwitch op contains the following data
    // (length between data is JUMP_OFFSET_LEN)
    //
    // 0: Offset of default case
    // 1: Lowest number in tableswitch
    // 2: Highest number in tableswitch
    // 3: Offset of case low
    // 4: Offset of case low+1
    // .: ...
    // .: Offset of case high

    MOZ_ASSERT(op == JSOP_TABLESWITCH);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_TABLESWITCH);

    // Pop input.
    MDefinition* ins = current->pop();

    // Get the default and exit pc
    jsbytecode* exitpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* defaultpc = pc + GET_JUMP_OFFSET(pc);

    MOZ_ASSERT(defaultpc > pc && defaultpc <= exitpc);

    // Get the low and high from the tableswitch
    jsbytecode* pc2 = pc;
    pc2 += JUMP_OFFSET_LEN;
    int low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int high = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;

    // Create MIR instruction
    MTableSwitch* tableswitch = MTableSwitch::New(alloc(), ins, low, high);

    // Create default case
    MBasicBlock* defaultcase = newBlock(current, defaultpc);
    if (!defaultcase)
        return ControlStatus_Error;

    if (!tableswitch->addDefault(defaultcase))
        return ControlStatus_Error;

    if (!tableswitch->addBlock(defaultcase))
        return ControlStatus_Error;

    // Create cases
    jsbytecode* casepc = nullptr;
    for (int i = 0; i < high-low+1; i++) {
        casepc = pc + GET_JUMP_OFFSET(pc2);

        MOZ_ASSERT(casepc >= pc && casepc <= exitpc);
        MBasicBlock* caseblock;

        if (casepc == pc) {
            // If the casepc equals the current pc, it is not a written case,
            // but a filled gap. That way we can use a tableswitch instead of
            // condswitch, even if not all numbers are consecutive.
            // In that case this block goes to the default case
            caseblock = newBlock(current, defaultpc);
            if (!caseblock)
                return ControlStatus_Error;
            caseblock->end(MGoto::New(alloc(), defaultcase));
            if (!defaultcase->addPredecessor(alloc(), caseblock))
                return ControlStatus_Error;
        } else {
            // If this is an actual case (not filled gap),
            // add this block to the list that still needs to get processed.
            caseblock = newBlock(current, casepc);
            if (!caseblock)
                return ControlStatus_Error;

            if (!tableswitch->addBlock(caseblock))
                return ControlStatus_Error;

            // Add constant to indicate which case this is for use by
            // processNextTableSwitchCase.
            MConstant* constant = MConstant::New(alloc(), Int32Value(i + low));
            caseblock->add(constant);
        }

        size_t caseIndex;
        if (!tableswitch->addSuccessor(caseblock, &caseIndex))
            return ControlStatus_Error;

        if (!tableswitch->addCase(caseIndex))
            return ControlStatus_Error;

        pc2 += JUMP_OFFSET_LEN;
    }

    // Move defaultcase to the end, to maintain RPO.
    graph().moveBlockToEnd(defaultcase);

    MOZ_ASSERT(tableswitch->numCases() == (uint32_t)(high - low + 1));
    MOZ_ASSERT(tableswitch->numSuccessors() > 0);

    // Sort the list of blocks that still needs to get processed by pc
    qsort(tableswitch->blocks(), tableswitch->numBlocks(),
          sizeof(MBasicBlock*), CmpSuccessors);

    // Create info
    ControlFlowInfo switchinfo(cfgStack_.length(), exitpc);
    if (!switches_.append(switchinfo))
        return ControlStatus_Error;

    // Use a state to retrieve some information
    CFGState state = CFGState::TableSwitch(exitpc, tableswitch);

    // Save the MIR instruction as last instruction of this block.
    current->end(tableswitch);

    // If there is only one successor the block should stop at the end of the switch
    // Else it should stop at the start of the next successor
    if (tableswitch->numBlocks() > 1)
        state.stopAt = tableswitch->getBlock(1)->pc();
    if (!setCurrentAndSpecializePhis(tableswitch->getBlock(0)))
        return ControlStatus_Error;

    if (!cfgStack_.append(state))
        return ControlStatus_Error;

    pc = current->pc();
    return ControlStatus_Jumped;
}

bool
IonBuilder::replaceTypeSet(MDefinition* subject, TemporaryTypeSet* type, MTest* test)
{
    if (type->unknown())
        return true;

    if (subject->resultTypeSet() && type->equals(subject->resultTypeSet()))
        return true;

    MInstruction* replace = nullptr;
    MDefinition* ins;

    for (uint32_t i = 0; i < current->stackDepth(); i++) {
        ins = current->getSlot(i);

        // Instead of creating a new MFilterTypeSet, try to update the old one.
        if (ins->isFilterTypeSet() && ins->getOperand(0) == subject &&
            ins->dependency() == test)
        {
            TemporaryTypeSet* intersect =
                TypeSet::intersectSets(ins->resultTypeSet(), type, alloc_->lifoAlloc());
            if (!intersect)
                return false;

            ins->toFilterTypeSet()->setResultType(intersect->getKnownMIRType());
            ins->toFilterTypeSet()->setResultTypeSet(intersect);

            if (ins->type() == MIRType_Undefined)
                current->setSlot(i, constant(UndefinedValue()));
            if (ins->type() == MIRType_Null)
                current->setSlot(i, constant(NullValue()));
            continue;
        }

        if (ins == subject) {
            if (!replace) {
                replace = MFilterTypeSet::New(alloc(), subject, type);
                if (!replace)
                    return false;

                current->add(replace);

                // Make sure we don't hoist it above the MTest, we can use the
                // 'dependency' of an MInstruction. This is normally used by
                // Alias Analysis, but won't get overwritten, since this
                // instruction doesn't have an AliasSet.
                replace->setDependency(test);

                if (replace->type() == MIRType_Undefined)
                    replace = constant(UndefinedValue());
                if (replace->type() == MIRType_Null)
                    replace = constant(NullValue());
            }
            current->setSlot(i, replace);
        }
    }
    return true;
}

bool
IonBuilder::detectAndOrStructure(MPhi* ins, bool* branchIsAnd)
{
    // Look for a triangle pattern:
    //
    //       initialBlock
    //         /     |
    // branchBlock   |
    //         \     |
    //        testBlock
    //
    // Where ins is a phi from testBlock which combines two values
    // pushed onto the stack by initialBlock and branchBlock.

    if (ins->numOperands() != 2)
        return false;

    MBasicBlock* testBlock = ins->block();
    MOZ_ASSERT(testBlock->numPredecessors() == 2);

    MBasicBlock* initialBlock;
    MBasicBlock* branchBlock;
    if (testBlock->getPredecessor(0)->lastIns()->isTest()) {
        initialBlock = testBlock->getPredecessor(0);
        branchBlock = testBlock->getPredecessor(1);
    } else if (testBlock->getPredecessor(1)->lastIns()->isTest()) {
        initialBlock = testBlock->getPredecessor(1);
        branchBlock = testBlock->getPredecessor(0);
    } else {
        return false;
    }

    if (branchBlock->numSuccessors() != 1)
        return false;

    if (branchBlock->numPredecessors() != 1 || branchBlock->getPredecessor(0) != initialBlock)
        return false;

    if (initialBlock->numSuccessors() != 2)
        return false;

    MDefinition* branchResult = ins->getOperand(testBlock->indexForPredecessor(branchBlock));
    MDefinition* initialResult = ins->getOperand(testBlock->indexForPredecessor(initialBlock));

    if (branchBlock->stackDepth() != initialBlock->stackDepth())
        return false;
    if (branchBlock->stackDepth() != testBlock->stackDepth() + 1)
        return false;
    if (branchResult != branchBlock->peek(-1) || initialResult != initialBlock->peek(-1))
        return false;

    MTest* initialTest = initialBlock->lastIns()->toTest();
    bool branchIsTrue = branchBlock == initialTest->ifTrue();
    if (initialTest->input() == ins->getOperand(0))
        *branchIsAnd = branchIsTrue != (testBlock->getPredecessor(0) == branchBlock);
    else if (initialTest->input() == ins->getOperand(1))
        *branchIsAnd = branchIsTrue != (testBlock->getPredecessor(1) == branchBlock);
    else
        return false;

    return true;
}

bool
IonBuilder::improveTypesAtCompare(MCompare* ins, bool trueBranch, MTest* test)
{
    if (ins->compareType() == MCompare::Compare_Undefined ||
        ins->compareType() == MCompare::Compare_Null)
    {
        return improveTypesAtNullOrUndefinedCompare(ins, trueBranch, test);
    }

    if ((ins->lhs()->isTypeOf() || ins->rhs()->isTypeOf()) &&
        (ins->lhs()->isConstantValue() || ins->rhs()->isConstantValue()))
    {
        return improveTypesAtTypeOfCompare(ins, trueBranch, test);
    }

    return true;
}

bool
IonBuilder::improveTypesAtTypeOfCompare(MCompare* ins, bool trueBranch, MTest* test)
{
    MTypeOf* typeOf = ins->lhs()->isTypeOf() ? ins->lhs()->toTypeOf() : ins->rhs()->toTypeOf();
    const Value* constant =
        ins->lhs()->isConstant() ? ins->lhs()->constantVp() : ins->rhs()->constantVp();

    if (!constant->isString())
        return true;

    bool equal = ins->jsop() == JSOP_EQ || ins->jsop() == JSOP_STRICTEQ;
    bool notEqual = ins->jsop() == JSOP_NE || ins->jsop() == JSOP_STRICTNE;

    if (notEqual)
        trueBranch = !trueBranch;

    // Relational compares not supported.
    if (!equal && !notEqual)
        return true;

    MDefinition* subject = typeOf->input();
    TemporaryTypeSet* inputTypes = subject->resultTypeSet();

    // Create temporary typeset equal to the type if there is no resultTypeSet.
    TemporaryTypeSet tmp;
    if (!inputTypes) {
        if (subject->type() == MIRType_Value)
            return true;
        inputTypes = &tmp;
        tmp.addType(TypeSet::PrimitiveType(ValueTypeFromMIRType(subject->type())), alloc_->lifoAlloc());
    }

    if (inputTypes->unknown())
        return true;

    // Note: we cannot remove the AnyObject type in the false branch,
    // since there are multiple ways to get an object. That is the reason
    // for the 'trueBranch' test.
    TemporaryTypeSet filter;
    const JSAtomState& names = GetJitContext()->runtime->names();
    if (constant->toString() == TypeName(JSTYPE_VOID, names)) {
        filter.addType(TypeSet::UndefinedType(), alloc_->lifoAlloc());
        if (typeOf->inputMaybeCallableOrEmulatesUndefined() && trueBranch)
            filter.addType(TypeSet::AnyObjectType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_BOOLEAN, names)) {
        filter.addType(TypeSet::BooleanType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_NUMBER, names)) {
        filter.addType(TypeSet::Int32Type(), alloc_->lifoAlloc());
        filter.addType(TypeSet::DoubleType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_STRING, names)) {
        filter.addType(TypeSet::StringType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_SYMBOL, names)) {
        filter.addType(TypeSet::SymbolType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_OBJECT, names)) {
        filter.addType(TypeSet::NullType(), alloc_->lifoAlloc());
        if (trueBranch)
            filter.addType(TypeSet::AnyObjectType(), alloc_->lifoAlloc());
    } else if (constant->toString() == TypeName(JSTYPE_FUNCTION, names)) {
        if (typeOf->inputMaybeCallableOrEmulatesUndefined() && trueBranch)
            filter.addType(TypeSet::AnyObjectType(), alloc_->lifoAlloc());
    } else {
        return true;
    }

    TemporaryTypeSet* type;
    if (trueBranch)
        type = TypeSet::intersectSets(&filter, inputTypes, alloc_->lifoAlloc());
    else
        type = TypeSet::removeSet(inputTypes, &filter, alloc_->lifoAlloc());

    if (!type)
        return false;

    return replaceTypeSet(subject, type, test);
}

bool
IonBuilder::improveTypesAtNullOrUndefinedCompare(MCompare* ins, bool trueBranch, MTest* test)
{
    MOZ_ASSERT(ins->compareType() == MCompare::Compare_Undefined ||
               ins->compareType() == MCompare::Compare_Null);

    // altersUndefined/Null represents if we can filter/set Undefined/Null.
    bool altersUndefined, altersNull;
    JSOp op = ins->jsop();

    switch(op) {
      case JSOP_STRICTNE:
      case JSOP_STRICTEQ:
        altersUndefined = ins->compareType() == MCompare::Compare_Undefined;
        altersNull = ins->compareType() == MCompare::Compare_Null;
        break;
      case JSOP_NE:
      case JSOP_EQ:
        altersUndefined = altersNull = true;
        break;
      default:
        MOZ_CRASH("Relational compares not supported");
    }

    MDefinition* subject = ins->lhs();
    TemporaryTypeSet* inputTypes = subject->resultTypeSet();

    MOZ_ASSERT(IsNullOrUndefined(ins->rhs()->type()));

    // Create temporary typeset equal to the type if there is no resultTypeSet.
    TemporaryTypeSet tmp;
    if (!inputTypes) {
        if (subject->type() == MIRType_Value)
            return true;
        inputTypes = &tmp;
        tmp.addType(TypeSet::PrimitiveType(ValueTypeFromMIRType(subject->type())), alloc_->lifoAlloc());
    }

    if (inputTypes->unknown())
        return true;

    TemporaryTypeSet* type;

    // Decide if we need to filter the type or set it.
    if ((op == JSOP_STRICTEQ || op == JSOP_EQ) ^ trueBranch) {
        // Remove undefined/null
        TemporaryTypeSet remove;
        if (altersUndefined)
            remove.addType(TypeSet::UndefinedType(), alloc_->lifoAlloc());
        if (altersNull)
            remove.addType(TypeSet::NullType(), alloc_->lifoAlloc());

        type = TypeSet::removeSet(inputTypes, &remove, alloc_->lifoAlloc());
    } else {
        // Set undefined/null.
        TemporaryTypeSet base;
        if (altersUndefined) {
            base.addType(TypeSet::UndefinedType(), alloc_->lifoAlloc());
            // If TypeSet emulates undefined, then we cannot filter the objects.
            if (inputTypes->maybeEmulatesUndefined(constraints()))
                base.addType(TypeSet::AnyObjectType(), alloc_->lifoAlloc());
        }

        if (altersNull)
            base.addType(TypeSet::NullType(), alloc_->lifoAlloc());

        type = TypeSet::intersectSets(&base, inputTypes, alloc_->lifoAlloc());
    }

    if (!type)
        return false;

    return replaceTypeSet(subject, type, test);
}

bool
IonBuilder::improveTypesAtTest(MDefinition* ins, bool trueBranch, MTest* test)
{
    // We explore the test condition to try and deduce as much type information
    // as possible.

    // All branches of this switch that don't want to fall through to the
    // default behavior must return.  The default behavior assumes that a true
    // test means the incoming ins is not null or undefined and that a false
    // tests means it's one of null, undefined, false, 0, "", and objects
    // emulating undefined
    switch (ins->op()) {
      case MDefinition::Op_Not:
        return improveTypesAtTest(ins->toNot()->getOperand(0), !trueBranch, test);
      case MDefinition::Op_IsObject: {
        MDefinition* subject = ins->getOperand(0);
        TemporaryTypeSet* oldType = subject->resultTypeSet();

        // Create temporary typeset equal to the type if there is no resultTypeSet.
        TemporaryTypeSet tmp;
        if (!oldType) {
            if (subject->type() == MIRType_Value)
                return true;
            oldType = &tmp;
            tmp.addType(TypeSet::PrimitiveType(ValueTypeFromMIRType(subject->type())), alloc_->lifoAlloc());
        }

        if (oldType->unknown())
            return true;

        TemporaryTypeSet* type = nullptr;
        if (trueBranch)
            type = oldType->cloneObjectsOnly(alloc_->lifoAlloc());
        else
            type = oldType->cloneWithoutObjects(alloc_->lifoAlloc());

        if (!type)
            return false;

        return replaceTypeSet(subject, type, test);
      }
      case MDefinition::Op_Phi: {
        bool branchIsAnd = true;
        if (!detectAndOrStructure(ins->toPhi(), &branchIsAnd)) {
            // Just fall through to the default behavior.
            break;
        }

        // Now we have detected the triangular structure and determined if it
        // was an AND or an OR.
        if (branchIsAnd) {
            if (trueBranch) {
                if (!improveTypesAtTest(ins->toPhi()->getOperand(0), true, test))
                    return false;
                if (!improveTypesAtTest(ins->toPhi()->getOperand(1), true, test))
                    return false;
            }
        } else {
            /*
             * if (a || b) {
             *    ...
             * } else {
             *    ...
             * }
             *
             * If we have a statements like the one described above,
             * And we are in the else branch of it. It amounts to:
             * if (!(a || b)) and being in the true branch.
             *
             * Simplifying, we have (!a && !b)
             * In this case we can use the same logic we use for branchIsAnd
             *
             */
            if (!trueBranch) {
                if (!improveTypesAtTest(ins->toPhi()->getOperand(0), false, test))
                    return false;
                if (!improveTypesAtTest(ins->toPhi()->getOperand(1), false, test))
                    return false;
            }
        }
        return true;
      }

      case MDefinition::Op_Compare:
        return improveTypesAtCompare(ins->toCompare(), trueBranch, test);

      default:
        break;
    }

    // By default MTest tests ToBoolean(input). As a result in the true branch we can filter
    // undefined and null. In false branch we can only encounter undefined, null, false, 0, ""
    // and objects that emulate undefined.

    TemporaryTypeSet* oldType = ins->resultTypeSet();
    TemporaryTypeSet* type;

    // Create temporary typeset equal to the type if there is no resultTypeSet.
    TemporaryTypeSet tmp;
    if (!oldType) {
        if (ins->type() == MIRType_Value)
            return true;
        oldType = &tmp;
        tmp.addType(TypeSet::PrimitiveType(ValueTypeFromMIRType(ins->type())), alloc_->lifoAlloc());
    }

    // If ins does not have a typeset we return as we cannot optimize.
    if (oldType->unknown())
        return true;

    // Decide either to set or remove.
    if (trueBranch) {
        TemporaryTypeSet remove;
        remove.addType(TypeSet::UndefinedType(), alloc_->lifoAlloc());
        remove.addType(TypeSet::NullType(), alloc_->lifoAlloc());
        type = TypeSet::removeSet(oldType, &remove, alloc_->lifoAlloc());
    } else {
        TemporaryTypeSet base;
        base.addType(TypeSet::UndefinedType(), alloc_->lifoAlloc()); // ToBoolean(undefined) == false
        base.addType(TypeSet::NullType(), alloc_->lifoAlloc()); // ToBoolean(null) == false
        base.addType(TypeSet::BooleanType(), alloc_->lifoAlloc()); // ToBoolean(false) == false
        base.addType(TypeSet::Int32Type(), alloc_->lifoAlloc()); // ToBoolean(0) == false
        base.addType(TypeSet::DoubleType(), alloc_->lifoAlloc()); // ToBoolean(0.0) == false
        base.addType(TypeSet::StringType(), alloc_->lifoAlloc()); // ToBoolean("") == false

        // If the typeset does emulate undefined, then we cannot filter out
        // objects.
        if (oldType->maybeEmulatesUndefined(constraints()))
            base.addType(TypeSet::AnyObjectType(), alloc_->lifoAlloc());

        type = TypeSet::intersectSets(&base, oldType, alloc_->lifoAlloc());
    }

    return type && replaceTypeSet(ins, type, test);
}

bool
IonBuilder::jsop_label()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_LABEL);

    jsbytecode* endpc = pc + GET_JUMP_OFFSET(pc);
    MOZ_ASSERT(endpc > pc);

    ControlFlowInfo label(cfgStack_.length(), endpc);
    if (!labels_.append(label))
        return false;

    return cfgStack_.append(CFGState::Label(endpc));
}

bool
IonBuilder::jsop_condswitch()
{
    // CondSwitch op looks as follows:
    //   condswitch [length +exit_pc; first case offset +next-case ]
    //   {
    //     {
    //       ... any code ...
    //       case (+jump) [pcdelta offset +next-case]
    //     }+
    //     default (+jump)
    //     ... jump targets ...
    //   }
    //
    // The default case is always emitted even if there is no default case in
    // the source.  The last case statement pcdelta source note might have a 0
    // offset on the last case (not all the time).
    //
    // A conditional evaluate the condition of each case and compare it to the
    // switch value with a strict equality.  Cases conditions are iterated
    // linearly until one is matching. If one case succeeds, the flow jumps into
    // the corresponding body block.  The body block might alias others and
    // might continue in the next body block if the body is not terminated with
    // a break.
    //
    // Algorithm:
    //  1/ Loop over the case chain to reach the default target
    //   & Estimate the number of uniq bodies.
    //  2/ Generate code for all cases (see processCondSwitchCase).
    //  3/ Generate code for all bodies (see processCondSwitchBody).

    MOZ_ASSERT(JSOp(*pc) == JSOP_CONDSWITCH);
    jssrcnote* sn = info().getNote(gsn, pc);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_CONDSWITCH);

    // Get the exit pc
    jsbytecode* exitpc = pc + GetSrcNoteOffset(sn, 0);
    jsbytecode* firstCase = pc + GetSrcNoteOffset(sn, 1);

    // Iterate all cases in the conditional switch.
    // - Stop at the default case. (always emitted after the last case)
    // - Estimate the number of uniq bodies. This estimation might be off by 1
    //   if the default body alias a case body.
    jsbytecode* curCase = firstCase;
    jsbytecode* lastTarget = GetJumpOffset(curCase) + curCase;
    size_t nbBodies = 2; // default target and the first body.

    MOZ_ASSERT(pc < curCase && curCase <= exitpc);
    while (JSOp(*curCase) == JSOP_CASE) {
        // Fetch the next case.
        jssrcnote* caseSn = info().getNote(gsn, curCase);
        MOZ_ASSERT(caseSn && SN_TYPE(caseSn) == SRC_NEXTCASE);
        ptrdiff_t off = GetSrcNoteOffset(caseSn, 0);
        curCase = off ? curCase + off : GetNextPc(curCase);
        MOZ_ASSERT(pc < curCase && curCase <= exitpc);

        // Count non-aliased cases.
        jsbytecode* curTarget = GetJumpOffset(curCase) + curCase;
        if (lastTarget < curTarget)
            nbBodies++;
        lastTarget = curTarget;
    }

    // The current case now be the default case which jump to the body of the
    // default case, which might be behind the last target.
    MOZ_ASSERT(JSOp(*curCase) == JSOP_DEFAULT);
    jsbytecode* defaultTarget = GetJumpOffset(curCase) + curCase;
    MOZ_ASSERT(curCase < defaultTarget && defaultTarget <= exitpc);

    // Allocate the current graph state.
    CFGState state = CFGState::CondSwitch(this, exitpc, defaultTarget);
    if (!state.condswitch.bodies || !state.condswitch.bodies->init(alloc(), nbBodies))
        return ControlStatus_Error;

    // We loop on case conditions with processCondSwitchCase.
    MOZ_ASSERT(JSOp(*firstCase) == JSOP_CASE);
    state.stopAt = firstCase;
    state.state = CFGState::COND_SWITCH_CASE;

    return cfgStack_.append(state);
}

IonBuilder::CFGState
IonBuilder::CFGState::CondSwitch(IonBuilder* builder, jsbytecode* exitpc, jsbytecode* defaultTarget)
{
    CFGState state;
    state.state = COND_SWITCH_CASE;
    state.stopAt = nullptr;
    state.condswitch.bodies = (FixedList<MBasicBlock*>*)builder->alloc_->allocate(
        sizeof(FixedList<MBasicBlock*>));
    state.condswitch.currentIdx = 0;
    state.condswitch.defaultTarget = defaultTarget;
    state.condswitch.defaultIdx = uint32_t(-1);
    state.condswitch.exitpc = exitpc;
    state.condswitch.breaks = nullptr;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::Label(jsbytecode* exitpc)
{
    CFGState state;
    state.state = LABEL;
    state.stopAt = exitpc;
    state.label.breaks = nullptr;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::Try(jsbytecode* exitpc, MBasicBlock* successor)
{
    CFGState state;
    state.state = TRY;
    state.stopAt = exitpc;
    state.try_.successor = successor;
    return state;
}

IonBuilder::ControlStatus
IonBuilder::processCondSwitchCase(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::COND_SWITCH_CASE);
    MOZ_ASSERT(!state.condswitch.breaks);
    MOZ_ASSERT(current);
    MOZ_ASSERT(JSOp(*pc) == JSOP_CASE);
    FixedList<MBasicBlock*>& bodies = *state.condswitch.bodies;
    jsbytecode* defaultTarget = state.condswitch.defaultTarget;
    uint32_t& currentIdx = state.condswitch.currentIdx;
    jsbytecode* lastTarget = currentIdx ? bodies[currentIdx - 1]->pc() : nullptr;

    // Fetch the following case in which we will continue.
    jssrcnote* sn = info().getNote(gsn, pc);
    ptrdiff_t off = GetSrcNoteOffset(sn, 0);
    jsbytecode* casePc = off ? pc + off : GetNextPc(pc);
    bool caseIsDefault = JSOp(*casePc) == JSOP_DEFAULT;
    MOZ_ASSERT(JSOp(*casePc) == JSOP_CASE || caseIsDefault);

    // Allocate the block of the matching case.
    bool bodyIsNew = false;
    MBasicBlock* bodyBlock = nullptr;
    jsbytecode* bodyTarget = pc + GetJumpOffset(pc);
    if (lastTarget < bodyTarget) {
        // If the default body is in the middle or aliasing the current target.
        if (lastTarget < defaultTarget && defaultTarget <= bodyTarget) {
            MOZ_ASSERT(state.condswitch.defaultIdx == uint32_t(-1));
            state.condswitch.defaultIdx = currentIdx;
            bodies[currentIdx] = nullptr;
            // If the default body does not alias any and it would be allocated
            // later and stored in the defaultIdx location.
            if (defaultTarget < bodyTarget)
                currentIdx++;
        }

        bodyIsNew = true;
        // Pop switch and case operands.
        bodyBlock = newBlockPopN(current, bodyTarget, 2);
        bodies[currentIdx++] = bodyBlock;
    } else {
        // This body alias the previous one.
        MOZ_ASSERT(lastTarget == bodyTarget);
        MOZ_ASSERT(currentIdx > 0);
        bodyBlock = bodies[currentIdx - 1];
    }

    if (!bodyBlock)
        return ControlStatus_Error;

    lastTarget = bodyTarget;

    // Allocate the block of the non-matching case.  This can either be a normal
    // case or the default case.
    bool caseIsNew = false;
    MBasicBlock* caseBlock = nullptr;
    if (!caseIsDefault) {
        caseIsNew = true;
        // Pop the case operand.
        caseBlock = newBlockPopN(current, GetNextPc(pc), 1);
    } else {
        // The non-matching case is the default case, which jump directly to its
        // body. Skip the creation of a default case block and directly create
        // the default body if it does not alias any previous body.

        if (state.condswitch.defaultIdx == uint32_t(-1)) {
            // The default target is the last target.
            MOZ_ASSERT(lastTarget < defaultTarget);
            state.condswitch.defaultIdx = currentIdx++;
            caseIsNew = true;
        } else if (bodies[state.condswitch.defaultIdx] == nullptr) {
            // The default target is in the middle and it does not alias any
            // case target.
            MOZ_ASSERT(defaultTarget < lastTarget);
            caseIsNew = true;
        } else {
            // The default target is in the middle and it alias a case target.
            MOZ_ASSERT(defaultTarget <= lastTarget);
            caseBlock = bodies[state.condswitch.defaultIdx];
        }

        // Allocate and register the default body.
        if (caseIsNew) {
            // Pop the case & switch operands.
            caseBlock = newBlockPopN(current, defaultTarget, 2);
            bodies[state.condswitch.defaultIdx] = caseBlock;
        }
    }

    if (!caseBlock)
        return ControlStatus_Error;

    // Terminate the last case condition block by emitting the code
    // corresponding to JSOP_CASE bytecode.
    if (bodyBlock != caseBlock) {
        MDefinition* caseOperand = current->pop();
        MDefinition* switchOperand = current->peek(-1);

        if (!jsop_compare(JSOP_STRICTEQ, switchOperand, caseOperand))
            return ControlStatus_Error;
        MInstruction* cmpResult = current->pop()->toInstruction();
        MOZ_ASSERT(!cmpResult->isEffectful());
        current->end(newTest(cmpResult, bodyBlock, caseBlock));

        // Add last case as predecessor of the body if the body is aliasing
        // the previous case body.
        if (!bodyIsNew && !bodyBlock->addPredecessorPopN(alloc(), current, 1))
            return ControlStatus_Error;

        // Add last case as predecessor of the non-matching case if the
        // non-matching case is an aliased default case. We need to pop the
        // switch operand as we skip the default case block and use the default
        // body block directly.
        MOZ_ASSERT_IF(!caseIsNew, caseIsDefault);
        if (!caseIsNew && !caseBlock->addPredecessorPopN(alloc(), current, 1))
            return ControlStatus_Error;
    } else {
        // The default case alias the last case body.
        MOZ_ASSERT(caseIsDefault);
        current->pop(); // Case operand
        current->pop(); // Switch operand
        current->end(MGoto::New(alloc(), bodyBlock));
        if (!bodyIsNew && !bodyBlock->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    }

    if (caseIsDefault) {
        // The last case condition is finished.  Loop in processCondSwitchBody,
        // with potential stops in processSwitchBreak.  Check that the bodies
        // fixed list is over-estimate by at most 1, and shrink the size such as
        // length can be used as an upper bound while iterating bodies.
        MOZ_ASSERT(currentIdx == bodies.length() || currentIdx + 1 == bodies.length());
        bodies.shrink(bodies.length() - currentIdx);

        // Handle break statements in processSwitchBreak while processing
        // bodies.
        ControlFlowInfo breakInfo(cfgStack_.length() - 1, state.condswitch.exitpc);
        if (!switches_.append(breakInfo))
            return ControlStatus_Error;

        // Jump into the first body.
        currentIdx = 0;
        setCurrent(nullptr);
        state.state = CFGState::COND_SWITCH_BODY;
        return processCondSwitchBody(state);
    }

    // Continue until the case condition.
    if (!setCurrentAndSpecializePhis(caseBlock))
        return ControlStatus_Error;
    pc = current->pc();
    state.stopAt = casePc;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processCondSwitchBody(CFGState& state)
{
    MOZ_ASSERT(state.state == CFGState::COND_SWITCH_BODY);
    MOZ_ASSERT(pc <= state.condswitch.exitpc);
    FixedList<MBasicBlock*>& bodies = *state.condswitch.bodies;
    uint32_t& currentIdx = state.condswitch.currentIdx;

    MOZ_ASSERT(currentIdx <= bodies.length());
    if (currentIdx == bodies.length()) {
        MOZ_ASSERT_IF(current, pc == state.condswitch.exitpc);
        return processSwitchEnd(state.condswitch.breaks, state.condswitch.exitpc);
    }

    // Get the next body
    MBasicBlock* nextBody = bodies[currentIdx++];
    MOZ_ASSERT_IF(current, pc == nextBody->pc());

    // Fix the reverse post-order iteration.
    graph().moveBlockToEnd(nextBody);

    // The last body continue into the new one.
    if (current) {
        current->end(MGoto::New(alloc(), nextBody));
        if (!nextBody->addPredecessor(alloc(), current))
            return ControlStatus_Error;
    }

    // Continue in the next body.
    if (!setCurrentAndSpecializePhis(nextBody))
        return ControlStatus_Error;
    pc = current->pc();

    if (currentIdx < bodies.length())
        state.stopAt = bodies[currentIdx]->pc();
    else
        state.stopAt = state.condswitch.exitpc;
    return ControlStatus_Jumped;
}

bool
IonBuilder::jsop_andor(JSOp op)
{
    MOZ_ASSERT(op == JSOP_AND || op == JSOP_OR);

    jsbytecode* rhsStart = pc + CodeSpec[op].length;
    jsbytecode* joinStart = pc + GetJumpOffset(pc);
    MOZ_ASSERT(joinStart > pc);

    // We have to leave the LHS on the stack.
    MDefinition* lhs = current->peek(-1);

    MBasicBlock* evalLhs = newBlock(current, joinStart);
    MBasicBlock* evalRhs = newBlock(current, rhsStart);
    if (!evalLhs || !evalRhs)
        return false;

    MTest* test = (op == JSOP_AND)
                  ? newTest(lhs, evalRhs, evalLhs)
                  : newTest(lhs, evalLhs, evalRhs);
    current->end(test);

    // Create the lhs block and specialize.
    if (!setCurrentAndSpecializePhis(evalLhs))
        return false;

    if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
        return false;

    // Create the rhs block.
    if (!cfgStack_.append(CFGState::AndOr(joinStart, evalLhs)))
        return false;

    if (!setCurrentAndSpecializePhis(evalRhs))
        return false;

    if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
        return false;

    return true;
}

bool
IonBuilder::jsop_dup2()
{
    uint32_t lhsSlot = current->stackDepth() - 2;
    uint32_t rhsSlot = current->stackDepth() - 1;
    current->pushSlot(lhsSlot);
    current->pushSlot(rhsSlot);
    return true;
}

bool
IonBuilder::jsop_loophead(jsbytecode* pc)
{
    assertValidLoopHeadOp(pc);

    current->add(MInterruptCheck::New(alloc()));
    insertRecompileCheck();

    return true;
}

bool
IonBuilder::jsop_ifeq(JSOp op)
{
    // IFEQ always has a forward offset.
    jsbytecode* trueStart = pc + CodeSpec[op].length;
    jsbytecode* falseStart = pc + GetJumpOffset(pc);
    MOZ_ASSERT(falseStart > pc);

    // We only handle cases that emit source notes.
    jssrcnote* sn = info().getNote(gsn, pc);
    if (!sn)
        return abort("expected sourcenote");

    MDefinition* ins = current->pop();

    // Create true and false branches.
    MBasicBlock* ifTrue = newBlock(current, trueStart);
    MBasicBlock* ifFalse = newBlock(current, falseStart);
    if (!ifTrue || !ifFalse)
        return false;

    MTest* test = newTest(ins, ifTrue, ifFalse);
    current->end(test);

    // The bytecode for if/ternary gets emitted either like this:
    //
    //    IFEQ X  ; src note (IF_ELSE, COND) points to the GOTO
    //    ...
    //    GOTO Z
    // X: ...     ; else/else if
    //    ...
    // Z:         ; join
    //
    // Or like this:
    //
    //    IFEQ X  ; src note (IF) has no offset
    //    ...
    // Z: ...     ; join
    //
    // We want to parse the bytecode as if we were parsing the AST, so for the
    // IF_ELSE/COND cases, we use the source note and follow the GOTO. For the
    // IF case, the IFEQ offset is the join point.
    switch (SN_TYPE(sn)) {
      case SRC_IF:
        if (!cfgStack_.append(CFGState::If(falseStart, test)))
            return false;
        break;

      case SRC_IF_ELSE:
      case SRC_COND:
      {
        // Infer the join point from the JSOP_GOTO[X] sitting here, then
        // assert as we much we can that this is the right GOTO.
        jsbytecode* trueEnd = pc + GetSrcNoteOffset(sn, 0);
        MOZ_ASSERT(trueEnd > pc);
        MOZ_ASSERT(trueEnd < falseStart);
        MOZ_ASSERT(JSOp(*trueEnd) == JSOP_GOTO);
        MOZ_ASSERT(!info().getNote(gsn, trueEnd));

        jsbytecode* falseEnd = trueEnd + GetJumpOffset(trueEnd);
        MOZ_ASSERT(falseEnd > trueEnd);
        MOZ_ASSERT(falseEnd >= falseStart);

        if (!cfgStack_.append(CFGState::IfElse(trueEnd, falseEnd, test)))
            return false;
        break;
      }

      default:
        MOZ_CRASH("unexpected source note type");
    }

    // Switch to parsing the true branch. Note that no PC update is needed,
    // it's the next instruction.
    if (!setCurrentAndSpecializePhis(ifTrue))
        return false;

    // Filter the types in the true branch.
    if (!improveTypesAtTest(test->getOperand(0), test->ifTrue() == current, test))
        return false;

    return true;
}

bool
IonBuilder::jsop_try()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_TRY);

    // Try-finally is not yet supported.
    if (analysis().hasTryFinally())
        return abort("Has try-finally");

    // Try-catch within inline frames is not yet supported.
    MOZ_ASSERT(!isInlineBuilder());

    // Try-catch during the arguments usage analysis is not yet supported. Code
    // accessing the arguments within the 'catch' block is not accounted for.
    if (info().analysisMode() == Analysis_ArgumentsUsage)
        return abort("Try-catch during arguments usage analysis");

    graph().setHasTryBlock();

    jssrcnote* sn = info().getNote(gsn, pc);
    MOZ_ASSERT(SN_TYPE(sn) == SRC_TRY);

    // Get the pc of the last instruction in the try block. It's a JSOP_GOTO to
    // jump over the catch block.
    jsbytecode* endpc = pc + GetSrcNoteOffset(sn, 0);
    MOZ_ASSERT(JSOp(*endpc) == JSOP_GOTO);
    MOZ_ASSERT(GetJumpOffset(endpc) > 0);

    jsbytecode* afterTry = endpc + GetJumpOffset(endpc);

    // If controlflow in the try body is terminated (by a return or throw
    // statement), the code after the try-statement may still be reachable
    // via the catch block (which we don't compile) and OSR can enter it.
    // For example:
    //
    //     try {
    //         throw 3;
    //     } catch(e) { }
    //
    //     for (var i=0; i<1000; i++) {}
    //
    // To handle this, we create two blocks: one for the try block and one
    // for the code following the try-catch statement. Both blocks are
    // connected to the graph with an MGotoWithFake instruction that always
    // jumps to the try block. This ensures the successor block always has a
    // predecessor.
    //
    // If the code after the try block is unreachable (control flow in both the
    // try and catch blocks is terminated), only create the try block, to avoid
    // parsing unreachable code.

    MBasicBlock* tryBlock = newBlock(current, GetNextPc(pc));
    if (!tryBlock)
        return false;

    MBasicBlock* successor;
    if (analysis().maybeInfo(afterTry)) {
        successor = newBlock(current, afterTry);
        if (!successor)
            return false;

        current->end(MGotoWithFake::New(alloc(), tryBlock, successor));
    } else {
        successor = nullptr;
        current->end(MGoto::New(alloc(), tryBlock));
    }

    if (!cfgStack_.append(CFGState::Try(endpc, successor)))
        return false;

    // The baseline compiler should not attempt to enter the catch block
    // via OSR.
    MOZ_ASSERT(info().osrPc() < endpc || info().osrPc() >= afterTry);

    // Start parsing the try block.
    return setCurrentAndSpecializePhis(tryBlock);
}

IonBuilder::ControlStatus
IonBuilder::processReturn(JSOp op)
{
    MDefinition* def;
    switch (op) {
      case JSOP_RETURN:
        // Return the last instruction.
        def = current->pop();
        break;

      case JSOP_RETRVAL:
        // Return undefined eagerly if script doesn't use return value.
        if (script()->noScriptRval()) {
            MInstruction* ins = MConstant::New(alloc(), UndefinedValue());
            current->add(ins);
            def = ins;
            break;
        }

        def = current->getSlot(info().returnValueSlot());
        break;

      default:
        def = nullptr;
        MOZ_CRASH("unknown return op");
    }

    MReturn* ret = MReturn::New(alloc(), def);
    current->end(ret);

    if (!graph().addReturn(current))
        return ControlStatus_Error;

    // Make sure no one tries to use this block now.
    setCurrent(nullptr);
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processThrow()
{
    MDefinition* def = current->pop();

    // MThrow is not marked as effectful. This means when it throws and we
    // are inside a try block, we could use an earlier resume point and this
    // resume point may not be up-to-date, for example:
    //
    // (function() {
    //     try {
    //         var x = 1;
    //         foo(); // resume point
    //         x = 2;
    //         throw foo;
    //     } catch(e) {
    //         print(x);
    //     }
    // ])();
    //
    // If we use the resume point after the call, this will print 1 instead
    // of 2. To fix this, we create a resume point right before the MThrow.
    //
    // Note that this is not a problem for instructions other than MThrow
    // because they are either marked as effectful (have their own resume
    // point) or cannot throw a catchable exception.
    //
    // We always install this resume point (instead of only when the function
    // has a try block) in order to handle the Debugger onExceptionUnwind
    // hook. When we need to handle the hook, we bail out to baseline right
    // after the throw and propagate the exception when debug mode is on. This
    // is opposed to the normal behavior of resuming directly in the
    // associated catch block.
    MNop* nop = MNop::New(alloc());
    current->add(nop);

    if (!resumeAfter(nop))
        return ControlStatus_Error;

    MThrow* ins = MThrow::New(alloc(), def);
    current->end(ins);

    // Make sure no one tries to use this block now.
    setCurrent(nullptr);
    return processControlEnd();
}

bool
IonBuilder::pushConstant(const Value& v)
{
    current->push(constant(v));
    return true;
}

bool
IonBuilder::bitnotTrySpecialized(bool* emitted, MDefinition* input)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a specialized bitnot instruction based on the input type
    // of the operand.

    if (input->mightBeType(MIRType_Object) || input->mightBeType(MIRType_Symbol))
        return true;

    MBitNot* ins = MBitNot::New(alloc(), input);
    ins->setSpecialization(MIRType_Int32);

    current->add(ins);
    current->push(ins);

    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_bitnot()
{
    bool emitted = false;

    MDefinition* input = current->pop();

    if (!forceInlineCaches()) {
        if (!bitnotTrySpecialized(&emitted, input) || emitted)
            return emitted;
    }

    if (!arithTrySharedStub(&emitted, JSOP_BITNOT, nullptr, input) || emitted)
        return emitted;

    // Not possible to optimize. Do a slow vm call.
    MBitNot* ins = MBitNot::New(alloc(), input);

    current->add(ins);
    current->push(ins);
    MOZ_ASSERT(ins->isEffectful());
    return resumeAfter(ins);
}

bool
IonBuilder::jsop_bitop(JSOp op)
{
    // Pop inputs.
    MDefinition* right = current->pop();
    MDefinition* left = current->pop();

    MBinaryBitwiseInstruction* ins;
    switch (op) {
      case JSOP_BITAND:
        ins = MBitAnd::New(alloc(), left, right);
        break;

      case JSOP_BITOR:
        ins = MBitOr::New(alloc(), left, right);
        break;

      case JSOP_BITXOR:
        ins = MBitXor::New(alloc(), left, right);
        break;

      case JSOP_LSH:
        ins = MLsh::New(alloc(), left, right);
        break;

      case JSOP_RSH:
        ins = MRsh::New(alloc(), left, right);
        break;

      case JSOP_URSH:
        ins = MUrsh::New(alloc(), left, right);
        break;

      default:
        MOZ_CRASH("unexpected bitop");
    }

    current->add(ins);
    ins->infer(inspector, pc);

    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;

    return true;
}

MDefinition::Opcode
JSOpToMDefinition(JSOp op)
{
    switch (op) {
      case JSOP_ADD:
        return MDefinition::Op_Add;
      case JSOP_SUB:
        return MDefinition::Op_Sub;
      case JSOP_MUL:
        return MDefinition::Op_Mul;
      case JSOP_DIV:
        return MDefinition::Op_Div;
      case JSOP_MOD:
        return MDefinition::Op_Mod;
      default:
        MOZ_CRASH("unexpected binary opcode");
    }
}

bool
IonBuilder::binaryArithTryConcat(bool* emitted, JSOp op, MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to convert an addition into a concat operation if the inputs
    // indicate this might be a concatenation.

    // Only try to replace this with concat when we have an addition.
    if (op != JSOP_ADD)
        return true;

    // Make sure one of the inputs is a string.
    if (left->type() != MIRType_String && right->type() != MIRType_String)
        return true;

    // The none-string input (if present) should be atleast a numerical type.
    // Which we can easily coerce to string.
    if (right->type() != MIRType_String && !IsNumberType(right->type()))
        return true;
    if (left->type() != MIRType_String && !IsNumberType(left->type()))
        return true;

    MConcat* ins = MConcat::New(alloc(), left, right);
    current->add(ins);
    current->push(ins);

    if (!maybeInsertResume())
        return false;

    *emitted = true;
    return true;
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

bool
IonBuilder::binaryArithTrySpecialized(bool* emitted, JSOp op, MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a specialized binary instruction based on the input types
    // of the operands.

    // Anything complex - strings, symbols, and objects - are not specialized
    if (!SimpleArithOperand(left) || !SimpleArithOperand(right))
        return true;

    // One of the inputs need to be a number.
    if (!IsNumberType(left->type()) && !IsNumberType(right->type()))
        return true;

    MDefinition::Opcode defOp = JSOpToMDefinition(op);
    MBinaryArithInstruction* ins = MBinaryArithInstruction::New(alloc(), defOp, left, right);
    ins->setNumberSpecialization(alloc(), inspector, pc);

    if (op == JSOP_ADD || op == JSOP_MUL)
        ins->setCommutative();

    current->add(ins);
    current->push(ins);

    MOZ_ASSERT(!ins->isEffectful());
    if (!maybeInsertResume())
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::binaryArithTrySpecializedOnBaselineInspector(bool* emitted, JSOp op,
                                                         MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a specialized binary instruction speculating the
    // type using the baseline caches.

    MIRType specialization = inspector->expectedBinaryArithSpecialization(pc);
    if (specialization == MIRType_None)
        return true;

    MDefinition::Opcode def_op = JSOpToMDefinition(op);
    MBinaryArithInstruction* ins = MBinaryArithInstruction::New(alloc(), def_op, left, right);
    ins->setSpecialization(specialization);

    current->add(ins);
    current->push(ins);

    MOZ_ASSERT(!ins->isEffectful());
    if (!maybeInsertResume())
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::arithTrySharedStub(bool* emitted, JSOp op,
                               MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);
    JSOp actualOp = JSOp(*pc);

    // Try to emit a shared stub cache.

    if (JitOptions.disableSharedStubs)
        return true;

    // The actual jsop 'jsop_pos' is not supported yet.
    if (actualOp == JSOP_POS)
        return true;

    MInstruction* stub = nullptr;
    switch (actualOp) {
      case JSOP_NEG:
      case JSOP_BITNOT:
        MOZ_ASSERT_IF(op == JSOP_MUL, left->isConstantValue() &&
                                      left->constantValue().toInt32() == -1);
        MOZ_ASSERT_IF(op != JSOP_MUL, !left);

        stub = MUnarySharedStub::New(alloc(), right);
        break;
      case JSOP_ADD:
      case JSOP_SUB:
      case JSOP_MUL:
      case JSOP_DIV:
      case JSOP_MOD:
        stub = MBinarySharedStub::New(alloc(), left, right);
        break;
      default:
        MOZ_CRASH("unsupported arith");
    }

    current->add(stub);
    current->push(stub);

    // Decrease type from 'any type' to 'empty type' when one of the operands
    // is 'empty typed'.
    maybeMarkEmpty(stub);

    if (!resumeAfter(stub))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_binary_arith(JSOp op, MDefinition* left, MDefinition* right)
{
    bool emitted = false;

    if (!forceInlineCaches()) {
        if (!binaryArithTryConcat(&emitted, op, left, right) || emitted)
            return emitted;

        if (!binaryArithTrySpecialized(&emitted, op, left, right) || emitted)
            return emitted;

        if (!binaryArithTrySpecializedOnBaselineInspector(&emitted, op, left, right) || emitted)
            return emitted;
    }

    if (!arithTrySharedStub(&emitted, op, left, right) || emitted)
        return emitted;

    // Not possible to optimize. Do a slow vm call.
    MDefinition::Opcode def_op = JSOpToMDefinition(op);
    MBinaryArithInstruction* ins = MBinaryArithInstruction::New(alloc(), def_op, left, right);

    // Decrease type from 'any type' to 'empty type' when one of the operands
    // is 'empty typed'.
    maybeMarkEmpty(ins);

    current->add(ins);
    current->push(ins);
    MOZ_ASSERT(ins->isEffectful());
    return resumeAfter(ins);
}

bool
IonBuilder::jsop_binary_arith(JSOp op)
{
    MDefinition* right = current->pop();
    MDefinition* left = current->pop();

    return jsop_binary_arith(op, left, right);
}

bool
IonBuilder::jsop_pow()
{
    MDefinition* exponent = current->pop();
    MDefinition* base = current->pop();

    if (inlineMathPowHelper(base, exponent, MIRType_Double) == InliningStatus_Inlined) {
        base->setImplicitlyUsedUnchecked();
        exponent->setImplicitlyUsedUnchecked();
        return true;
    }

    // For now, use MIRType_Double, as a safe cover-all. See bug 1188079.
    MPow* pow = MPow::New(alloc(), base, exponent, MIRType_Double);
    current->add(pow);
    current->push(pow);
    return true;
}

bool
IonBuilder::jsop_pos()
{
    if (IsNumberType(current->peek(-1)->type())) {
        // Already int32 or double. Set the operand as implicitly used so it
        // doesn't get optimized out if it has no other uses, as we could bail
        // out.
        current->peek(-1)->setImplicitlyUsedUnchecked();
        return true;
    }

    // Compile +x as x * 1.
    MDefinition* value = current->pop();
    MConstant* one = MConstant::New(alloc(), Int32Value(1));
    current->add(one);

    return jsop_binary_arith(JSOP_MUL, value, one);
}

bool
IonBuilder::jsop_neg()
{
    // Since JSOP_NEG does not use a slot, we cannot push the MConstant.
    // The MConstant is therefore passed to JSOP_MUL without slot traffic.
    MConstant* negator = MConstant::New(alloc(), Int32Value(-1));
    current->add(negator);

    MDefinition* right = current->pop();

    return jsop_binary_arith(JSOP_MUL, negator, right);
}

bool
IonBuilder::jsop_tostring()
{
    if (current->peek(-1)->type() == MIRType_String)
        return true;

    MDefinition* value = current->pop();
    MToString* ins = MToString::New(alloc(), value);
    current->add(ins);
    current->push(ins);
    MOZ_ASSERT(!ins->isEffectful());
    return true;
}

class AutoAccumulateReturns
{
    MIRGraph& graph_;
    MIRGraphReturns* prev_;

  public:
    AutoAccumulateReturns(MIRGraph& graph, MIRGraphReturns& returns)
      : graph_(graph)
    {
        prev_ = graph_.returnAccumulator();
        graph_.setReturnAccumulator(&returns);
    }
    ~AutoAccumulateReturns() {
        graph_.setReturnAccumulator(prev_);
    }
};

bool
IonBuilder::inlineScriptedCall(CallInfo& callInfo, JSFunction* target)
{
    MOZ_ASSERT(target->hasScript());
    MOZ_ASSERT(IsIonInlinablePC(pc));

    callInfo.setImplicitlyUsedUnchecked();

    // Ensure sufficient space in the slots: needed for inlining from FUNAPPLY.
    uint32_t depth = current->stackDepth() + callInfo.numFormals();
    if (depth > current->nslots()) {
        if (!current->increaseSlots(depth - current->nslots()))
            return false;
    }

    // Create new |this| on the caller-side for inlined constructors.
    if (callInfo.constructing()) {
        MDefinition* thisDefn = createThis(target, callInfo.fun(), callInfo.getNewTarget());
        if (!thisDefn)
            return false;
        callInfo.setThis(thisDefn);
    }

    // Capture formals in the outer resume point.
    callInfo.pushFormals(current);

    MResumePoint* outerResumePoint =
        MResumePoint::New(alloc(), current, pc, MResumePoint::Outer);
    if (!outerResumePoint)
        return false;
    current->setOuterResumePoint(outerResumePoint);

    // Pop formals again, except leave |fun| on stack for duration of call.
    callInfo.popFormals(current);
    current->push(callInfo.fun());

    JSScript* calleeScript = target->nonLazyScript();
    BaselineInspector inspector(calleeScript);

    // Improve type information of |this| when not set.
    if (callInfo.constructing() &&
        !callInfo.thisArg()->resultTypeSet())
    {
        StackTypeSet* types = TypeScript::ThisTypes(calleeScript);
        if (types && !types->unknown()) {
            TemporaryTypeSet* clonedTypes = types->clone(alloc_->lifoAlloc());
            if (!clonedTypes)
                return oom();
            MTypeBarrier* barrier = MTypeBarrier::New(alloc(), callInfo.thisArg(), clonedTypes);
            current->add(barrier);
            if (barrier->type() == MIRType_Undefined)
                callInfo.setThis(constant(UndefinedValue()));
            else if (barrier->type() == MIRType_Null)
                callInfo.setThis(constant(NullValue()));
            else
                callInfo.setThis(barrier);
        }
    }

    // Start inlining.
    LifoAlloc* lifoAlloc = alloc_->lifoAlloc();
    InlineScriptTree* inlineScriptTree =
        info().inlineScriptTree()->addCallee(alloc_, pc, calleeScript);
    if (!inlineScriptTree)
        return false;
    CompileInfo* info = lifoAlloc->new_<CompileInfo>(calleeScript, target,
                                                     (jsbytecode*)nullptr, callInfo.constructing(),
                                                     this->info().analysisMode(),
                                                     /* needsArgsObj = */ false,
                                                     inlineScriptTree);
    if (!info)
        return false;

    MIRGraphReturns returns(alloc());
    AutoAccumulateReturns aar(graph(), returns);

    // Build the graph.
    IonBuilder inlineBuilder(analysisContext, compartment, options, &alloc(), &graph(), constraints(),
                             &inspector, info, &optimizationInfo(), nullptr, inliningDepth_ + 1,
                             loopDepth_);
    if (!inlineBuilder.buildInline(this, outerResumePoint, callInfo)) {
        if (analysisContext && analysisContext->isExceptionPending()) {
            JitSpew(JitSpew_IonAbort, "Inline builder raised exception.");
            abortReason_ = AbortReason_Error;
            return false;
        }

        // Inlining the callee failed. Mark the callee as uninlineable only if
        // the inlining was aborted for a non-exception reason.
        if (inlineBuilder.abortReason_ == AbortReason_Disable) {
            calleeScript->setUninlineable();
            abortReason_ = AbortReason_Inlining;
        } else if (inlineBuilder.abortReason_ == AbortReason_Inlining) {
            abortReason_ = AbortReason_Inlining;
        } else if (inlineBuilder.abortReason_ == AbortReason_PreliminaryObjects) {
            const ObjectGroupVector& groups = inlineBuilder.abortedPreliminaryGroups();
            MOZ_ASSERT(!groups.empty());
            for (size_t i = 0; i < groups.length(); i++)
                addAbortedPreliminaryGroup(groups[i]);
            abortReason_ = AbortReason_PreliminaryObjects;
        }

        return false;
    }

    // Create return block.
    jsbytecode* postCall = GetNextPc(pc);
    MBasicBlock* returnBlock = newBlock(nullptr, postCall);
    if (!returnBlock)
        return false;
    returnBlock->setCallerResumePoint(callerResumePoint_);

    // Inherit the slots from current and pop |fun|.
    returnBlock->inheritSlots(current);
    returnBlock->pop();

    // Accumulate return values.
    if (returns.empty()) {
        // Inlining of functions that have no exit is not supported.
        calleeScript->setUninlineable();
        abortReason_ = AbortReason_Inlining;
        return false;
    }
    MDefinition* retvalDefn = patchInlinedReturns(callInfo, returns, returnBlock);
    if (!retvalDefn)
        return false;
    returnBlock->push(retvalDefn);

    // Initialize entry slots now that the stack has been fixed up.
    if (!returnBlock->initEntrySlots(alloc()))
        return false;

    return setCurrentAndSpecializePhis(returnBlock);
}

MDefinition*
IonBuilder::patchInlinedReturn(CallInfo& callInfo, MBasicBlock* exit, MBasicBlock* bottom)
{
    // Replaces the MReturn in the exit block with an MGoto.
    MDefinition* rdef = exit->lastIns()->toReturn()->input();
    exit->discardLastIns();

    // Constructors must be patched by the caller to always return an object.
    if (callInfo.constructing()) {
        if (rdef->type() == MIRType_Value) {
            // Unknown return: dynamically detect objects.
            MReturnFromCtor* filter = MReturnFromCtor::New(alloc(), rdef, callInfo.thisArg());
            exit->add(filter);
            rdef = filter;
        } else if (rdef->type() != MIRType_Object) {
            // Known non-object return: force |this|.
            rdef = callInfo.thisArg();
        }
    } else if (callInfo.isSetter()) {
        // Setters return their argument, not whatever value is returned.
        rdef = callInfo.getArg(0);
    }

    if (!callInfo.isSetter())
        rdef = specializeInlinedReturn(rdef, exit);

    MGoto* replacement = MGoto::New(alloc(), bottom);
    exit->end(replacement);
    if (!bottom->addPredecessorWithoutPhis(exit))
        return nullptr;

    return rdef;
}

MDefinition*
IonBuilder::specializeInlinedReturn(MDefinition* rdef, MBasicBlock* exit)
{
    // Remove types from the return definition that weren't observed.
    TemporaryTypeSet* types = bytecodeTypes(pc);

    // The observed typeset doesn't contain extra information.
    if (types->empty() || types->unknown())
        return rdef;

    // Decide if specializing is needed using the result typeset if available,
    // else use the result type.

    if (rdef->resultTypeSet()) {
        // Don't specialize if return typeset is a subset of the
        // observed typeset. The return typeset is already more specific.
        if (rdef->resultTypeSet()->isSubset(types))
            return rdef;
    } else {
        MIRType observedType = types->getKnownMIRType();

        // Don't specialize if type is MIRType_Float32 and TI reports
        // MIRType_Double. Float is more specific than double.
        if (observedType == MIRType_Double && rdef->type() == MIRType_Float32)
            return rdef;

        // Don't specialize if types are inaccordance, except for MIRType_Value
        // and MIRType_Object (when not unknown object), since the typeset
        // contains more specific information.
        if (observedType == rdef->type() &&
            observedType != MIRType_Value &&
            (observedType != MIRType_Object || types->unknownObject()))
        {
            return rdef;
        }
    }

    setCurrent(exit);

    MTypeBarrier* barrier = nullptr;
    rdef = addTypeBarrier(rdef, types, BarrierKind::TypeSet, &barrier);
    if (barrier)
        barrier->setNotMovable();

    return rdef;
}

MDefinition*
IonBuilder::patchInlinedReturns(CallInfo& callInfo, MIRGraphReturns& returns, MBasicBlock* bottom)
{
    // Replaces MReturns with MGotos, returning the MDefinition
    // representing the return value, or nullptr.
    MOZ_ASSERT(returns.length() > 0);

    if (returns.length() == 1)
        return patchInlinedReturn(callInfo, returns[0], bottom);

    // Accumulate multiple returns with a phi.
    MPhi* phi = MPhi::New(alloc());
    if (!phi->reserveLength(returns.length()))
        return nullptr;

    for (size_t i = 0; i < returns.length(); i++) {
        MDefinition* rdef = patchInlinedReturn(callInfo, returns[i], bottom);
        if (!rdef)
            return nullptr;
        phi->addInput(rdef);
    }

    bottom->addPhi(phi);
    return phi;
}

IonBuilder::InliningDecision
IonBuilder::makeInliningDecision(JSObject* targetArg, CallInfo& callInfo)
{
    // When there is no target, inlining is impossible.
    if (targetArg == nullptr) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNoTarget);
        return InliningDecision_DontInline;
    }

    // Inlining non-function targets is handled by inlineNonFunctionCall().
    if (!targetArg->is<JSFunction>())
        return InliningDecision_Inline;

    JSFunction* target = &targetArg->as<JSFunction>();

    // Never inline during the arguments usage analysis.
    if (info().analysisMode() == Analysis_ArgumentsUsage)
        return InliningDecision_DontInline;

    // Native functions provide their own detection in inlineNativeCall().
    if (target->isNative())
        return InliningDecision_Inline;

    // Determine whether inlining is possible at callee site
    InliningDecision decision = canInlineTarget(target, callInfo);
    if (decision != InliningDecision_Inline)
        return decision;

    // Heuristics!
    JSScript* targetScript = target->nonLazyScript();

    // Callee must not be excessively large.
    // This heuristic also applies to the callsite as a whole.
    bool offThread = options.offThreadCompilationAvailable();
    if (targetScript->length() > optimizationInfo().inlineMaxBytecodePerCallSite(offThread)) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineBigCallee);
        return DontInline(targetScript, "Vetoed: callee excessively large");
    }

    // Callee must have been called a few times to have somewhat stable
    // type information, except for definite properties analysis,
    // as the caller has not run yet.
    if (targetScript->getWarmUpCount() < optimizationInfo().inliningWarmUpThreshold() &&
        !targetScript->baselineScript()->ionCompiledOrInlined() &&
        info().analysisMode() != Analysis_DefiniteProperties)
    {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNotHot);
        JitSpew(JitSpew_Inlining, "Cannot inline %s:%" PRIuSIZE ": callee is insufficiently hot.",
                targetScript->filename(), targetScript->lineno());
        return InliningDecision_WarmUpCountTooLow;
    }

    // Don't inline if the callee is known to inline a lot of code, to avoid
    // huge MIR graphs.
    uint32_t inlinedBytecodeLength = targetScript->baselineScript()->inlinedBytecodeLength();
    if (inlinedBytecodeLength > optimizationInfo().inlineMaxCalleeInlinedBytecodeLength()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineBigCalleeInlinedBytecodeLength);
        return DontInline(targetScript, "Vetoed: callee inlinedBytecodeLength is too big");
    }

    IonBuilder* outerBuilder = outermostBuilder();

    // Cap the total bytecode length we inline under a single script, to avoid
    // excessive inlining in pathological cases.
    size_t totalBytecodeLength = outerBuilder->inlinedBytecodeLength_ + targetScript->length();
    if (totalBytecodeLength > optimizationInfo().inlineMaxTotalBytecodeLength()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineExceededTotalBytecodeLength);
        return DontInline(targetScript, "Vetoed: exceeding max total bytecode length");
    }

    // Cap the inlining depth.

    uint32_t maxInlineDepth;
    if (JitOptions.isSmallFunction(targetScript)) {
        maxInlineDepth = optimizationInfo().smallFunctionMaxInlineDepth();
    } else {
        maxInlineDepth = optimizationInfo().maxInlineDepth();

        // Caller must not be excessively large.
        if (script()->length() >= optimizationInfo().inliningMaxCallerBytecodeLength()) {
            trackOptimizationOutcome(TrackedOutcome::CantInlineBigCaller);
            return DontInline(targetScript, "Vetoed: caller excessively large");
        }
    }

    BaselineScript* outerBaseline = outermostBuilder()->script()->baselineScript();
    if (inliningDepth_ >= maxInlineDepth) {
        // We hit the depth limit and won't inline this function. Give the
        // outermost script a max inlining depth of 0, so that it won't be
        // inlined in other scripts. This heuristic is currently only used
        // when we're inlining scripts with loops, see the comment below.
        outerBaseline->setMaxInliningDepth(0);

        trackOptimizationOutcome(TrackedOutcome::CantInlineExceededDepth);
        return DontInline(targetScript, "Vetoed: exceeding allowed inline depth");
    }

    // Inlining functions with loops can be complicated. For instance, if we're
    // close to the inlining depth limit and we inline the function f below, we
    // can no longer inline the call to g:
    //
    //   function f() {
    //      while (cond) {
    //          g();
    //      }
    //   }
    //
    // If the loop has many iterations, it's more efficient to call f and inline
    // g in f.
    //
    // To avoid this problem, we record a separate max inlining depth for each
    // script, indicating at which depth we won't be able to inline all functions
    // we inlined this time. This solves the issue above, because we will only
    // inline f if it means we can also inline g.
    if (targetScript->hasLoops() &&
        inliningDepth_ >= targetScript->baselineScript()->maxInliningDepth())
    {
        trackOptimizationOutcome(TrackedOutcome::CantInlineExceededDepth);
        return DontInline(targetScript, "Vetoed: exceeding allowed script inline depth");
    }

    // Update the max depth at which we can inline the outer script.
    MOZ_ASSERT(maxInlineDepth > inliningDepth_);
    uint32_t scriptInlineDepth = maxInlineDepth - inliningDepth_ - 1;
    if (scriptInlineDepth < outerBaseline->maxInliningDepth())
        outerBaseline->setMaxInliningDepth(scriptInlineDepth);

    // End of heuristics, we will inline this function.

    // TI calls ObjectStateChange to trigger invalidation of the caller.
    TypeSet::ObjectKey* targetKey = TypeSet::ObjectKey::get(target);
    targetKey->watchStateChangeForInlinedCall(constraints());

    outerBuilder->inlinedBytecodeLength_ += targetScript->length();

    return InliningDecision_Inline;
}

bool
IonBuilder::selectInliningTargets(const ObjectVector& targets, CallInfo& callInfo, BoolVector& choiceSet,
                                  uint32_t* numInlineable)
{
    *numInlineable = 0;
    uint32_t totalSize = 0;

    // For each target, ask whether it may be inlined.
    if (!choiceSet.reserve(targets.length()))
        return false;

    // Don't inline polymorphic sites during the definite properties analysis.
    // AddClearDefiniteFunctionUsesInScript depends on this for correctness.
    if (info().analysisMode() == Analysis_DefiniteProperties && targets.length() > 1)
        return true;

    for (size_t i = 0; i < targets.length(); i++) {
        JSObject* target = targets[i];

        trackOptimizationAttempt(TrackedStrategy::Call_Inline);
        trackTypeInfo(TrackedTypeSite::Call_Target, target);

        bool inlineable;
        InliningDecision decision = makeInliningDecision(target, callInfo);
        switch (decision) {
          case InliningDecision_Error:
            return false;
          case InliningDecision_DontInline:
          case InliningDecision_WarmUpCountTooLow:
            inlineable = false;
            break;
          case InliningDecision_Inline:
            inlineable = true;
            break;
          default:
            MOZ_CRASH("Unhandled InliningDecision value!");
        }

        if (target->is<JSFunction>()) {
            // Enforce a maximum inlined bytecode limit at the callsite.
            if (inlineable && target->as<JSFunction>().isInterpreted()) {
                totalSize += target->as<JSFunction>().nonLazyScript()->length();
                bool offThread = options.offThreadCompilationAvailable();
                if (totalSize > optimizationInfo().inlineMaxBytecodePerCallSite(offThread))
                    inlineable = false;
            }
        } else {
            // Non-function targets are not supported by polymorphic inlining.
            inlineable = false;
        }

        choiceSet.infallibleAppend(inlineable);
        if (inlineable)
            *numInlineable += 1;
    }

    // If optimization tracking is turned on and one of the inlineable targets
    // is a native, track the type info of the call. Most native inlinings
    // depend on the types of the arguments and the return value.
    if (isOptimizationTrackingEnabled()) {
        for (size_t i = 0; i < targets.length(); i++) {
            if (choiceSet[i] && targets[i]->as<JSFunction>().isNative()) {
                trackTypeInfo(callInfo);
                break;
            }
        }
    }

    MOZ_ASSERT(choiceSet.length() == targets.length());
    return true;
}

static bool
CanInlineGetPropertyCache(MGetPropertyCache* cache, MDefinition* thisDef)
{
    MOZ_ASSERT(cache->object()->type() == MIRType_Object);
    if (cache->object() != thisDef)
        return false;

    InlinePropertyTable* table = cache->propTable();
    if (!table)
        return false;
    if (table->numEntries() == 0)
        return false;
    return true;
}

class WrapMGetPropertyCache
{
    MGetPropertyCache* cache_;

  private:
    void discardPriorResumePoint() {
        if (!cache_)
            return;

        InlinePropertyTable* propTable = cache_->propTable();
        if (!propTable)
            return;
        MResumePoint* rp = propTable->takePriorResumePoint();
        if (!rp)
            return;
        cache_->block()->discardPreAllocatedResumePoint(rp);
    }

  public:
    explicit WrapMGetPropertyCache(MGetPropertyCache* cache)
      : cache_(cache)
    { }

    ~WrapMGetPropertyCache() {
        discardPriorResumePoint();
    }

    MGetPropertyCache* get() {
        return cache_;
    }
    MGetPropertyCache* operator->() {
        return get();
    }

    // This function returns the cache given to the constructor if the
    // GetPropertyCache can be moved into the ObjectGroup fallback path.
    MGetPropertyCache* moveableCache(bool hasTypeBarrier, MDefinition* thisDef) {
        // If we have unhandled uses of the MGetPropertyCache, then we cannot
        // move it to the ObjectGroup fallback path.
        if (!hasTypeBarrier) {
            if (cache_->hasUses())
                return nullptr;
        } else {
            // There is the TypeBarrier consumer, so we check that this is the
            // only consumer.
            MOZ_ASSERT(cache_->hasUses());
            if (!cache_->hasOneUse())
                return nullptr;
        }

        // If the this-object is not identical to the object of the
        // MGetPropertyCache, then we cannot use the InlinePropertyTable, or if
        // we do not yet have enough information from the ObjectGroup.
        if (!CanInlineGetPropertyCache(cache_, thisDef))
            return nullptr;

        MGetPropertyCache* ret = cache_;
        cache_ = nullptr;
        return ret;
    }
};

MGetPropertyCache*
IonBuilder::getInlineableGetPropertyCache(CallInfo& callInfo)
{
    if (callInfo.constructing())
        return nullptr;

    MDefinition* thisDef = callInfo.thisArg();
    if (thisDef->type() != MIRType_Object)
        return nullptr;

    MDefinition* funcDef = callInfo.fun();
    if (funcDef->type() != MIRType_Object)
        return nullptr;

    // MGetPropertyCache with no uses may be optimized away.
    if (funcDef->isGetPropertyCache()) {
        WrapMGetPropertyCache cache(funcDef->toGetPropertyCache());
        return cache.moveableCache(/* hasTypeBarrier = */ false, thisDef);
    }

    // Optimize away the following common pattern:
    // MTypeBarrier[MIRType_Object] <- MGetPropertyCache
    if (funcDef->isTypeBarrier()) {
        MTypeBarrier* barrier = funcDef->toTypeBarrier();
        if (barrier->hasUses())
            return nullptr;
        if (barrier->type() != MIRType_Object)
            return nullptr;
        if (!barrier->input()->isGetPropertyCache())
            return nullptr;

        WrapMGetPropertyCache cache(barrier->input()->toGetPropertyCache());
        return cache.moveableCache(/* hasTypeBarrier = */ true, thisDef);
    }

    return nullptr;
}

IonBuilder::InliningStatus
IonBuilder::inlineSingleCall(CallInfo& callInfo, JSObject* targetArg)
{
    if (!targetArg->is<JSFunction>()) {
        InliningStatus status = inlineNonFunctionCall(callInfo, targetArg);
        trackInlineSuccess(status);
        return status;
    }

    JSFunction* target = &targetArg->as<JSFunction>();
    if (target->isNative()) {
        InliningStatus status = inlineNativeCall(callInfo, target);
        trackInlineSuccess(status);
        return status;
    }

    // Track success now, as inlining a scripted call makes a new return block
    // which has a different pc than the current call pc.
    trackInlineSuccess();
    if (!inlineScriptedCall(callInfo, target))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineCallsite(const ObjectVector& targets, CallInfo& callInfo)
{
    if (targets.empty()) {
        trackOptimizationAttempt(TrackedStrategy::Call_Inline);
        trackOptimizationOutcome(TrackedOutcome::CantInlineNoTarget);
        return InliningStatus_NotInlined;
    }

    // Is the function provided by an MGetPropertyCache?
    // If so, the cache may be movable to a fallback path, with a dispatch
    // instruction guarding on the incoming ObjectGroup.
    WrapMGetPropertyCache propCache(getInlineableGetPropertyCache(callInfo));
    keepFallbackFunctionGetter(propCache.get());

    // Inline single targets -- unless they derive from a cache, in which case
    // avoiding the cache and guarding is still faster.
    if (!propCache.get() && targets.length() == 1) {
        JSObject* target = targets[0];

        trackOptimizationAttempt(TrackedStrategy::Call_Inline);
        trackTypeInfo(TrackedTypeSite::Call_Target, target);

        InliningDecision decision = makeInliningDecision(target, callInfo);
        switch (decision) {
          case InliningDecision_Error:
            return InliningStatus_Error;
          case InliningDecision_DontInline:
            return InliningStatus_NotInlined;
          case InliningDecision_WarmUpCountTooLow:
            return InliningStatus_WarmUpCountTooLow;
          case InliningDecision_Inline:
            break;
        }

        // Inlining will elminate uses of the original callee, but it needs to
        // be preserved in phis if we bail out.  Mark the old callee definition as
        // implicitly used to ensure this happens.
        callInfo.fun()->setImplicitlyUsedUnchecked();

        // If the callee is not going to be a lambda (which may vary across
        // different invocations), then the callee definition can be replaced by a
        // constant.
        if (target->isSingleton()) {
            // Replace the function with an MConstant.
            MConstant* constFun = constant(ObjectValue(*target));
            if (callInfo.constructing() && callInfo.getNewTarget() == callInfo.fun())
                callInfo.setNewTarget(constFun);
            callInfo.setFun(constFun);
        }

        return inlineSingleCall(callInfo, target);
    }

    // Choose a subset of the targets for polymorphic inlining.
    BoolVector choiceSet(alloc());
    uint32_t numInlined;
    if (!selectInliningTargets(targets, callInfo, choiceSet, &numInlined))
        return InliningStatus_Error;
    if (numInlined == 0)
        return InliningStatus_NotInlined;

    // Perform a polymorphic dispatch.
    if (!inlineCalls(callInfo, targets, choiceSet, propCache.get()))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

bool
IonBuilder::inlineGenericFallback(JSFunction* target, CallInfo& callInfo, MBasicBlock* dispatchBlock)
{
    // Generate a new block with all arguments on-stack.
    MBasicBlock* fallbackBlock = newBlock(dispatchBlock, pc);
    if (!fallbackBlock)
        return false;

    // Create a new CallInfo to track modified state within this block.
    CallInfo fallbackInfo(alloc(), callInfo.constructing());
    if (!fallbackInfo.init(callInfo))
        return false;
    fallbackInfo.popFormals(fallbackBlock);

    // Generate an MCall, which uses stateful |current|.
    if (!setCurrentAndSpecializePhis(fallbackBlock))
        return false;
    if (!makeCall(target, fallbackInfo))
        return false;

    // Pass return block to caller as |current|.
    return true;
}

bool
IonBuilder::inlineObjectGroupFallback(CallInfo& callInfo, MBasicBlock* dispatchBlock,
                                     MObjectGroupDispatch* dispatch, MGetPropertyCache* cache,
                                     MBasicBlock** fallbackTarget)
{
    // Getting here implies the following:
    // 1. The call function is an MGetPropertyCache, or an MGetPropertyCache
    //    followed by an MTypeBarrier.
    MOZ_ASSERT(callInfo.fun()->isGetPropertyCache() || callInfo.fun()->isTypeBarrier());

    // 2. The MGetPropertyCache has inlineable cases by guarding on the ObjectGroup.
    MOZ_ASSERT(dispatch->numCases() > 0);

    // 3. The MGetPropertyCache (and, if applicable, MTypeBarrier) only
    //    have at most a single use.
    MOZ_ASSERT_IF(callInfo.fun()->isGetPropertyCache(), !cache->hasUses());
    MOZ_ASSERT_IF(callInfo.fun()->isTypeBarrier(), cache->hasOneUse());

    // This means that no resume points yet capture the MGetPropertyCache,
    // so everything from the MGetPropertyCache up until the call is movable.
    // We now move the MGetPropertyCache and friends into a fallback path.
    MOZ_ASSERT(cache->idempotent());

    // Create a new CallInfo to track modified state within the fallback path.
    CallInfo fallbackInfo(alloc(), callInfo.constructing());
    if (!fallbackInfo.init(callInfo))
        return false;

    // Capture stack prior to the call operation. This captures the function.
    MResumePoint* preCallResumePoint =
        MResumePoint::New(alloc(), dispatchBlock, pc, MResumePoint::ResumeAt);
    if (!preCallResumePoint)
        return false;

    DebugOnly<size_t> preCallFuncIndex = preCallResumePoint->stackDepth() - callInfo.numFormals();
    MOZ_ASSERT(preCallResumePoint->getOperand(preCallFuncIndex) == fallbackInfo.fun());

    // In the dispatch block, replace the function's slot entry with Undefined.
    MConstant* undefined = MConstant::New(alloc(), UndefinedValue());
    dispatchBlock->add(undefined);
    dispatchBlock->rewriteAtDepth(-int(callInfo.numFormals()), undefined);

    // Construct a block that does nothing but remove formals from the stack.
    // This is effectively changing the entry resume point of the later fallback block.
    MBasicBlock* prepBlock = newBlock(dispatchBlock, pc);
    if (!prepBlock)
        return false;
    fallbackInfo.popFormals(prepBlock);

    // Construct a block into which the MGetPropertyCache can be moved.
    // This is subtle: the pc and resume point are those of the MGetPropertyCache!
    InlinePropertyTable* propTable = cache->propTable();
    MResumePoint* priorResumePoint = propTable->takePriorResumePoint();
    MOZ_ASSERT(propTable->pc() != nullptr);
    MOZ_ASSERT(priorResumePoint != nullptr);
    MBasicBlock* getPropBlock = newBlock(prepBlock, propTable->pc(), priorResumePoint);
    if (!getPropBlock)
        return false;

    prepBlock->end(MGoto::New(alloc(), getPropBlock));

    // Since the getPropBlock inherited the stack from right before the MGetPropertyCache,
    // the target of the MGetPropertyCache is still on the stack.
    DebugOnly<MDefinition*> checkObject = getPropBlock->pop();
    MOZ_ASSERT(checkObject == cache->object());

    // Move the MGetPropertyCache and friends into the getPropBlock.
    if (fallbackInfo.fun()->isGetPropertyCache()) {
        MOZ_ASSERT(fallbackInfo.fun()->toGetPropertyCache() == cache);
        getPropBlock->addFromElsewhere(cache);
        getPropBlock->push(cache);
    } else {
        MTypeBarrier* barrier = callInfo.fun()->toTypeBarrier();
        MOZ_ASSERT(barrier->type() == MIRType_Object);
        MOZ_ASSERT(barrier->input()->isGetPropertyCache());
        MOZ_ASSERT(barrier->input()->toGetPropertyCache() == cache);

        getPropBlock->addFromElsewhere(cache);
        getPropBlock->addFromElsewhere(barrier);
        getPropBlock->push(barrier);
    }

    // Construct an end block with the correct resume point.
    MBasicBlock* preCallBlock = newBlock(getPropBlock, pc, preCallResumePoint);
    if (!preCallBlock)
        return false;
    getPropBlock->end(MGoto::New(alloc(), preCallBlock));

    // Now inline the MCallGeneric, using preCallBlock as the dispatch point.
    if (!inlineGenericFallback(nullptr, fallbackInfo, preCallBlock))
        return false;

    // inlineGenericFallback() set the return block as |current|.
    preCallBlock->end(MGoto::New(alloc(), current));
    *fallbackTarget = prepBlock;
    return true;
}

bool
IonBuilder::inlineCalls(CallInfo& callInfo, const ObjectVector& targets, BoolVector& choiceSet,
                        MGetPropertyCache* maybeCache)
{
    // Only handle polymorphic inlining.
    MOZ_ASSERT(IsIonInlinablePC(pc));
    MOZ_ASSERT(choiceSet.length() == targets.length());
    MOZ_ASSERT_IF(!maybeCache, targets.length() >= 2);
    MOZ_ASSERT_IF(maybeCache, targets.length() >= 1);

    MBasicBlock* dispatchBlock = current;
    callInfo.setImplicitlyUsedUnchecked();
    callInfo.pushFormals(dispatchBlock);

    // Patch any InlinePropertyTable to only contain functions that are
    // inlineable. The InlinePropertyTable will also be patched at the end to
    // exclude native functions that vetoed inlining.
    if (maybeCache) {
        InlinePropertyTable* propTable = maybeCache->propTable();
        propTable->trimToTargets(targets);
        if (propTable->numEntries() == 0)
            maybeCache = nullptr;
    }

    // Generate a dispatch based on guard kind.
    MDispatchInstruction* dispatch;
    if (maybeCache) {
        dispatch = MObjectGroupDispatch::New(alloc(), maybeCache->object(), maybeCache->propTable());
        callInfo.fun()->setImplicitlyUsedUnchecked();
    } else {
        dispatch = MFunctionDispatch::New(alloc(), callInfo.fun());
    }

    // Generate a return block to host the rval-collecting MPhi.
    jsbytecode* postCall = GetNextPc(pc);
    MBasicBlock* returnBlock = newBlock(nullptr, postCall);
    if (!returnBlock)
        return false;
    returnBlock->setCallerResumePoint(callerResumePoint_);

    // Set up stack, used to manually create a post-call resume point.
    returnBlock->inheritSlots(dispatchBlock);
    callInfo.popFormals(returnBlock);

    MPhi* retPhi = MPhi::New(alloc());
    returnBlock->addPhi(retPhi);
    returnBlock->push(retPhi);

    // Create a resume point from current stack state.
    returnBlock->initEntrySlots(alloc());

    // Reserve the capacity for the phi.
    // Note: this is an upperbound. Unreachable targets and uninlineable natives are also counted.
    uint32_t count = 1; // Possible fallback block.
    for (uint32_t i = 0; i < targets.length(); i++) {
        if (choiceSet[i])
            count++;
    }
    retPhi->reserveLength(count);

    // Inline each of the inlineable targets.
    for (uint32_t i = 0; i < targets.length(); i++) {
        // Target must be inlineable.
        if (!choiceSet[i])
            continue;

        // Even though we made one round of inline decisions already, we may
        // be amending them below.
        amendOptimizationAttempt(i);

        // Target must be reachable by the MDispatchInstruction.
        JSFunction* target = &targets[i]->as<JSFunction>();
        if (maybeCache && !maybeCache->propTable()->hasFunction(target)) {
            choiceSet[i] = false;
            trackOptimizationOutcome(TrackedOutcome::CantInlineNotInDispatch);
            continue;
        }

        MBasicBlock* inlineBlock = newBlock(dispatchBlock, pc);
        if (!inlineBlock)
            return false;

        // Create a function MConstant to use in the entry ResumePoint. If we
        // can't use a constant, add a no-op MPolyInlineGuard, to prevent
        // hoisting scope chain gets above the dispatch instruction.
        MInstruction* funcDef;
        if (target->isSingleton())
            funcDef = MConstant::New(alloc(), ObjectValue(*target), constraints());
        else
            funcDef = MPolyInlineGuard::New(alloc(), callInfo.fun());

        funcDef->setImplicitlyUsedUnchecked();
        dispatchBlock->add(funcDef);

        // Use the inlined callee in the inline resume point and on stack.
        int funIndex = inlineBlock->entryResumePoint()->stackDepth() - callInfo.numFormals();
        inlineBlock->entryResumePoint()->replaceOperand(funIndex, funcDef);
        inlineBlock->rewriteSlot(funIndex, funcDef);

        // Create a new CallInfo to track modified state within the inline block.
        CallInfo inlineInfo(alloc(), callInfo.constructing());
        if (!inlineInfo.init(callInfo))
            return false;
        inlineInfo.popFormals(inlineBlock);
        inlineInfo.setFun(funcDef);

        if (maybeCache) {
            // Assign the 'this' value a TypeSet specialized to the groups that
            // can generate this inlining target.
            MOZ_ASSERT(callInfo.thisArg() == maybeCache->object());
            TemporaryTypeSet* thisTypes = maybeCache->propTable()->buildTypeSetForFunction(target);
            if (!thisTypes)
                return false;

            MFilterTypeSet* filter = MFilterTypeSet::New(alloc(), inlineInfo.thisArg(), thisTypes);
            inlineBlock->add(filter);
            inlineInfo.setThis(filter);
        }

        // Inline the call into the inlineBlock.
        if (!setCurrentAndSpecializePhis(inlineBlock))
            return false;
        InliningStatus status = inlineSingleCall(inlineInfo, target);
        if (status == InliningStatus_Error)
            return false;

        // Natives may veto inlining.
        if (status == InliningStatus_NotInlined) {
            MOZ_ASSERT(target->isNative());
            MOZ_ASSERT(current == inlineBlock);
            graph().removeBlock(inlineBlock);
            choiceSet[i] = false;
            continue;
        }

        // inlineSingleCall() changed |current| to the inline return block.
        MBasicBlock* inlineReturnBlock = current;
        setCurrent(dispatchBlock);

        // Connect the inline path to the returnBlock.
        ObjectGroup* funcGroup = target->isSingleton() ? nullptr : target->group();
        if (!dispatch->addCase(target, funcGroup, inlineBlock))
            return false;

        MDefinition* retVal = inlineReturnBlock->peek(-1);
        retPhi->addInput(retVal);
        inlineReturnBlock->end(MGoto::New(alloc(), returnBlock));
        if (!returnBlock->addPredecessorWithoutPhis(inlineReturnBlock))
            return false;
    }

    // Patch the InlinePropertyTable to not dispatch to vetoed paths.
    bool useFallback;
    if (maybeCache) {
        InlinePropertyTable* propTable = maybeCache->propTable();
        propTable->trimTo(targets, choiceSet);

        if (propTable->numEntries() == 0) {
            // If all paths were vetoed, output only a generic fallback path.
            MOZ_ASSERT(dispatch->numCases() == 0);
            maybeCache = nullptr;
            useFallback = true;
        } else {
            // We need a fallback path if the ObjectGroup dispatch does not
            // handle all incoming objects.
            useFallback = false;
            TemporaryTypeSet* objectTypes = maybeCache->object()->resultTypeSet();
            for (uint32_t i = 0; i < objectTypes->getObjectCount(); i++) {
                TypeSet::ObjectKey* obj = objectTypes->getObject(i);
                if (!obj)
                    continue;

                if (!obj->isGroup()) {
                    useFallback = true;
                    break;
                }

                if (!propTable->hasObjectGroup(obj->group())) {
                    useFallback = true;
                    break;
                }
            }

            if (!useFallback) {
                // The object group dispatch handles all possible incoming
                // objects, so the cache and barrier will not be reached and
                // can be eliminated.
                if (callInfo.fun()->isGetPropertyCache()) {
                    MOZ_ASSERT(callInfo.fun() == maybeCache);
                } else {
                    MTypeBarrier* barrier = callInfo.fun()->toTypeBarrier();
                    MOZ_ASSERT(!barrier->hasUses());
                    MOZ_ASSERT(barrier->type() == MIRType_Object);
                    MOZ_ASSERT(barrier->input()->isGetPropertyCache());
                    MOZ_ASSERT(barrier->input()->toGetPropertyCache() == maybeCache);
                    barrier->block()->discard(barrier);
                }

                MOZ_ASSERT(!maybeCache->hasUses());
                maybeCache->block()->discard(maybeCache);
            }
        }
    } else {
        useFallback = dispatch->numCases() < targets.length();
    }

    // If necessary, generate a fallback path.
    if (useFallback) {
        // Generate fallback blocks, and set |current| to the fallback return block.
        if (maybeCache) {
            MBasicBlock* fallbackTarget;
            if (!inlineObjectGroupFallback(callInfo, dispatchBlock,
                                           dispatch->toObjectGroupDispatch(),
                                           maybeCache, &fallbackTarget))
            {
                return false;
            }
            dispatch->addFallback(fallbackTarget);
        } else {
            JSFunction* remaining = nullptr;

            // If there is only 1 remaining case, we can annotate the fallback call
            // with the target information.
            if (dispatch->numCases() + 1 == targets.length()) {
                for (uint32_t i = 0; i < targets.length(); i++) {
                    if (choiceSet[i])
                        continue;

                    MOZ_ASSERT(!remaining);
                    if (targets[i]->is<JSFunction>() && targets[i]->as<JSFunction>().isSingleton())
                        remaining = &targets[i]->as<JSFunction>();
                    break;
                }
            }

            if (!inlineGenericFallback(remaining, callInfo, dispatchBlock))
                return false;
            dispatch->addFallback(current);
        }

        MBasicBlock* fallbackReturnBlock = current;

        // Connect fallback case to return infrastructure.
        MDefinition* retVal = fallbackReturnBlock->peek(-1);
        retPhi->addInput(retVal);
        fallbackReturnBlock->end(MGoto::New(alloc(), returnBlock));
        if (!returnBlock->addPredecessorWithoutPhis(fallbackReturnBlock))
            return false;
    }

    // Finally add the dispatch instruction.
    // This must be done at the end so that add() may be called above.
    dispatchBlock->end(dispatch);

    // Check the depth change: +1 for retval
    MOZ_ASSERT(returnBlock->stackDepth() == dispatchBlock->stackDepth() - callInfo.numFormals() + 1);

    graph().moveBlockToEnd(returnBlock);
    return setCurrentAndSpecializePhis(returnBlock);
}

MInstruction*
IonBuilder::createDeclEnvObject(MDefinition* callee, MDefinition* scope)
{
    // Get a template CallObject that we'll use to generate inline object
    // creation.
    DeclEnvObject* templateObj = inspector->templateDeclEnvObject();

    // One field is added to the function to handle its name.  This cannot be a
    // dynamic slot because there is still plenty of room on the DeclEnv object.
    MOZ_ASSERT(!templateObj->hasDynamicSlots());

    // Allocate the actual object. It is important that no intervening
    // instructions could potentially bailout, thus leaking the dynamic slots
    // pointer.
    MInstruction* declEnvObj = MNewDeclEnvObject::New(alloc(), templateObj);
    current->add(declEnvObj);

    // Initialize the object's reserved slots. No post barrier is needed here:
    // the object will be allocated in the nursery if possible, and if the
    // tenured heap is used instead, a minor collection will have been performed
    // that moved scope/callee to the tenured heap.
    current->add(MStoreFixedSlot::New(alloc(), declEnvObj, DeclEnvObject::enclosingScopeSlot(), scope));
    current->add(MStoreFixedSlot::New(alloc(), declEnvObj, DeclEnvObject::lambdaSlot(), callee));

    return declEnvObj;
}

MInstruction*
IonBuilder::createCallObject(MDefinition* callee, MDefinition* scope)
{
    // Get a template CallObject that we'll use to generate inline object
    // creation.
    CallObject* templateObj = inspector->templateCallObject();

    // Allocate the object. Run-once scripts need a singleton type, so always do
    // a VM call in such cases.
    MNullaryInstruction* callObj;
    if (script()->treatAsRunOnce())
        callObj = MNewRunOnceCallObject::New(alloc(), templateObj);
    else
        callObj = MNewCallObject::New(alloc(), templateObj);
    current->add(callObj);

    // Initialize the object's reserved slots. No post barrier is needed here,
    // for the same reason as in createDeclEnvObject.
    current->add(MStoreFixedSlot::New(alloc(), callObj, CallObject::enclosingScopeSlot(), scope));
    current->add(MStoreFixedSlot::New(alloc(), callObj, CallObject::calleeSlot(), callee));

    // Initialize argument slots.
    MSlots* slots = nullptr;
    for (AliasedFormalIter i(script()); i; i++) {
        unsigned slot = i.scopeSlot();
        unsigned formal = i.frameIndex();
        MDefinition* param = current->getSlot(info().argSlotUnchecked(formal));
        if (slot >= templateObj->numFixedSlots()) {
            if (!slots) {
                slots = MSlots::New(alloc(), callObj);
                current->add(slots);
            }
            current->add(MStoreSlot::New(alloc(), slots, slot - templateObj->numFixedSlots(), param));
        } else {
            current->add(MStoreFixedSlot::New(alloc(), callObj, slot, param));
        }
    }

    return callObj;
}

MDefinition*
IonBuilder::createThisScripted(MDefinition* callee, MDefinition* newTarget)
{
    // Get callee.prototype.
    //
    // This instruction MUST be idempotent: since it does not correspond to an
    // explicit operation in the bytecode, we cannot use resumeAfter().
    // Getters may not override |prototype| fetching, so this operation is indeed idempotent.
    // - First try an idempotent property cache.
    // - Upon failing idempotent property cache, we can't use a non-idempotent cache,
    //   therefore we fallback to CallGetProperty
    //
    // Note: both CallGetProperty and GetPropertyCache can trigger a GC,
    //       and thus invalidation.
    MInstruction* getProto;
    if (!invalidatedIdempotentCache()) {
        MConstant* id = constant(StringValue(names().prototype));
        MGetPropertyCache* getPropCache = MGetPropertyCache::New(alloc(), newTarget, id,
                                                                 /* monitored = */ false);
        getPropCache->setIdempotent();
        getProto = getPropCache;
    } else {
        MCallGetProperty* callGetProp = MCallGetProperty::New(alloc(), newTarget, names().prototype);
        callGetProp->setIdempotent();
        getProto = callGetProp;
    }
    current->add(getProto);

    // Create this from prototype
    MCreateThisWithProto* createThis = MCreateThisWithProto::New(alloc(), callee, newTarget, getProto);
    current->add(createThis);

    return createThis;
}

JSObject*
IonBuilder::getSingletonPrototype(JSFunction* target)
{
    TypeSet::ObjectKey* targetKey = TypeSet::ObjectKey::get(target);
    if (targetKey->unknownProperties())
        return nullptr;

    jsid protoid = NameToId(names().prototype);
    HeapTypeSetKey protoProperty = targetKey->property(protoid);

    return protoProperty.singleton(constraints());
}

MDefinition*
IonBuilder::createThisScriptedSingleton(JSFunction* target, MDefinition* callee)
{
    // Get the singleton prototype (if exists)
    JSObject* proto = getSingletonPrototype(target);
    if (!proto)
        return nullptr;

    JSObject* templateObject = inspector->getTemplateObject(pc);
    if (!templateObject)
        return nullptr;
    if (!templateObject->is<PlainObject>() && !templateObject->is<UnboxedPlainObject>())
        return nullptr;
    if (templateObject->getProto() != proto)
        return nullptr;

    TypeSet::ObjectKey* templateObjectKey = TypeSet::ObjectKey::get(templateObject->group());
    if (templateObjectKey->hasFlags(constraints(), OBJECT_FLAG_NEW_SCRIPT_CLEARED))
        return nullptr;

    StackTypeSet* thisTypes = TypeScript::ThisTypes(target->nonLazyScript());
    if (!thisTypes || !thisTypes->hasType(TypeSet::ObjectType(templateObject)))
        return nullptr;

    // Generate an inline path to create a new |this| object with
    // the given singleton prototype.
    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    MCreateThisWithTemplate* createThis =
        MCreateThisWithTemplate::New(alloc(), constraints(), templateConst,
                                     templateObject->group()->initialHeap(constraints()));
    current->add(templateConst);
    current->add(createThis);

    return createThis;
}

MDefinition*
IonBuilder::createThisScriptedBaseline(MDefinition* callee)
{
    // Try to inline |this| creation based on Baseline feedback.

    JSFunction* target = inspector->getSingleCallee(pc);
    if (!target || !target->hasScript())
        return nullptr;

    JSObject* templateObject = inspector->getTemplateObject(pc);
    if (!templateObject)
        return nullptr;
    if (!templateObject->is<PlainObject>() && !templateObject->is<UnboxedPlainObject>())
        return nullptr;

    Shape* shape = target->lookupPure(compartment->runtime()->names().prototype);
    if (!shape || !shape->hasDefaultGetter() || !shape->hasSlot())
        return nullptr;

    Value protov = target->getSlot(shape->slot());
    if (!protov.isObject())
        return nullptr;

    JSObject* proto = checkNurseryObject(&protov.toObject());
    if (proto != templateObject->getProto())
        return nullptr;

    TypeSet::ObjectKey* templateObjectKey = TypeSet::ObjectKey::get(templateObject->group());
    if (templateObjectKey->hasFlags(constraints(), OBJECT_FLAG_NEW_SCRIPT_CLEARED))
        return nullptr;

    StackTypeSet* thisTypes = TypeScript::ThisTypes(target->nonLazyScript());
    if (!thisTypes || !thisTypes->hasType(TypeSet::ObjectType(templateObject)))
        return nullptr;

    // Shape guard.
    callee = addShapeGuard(callee, target->lastProperty(), Bailout_ShapeGuard);

    // Guard callee.prototype == proto.
    MOZ_ASSERT(shape->numFixedSlots() == 0, "Must be a dynamic slot");
    MSlots* slots = MSlots::New(alloc(), callee);
    current->add(slots);
    MLoadSlot* prototype = MLoadSlot::New(alloc(), slots, shape->slot());
    current->add(prototype);
    MDefinition* protoConst = constant(ObjectValue(*proto));
    MGuardObjectIdentity* guard = MGuardObjectIdentity::New(alloc(), prototype, protoConst,
                                                            /* bailOnEquality = */ false);
    current->add(guard);

    // Generate an inline path to create a new |this| object with
    // the given prototype.
    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    MCreateThisWithTemplate* createThis =
        MCreateThisWithTemplate::New(alloc(), constraints(), templateConst,
                                     templateObject->group()->initialHeap(constraints()));
    current->add(templateConst);
    current->add(createThis);

    return createThis;
}

MDefinition*
IonBuilder::createThis(JSFunction* target, MDefinition* callee, MDefinition* newTarget)
{
    // Create |this| for unknown target.
    if (!target) {
        if (MDefinition* createThis = createThisScriptedBaseline(callee))
            return createThis;

        MCreateThis* createThis = MCreateThis::New(alloc(), callee, newTarget);
        current->add(createThis);
        return createThis;
    }

    // Native constructors build the new Object themselves.
    if (target->isNative()) {
        if (!target->isConstructor())
            return nullptr;

        MConstant* magic = MConstant::New(alloc(), MagicValue(JS_IS_CONSTRUCTING));
        current->add(magic);
        return magic;
    }

    if (target->isDerivedClassConstructor()) {
        MOZ_ASSERT(target->isClassConstructor());
        return constant(MagicValue(JS_UNINITIALIZED_LEXICAL));
    }

    // Try baking in the prototype.
    if (MDefinition* createThis = createThisScriptedSingleton(target, callee))
        return createThis;

    if (MDefinition* createThis = createThisScriptedBaseline(callee))
        return createThis;

    return createThisScripted(callee, newTarget);
}

bool
IonBuilder::jsop_funcall(uint32_t argc)
{
    // Stack for JSOP_FUNCALL:
    // 1:      arg0
    // ...
    // argc:   argN
    // argc+1: JSFunction*, the 'f' in |f.call()|, in |this| position.
    // argc+2: The native 'call' function.

    int calleeDepth = -((int)argc + 2);
    int funcDepth = -((int)argc + 1);

    // If |Function.prototype.call| may be overridden, don't optimize callsite.
    TemporaryTypeSet* calleeTypes = current->peek(calleeDepth)->resultTypeSet();
    JSFunction* native = getSingleCallTarget(calleeTypes);
    if (!native || !native->isNative() || native->native() != &fun_call) {
        CallInfo callInfo(alloc(), false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo);
    }
    current->peek(calleeDepth)->setImplicitlyUsedUnchecked();

    // Extract call target.
    TemporaryTypeSet* funTypes = current->peek(funcDepth)->resultTypeSet();
    JSFunction* target = getSingleCallTarget(funTypes);

    // Shimmy the slots down to remove the native 'call' function.
    current->shimmySlots(funcDepth - 1);

    bool zeroArguments = (argc == 0);

    // If no |this| argument was provided, explicitly pass Undefined.
    // Pushing is safe here, since one stack slot has been removed.
    if (zeroArguments) {
        pushConstant(UndefinedValue());
    } else {
        // |this| becomes implicit in the call.
        argc -= 1;
    }

    CallInfo callInfo(alloc(), false);
    if (!callInfo.init(current, argc))
        return false;

    // Try to inline the call.
    if (!zeroArguments) {
        InliningDecision decision = makeInliningDecision(target, callInfo);
        switch (decision) {
          case InliningDecision_Error:
            return false;
          case InliningDecision_DontInline:
          case InliningDecision_WarmUpCountTooLow:
            break;
          case InliningDecision_Inline:
            if (target->isInterpreted())
                return inlineScriptedCall(callInfo, target);
            break;
        }
    }

    // Call without inlining.
    return makeCall(target, callInfo);
}

bool
IonBuilder::jsop_funapply(uint32_t argc)
{
    int calleeDepth = -((int)argc + 2);

    TemporaryTypeSet* calleeTypes = current->peek(calleeDepth)->resultTypeSet();
    JSFunction* native = getSingleCallTarget(calleeTypes);
    if (argc != 2 || info().analysisMode() == Analysis_ArgumentsUsage) {
        CallInfo callInfo(alloc(), false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo);
    }

    // Disable compilation if the second argument to |apply| cannot be guaranteed
    // to be either definitely |arguments| or definitely not |arguments|.
    MDefinition* argument = current->peek(-1);
    if (script()->argumentsHasVarBinding() &&
        argument->mightBeType(MIRType_MagicOptimizedArguments) &&
        argument->type() != MIRType_MagicOptimizedArguments)
    {
        return abort("fun.apply with MaybeArguments");
    }

    // Fallback to regular call if arg 2 is not definitely |arguments|.
    if (argument->type() != MIRType_MagicOptimizedArguments) {
        // Optimize fun.apply(self, array) if the length is sane and there are no holes.
        TemporaryTypeSet* objTypes = argument->resultTypeSet();
        if (native && native->isNative() && native->native() == fun_apply &&
            objTypes &&
            objTypes->getKnownClass(constraints()) == &ArrayObject::class_ &&
            !objTypes->hasObjectFlags(constraints(), OBJECT_FLAG_LENGTH_OVERFLOW) &&
            ElementAccessIsPacked(constraints(), argument))
        {
            return jsop_funapplyarray(argc);
        }

        CallInfo callInfo(alloc(), false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo);
    }

    if ((!native || !native->isNative() ||
        native->native() != fun_apply) &&
        info().analysisMode() != Analysis_DefiniteProperties)
    {
        return abort("fun.apply speculation failed");
    }

    // Use funapply that definitely uses |arguments|
    return jsop_funapplyarguments(argc);
}

bool
IonBuilder::jsop_funapplyarray(uint32_t argc)
{
    MOZ_ASSERT(argc == 2);

    int funcDepth = -((int)argc + 1);

    // Extract call target.
    TemporaryTypeSet* funTypes = current->peek(funcDepth)->resultTypeSet();
    JSFunction* target = getSingleCallTarget(funTypes);

    // Pop the array agument
    MDefinition* argObj = current->pop();

    MElements* elements = MElements::New(alloc(), argObj);
    current->add(elements);

    // Pop the |this| argument.
    MDefinition* argThis = current->pop();

    // Unwrap the (JSFunction *) parameter.
    MDefinition* argFunc = current->pop();

    // Pop apply function.
    MDefinition* nativeFunc = current->pop();
    nativeFunc->setImplicitlyUsedUnchecked();

    MApplyArray* apply = MApplyArray::New(alloc(), target, argFunc, elements, argThis);
    current->add(apply);
    current->push(apply);
    if (!resumeAfter(apply))
        return false;

    TemporaryTypeSet* types = bytecodeTypes(pc);
    return pushTypeBarrier(apply, types, BarrierKind::TypeSet);
}

bool
IonBuilder::jsop_funapplyarguments(uint32_t argc)
{
    // Stack for JSOP_FUNAPPLY:
    // 1:      Vp
    // 2:      This
    // argc+1: JSFunction*, the 'f' in |f.call()|, in |this| position.
    // argc+2: The native 'apply' function.

    int funcDepth = -((int)argc + 1);

    // Extract call target.
    TemporaryTypeSet* funTypes = current->peek(funcDepth)->resultTypeSet();
    JSFunction* target = getSingleCallTarget(funTypes);

    // When this script isn't inlined, use MApplyArgs,
    // to copy the arguments from the stack and call the function
    if (inliningDepth_ == 0 && info().analysisMode() != Analysis_DefiniteProperties) {
        // The array argument corresponds to the arguments object. As the JIT
        // is implicitly reading the arguments object in the next instruction,
        // we need to prevent the deletion of the arguments object from resume
        // points, so that Baseline will behave correctly after a bailout.
        MDefinition* vp = current->pop();
        vp->setImplicitlyUsedUnchecked();

        MDefinition* argThis = current->pop();

        // Unwrap the (JSFunction*) parameter.
        MDefinition* argFunc = current->pop();

        // Pop apply function.
        MDefinition* nativeFunc = current->pop();
        nativeFunc->setImplicitlyUsedUnchecked();

        MArgumentsLength* numArgs = MArgumentsLength::New(alloc());
        current->add(numArgs);

        MApplyArgs* apply = MApplyArgs::New(alloc(), target, argFunc, numArgs, argThis);
        current->add(apply);
        current->push(apply);
        if (!resumeAfter(apply))
            return false;

        TemporaryTypeSet* types = bytecodeTypes(pc);
        return pushTypeBarrier(apply, types, BarrierKind::TypeSet);
    }

    // When inlining we have the arguments the function gets called with
    // and can optimize even more, by just calling the functions with the args.
    // We also try this path when doing the definite properties analysis, as we
    // can inline the apply() target and don't care about the actual arguments
    // that were passed in.

    CallInfo callInfo(alloc(), false);

    // Vp
    MDefinition* vp = current->pop();
    vp->setImplicitlyUsedUnchecked();

    // Arguments
    if (inliningDepth_) {
        if (!callInfo.setArgs(inlineCallInfo_->argv()))
            return false;
    }

    // This
    MDefinition* argThis = current->pop();
    callInfo.setThis(argThis);

    // Pop function parameter.
    MDefinition* argFunc = current->pop();
    callInfo.setFun(argFunc);

    // Pop apply function.
    MDefinition* nativeFunc = current->pop();
    nativeFunc->setImplicitlyUsedUnchecked();

    // Try to inline the call.
    InliningDecision decision = makeInliningDecision(target, callInfo);
    switch (decision) {
      case InliningDecision_Error:
        return false;
      case InliningDecision_DontInline:
      case InliningDecision_WarmUpCountTooLow:
        break;
      case InliningDecision_Inline:
        if (target->isInterpreted())
            return inlineScriptedCall(callInfo, target);
    }

    return makeCall(target, callInfo);
}

bool
IonBuilder::jsop_call(uint32_t argc, bool constructing)
{
    startTrackingOptimizations();

    // If this call has never executed, try to seed the observed type set
    // based on how the call result is used.
    TemporaryTypeSet* observed = bytecodeTypes(pc);
    if (observed->empty()) {
        if (BytecodeFlowsToBitop(pc)) {
            observed->addType(TypeSet::Int32Type(), alloc_->lifoAlloc());
        } else if (*GetNextPc(pc) == JSOP_POS) {
            // Note: this is lame, overspecialized on the code patterns used
            // by asm.js and should be replaced by a more general mechanism.
            // See bug 870847.
            observed->addType(TypeSet::DoubleType(), alloc_->lifoAlloc());
        }
    }

    int calleeDepth = -((int)argc + 2 + constructing);

    // Acquire known call target if existent.
    ObjectVector targets(alloc());
    TemporaryTypeSet* calleeTypes = current->peek(calleeDepth)->resultTypeSet();
    if (calleeTypes && !getPolyCallTargets(calleeTypes, constructing, targets, 4))
        return false;

    CallInfo callInfo(alloc(), constructing);
    if (!callInfo.init(current, argc))
        return false;

    // Try inlining
    InliningStatus status = inlineCallsite(targets, callInfo);
    if (status == InliningStatus_Inlined)
        return true;
    if (status == InliningStatus_Error)
        return false;

    // No inline, just make the call.
    JSFunction* target = nullptr;
    if (targets.length() == 1 && targets[0]->is<JSFunction>())
        target = &targets[0]->as<JSFunction>();

    if (target && status == InliningStatus_WarmUpCountTooLow) {
        MRecompileCheck* check =
            MRecompileCheck::New(alloc(), target->nonLazyScript(),
                                 optimizationInfo().inliningRecompileThreshold(),
                                 MRecompileCheck::RecompileCheck_Inlining);
        current->add(check);
    }

    return makeCall(target, callInfo);
}

bool
IonBuilder::testShouldDOMCall(TypeSet* inTypes, JSFunction* func, JSJitInfo::OpType opType)
{
    if (!func->isNative() || !func->jitInfo())
        return false;

    // If all the DOM objects flowing through are legal with this
    // property, we can bake in a call to the bottom half of the DOM
    // accessor
    DOMInstanceClassHasProtoAtDepth instanceChecker =
        compartment->runtime()->DOMcallbacks()->instanceClassMatchesProto;

    const JSJitInfo* jinfo = func->jitInfo();
    if (jinfo->type() != opType)
        return false;

    for (unsigned i = 0; i < inTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = inTypes->getObject(i);
        if (!key)
            continue;

        if (!key->hasStableClassAndProto(constraints()))
            return false;

        if (!instanceChecker(key->clasp(), jinfo->protoID, jinfo->depth))
            return false;
    }

    return true;
}

static bool
ArgumentTypesMatch(MDefinition* def, StackTypeSet* calleeTypes)
{
    if (!calleeTypes)
        return false;

    if (def->resultTypeSet()) {
        MOZ_ASSERT(def->type() == MIRType_Value || def->mightBeType(def->type()));
        return def->resultTypeSet()->isSubset(calleeTypes);
    }

    if (def->type() == MIRType_Value)
        return false;

    if (def->type() == MIRType_Object)
        return calleeTypes->unknownObject();

    return calleeTypes->mightBeMIRType(def->type());
}

bool
IonBuilder::testNeedsArgumentCheck(JSFunction* target, CallInfo& callInfo)
{
    // If we have a known target, check if the caller arg types are a subset of callee.
    // Since typeset accumulates and can't decrease that means we don't need to check
    // the arguments anymore.
    if (!target->hasScript())
        return true;

    JSScript* targetScript = target->nonLazyScript();

    if (!ArgumentTypesMatch(callInfo.thisArg(), TypeScript::ThisTypes(targetScript)))
        return true;
    uint32_t expected_args = Min<uint32_t>(callInfo.argc(), target->nargs());
    for (size_t i = 0; i < expected_args; i++) {
        if (!ArgumentTypesMatch(callInfo.getArg(i), TypeScript::ArgTypes(targetScript, i)))
            return true;
    }
    for (size_t i = callInfo.argc(); i < target->nargs(); i++) {
        if (!TypeScript::ArgTypes(targetScript, i)->mightBeMIRType(MIRType_Undefined))
            return true;
    }

    return false;
}

MCall*
IonBuilder::makeCallHelper(JSFunction* target, CallInfo& callInfo)
{
    // This function may be called with mutated stack.
    // Querying TI for popped types is invalid.

    uint32_t targetArgs = callInfo.argc();

    // Collect number of missing arguments provided that the target is
    // scripted. Native functions are passed an explicit 'argc' parameter.
    if (target && !target->isNative())
        targetArgs = Max<uint32_t>(target->nargs(), callInfo.argc());

    bool isDOMCall = false;
    if (target && !callInfo.constructing()) {
        // We know we have a single call target.  Check whether the "this" types
        // are DOM types and our function a DOM function, and if so flag the
        // MCall accordingly.
        TemporaryTypeSet* thisTypes = callInfo.thisArg()->resultTypeSet();
        if (thisTypes &&
            thisTypes->getKnownMIRType() == MIRType_Object &&
            thisTypes->isDOMClass(constraints()) &&
            testShouldDOMCall(thisTypes, target, JSJitInfo::Method))
        {
            isDOMCall = true;
        }
    }

    MCall* call = MCall::New(alloc(), target, targetArgs + 1 + callInfo.constructing(),
                             callInfo.argc(), callInfo.constructing(), isDOMCall);
    if (!call)
        return nullptr;

    if (callInfo.constructing())
        call->addArg(targetArgs + 1, callInfo.getNewTarget());

    // Explicitly pad any missing arguments with |undefined|.
    // This permits skipping the argumentsRectifier.
    for (int i = targetArgs; i > (int)callInfo.argc(); i--) {
        MOZ_ASSERT_IF(target, !target->isNative());
        MConstant* undef = constant(UndefinedValue());
        call->addArg(i, undef);
    }

    // Add explicit arguments.
    // Skip addArg(0) because it is reserved for this
    for (int32_t i = callInfo.argc() - 1; i >= 0; i--)
        call->addArg(i + 1, callInfo.getArg(i));

    // Now that we've told it about all the args, compute whether it's movable
    call->computeMovable();

    // Inline the constructor on the caller-side.
    if (callInfo.constructing()) {
        MDefinition* create = createThis(target, callInfo.fun(), callInfo.getNewTarget());
        if (!create) {
            abort("Failure inlining constructor for call.");
            return nullptr;
        }

        callInfo.thisArg()->setImplicitlyUsedUnchecked();
        callInfo.setThis(create);
    }

    // Pass |this| and function.
    MDefinition* thisArg = callInfo.thisArg();
    call->addArg(0, thisArg);

    if (target && !testNeedsArgumentCheck(target, callInfo))
        call->disableArgCheck();

    call->initFunction(callInfo.fun());

    current->add(call);
    return call;
}

static bool
DOMCallNeedsBarrier(const JSJitInfo* jitinfo, TemporaryTypeSet* types)
{
    MOZ_ASSERT(jitinfo->type() != JSJitInfo::InlinableNative);

    // If the return type of our DOM native is in "types" already, we don't
    // actually need a barrier.
    if (jitinfo->returnType() == JSVAL_TYPE_UNKNOWN)
        return true;

    // JSVAL_TYPE_OBJECT doesn't tell us much; we still have to barrier on the
    // actual type of the object.
    if (jitinfo->returnType() == JSVAL_TYPE_OBJECT)
        return true;

    // No need for a barrier if we're already expecting the type we'll produce.
    return MIRTypeFromValueType(jitinfo->returnType()) != types->getKnownMIRType();
}

bool
IonBuilder::makeCall(JSFunction* target, CallInfo& callInfo)
{
    // Constructor calls to non-constructors should throw. We don't want to use
    // CallKnown in this case.
    MOZ_ASSERT_IF(callInfo.constructing() && target, target->isConstructor());

    MCall* call = makeCallHelper(target, callInfo);
    if (!call)
        return false;

    current->push(call);
    if (call->isEffectful() && !resumeAfter(call))
        return false;

    TemporaryTypeSet* types = bytecodeTypes(pc);

    if (call->isCallDOMNative())
        return pushDOMTypeBarrier(call, types, call->getSingleTarget());

    return pushTypeBarrier(call, types, BarrierKind::TypeSet);
}

bool
IonBuilder::jsop_eval(uint32_t argc)
{
    int calleeDepth = -((int)argc + 2);
    TemporaryTypeSet* calleeTypes = current->peek(calleeDepth)->resultTypeSet();

    // Emit a normal call if the eval has never executed. This keeps us from
    // disabling compilation for the script when testing with --ion-eager.
    if (calleeTypes && calleeTypes->empty())
        return jsop_call(argc, /* constructing = */ false);

    JSFunction* singleton = getSingleCallTarget(calleeTypes);
    if (!singleton)
        return abort("No singleton callee for eval()");

    if (script()->global().valueIsEval(ObjectValue(*singleton))) {
        if (argc != 1)
            return abort("Direct eval with more than one argument");

        if (!info().funMaybeLazy())
            return abort("Direct eval in global code");

        if (info().funMaybeLazy()->isArrow())
            return abort("Direct eval from arrow function");

        CallInfo callInfo(alloc(), /* constructing = */ false);
        if (!callInfo.init(current, argc))
            return false;
        callInfo.setImplicitlyUsedUnchecked();

        callInfo.fun()->setImplicitlyUsedUnchecked();

        MDefinition* scopeChain = current->scopeChain();
        MDefinition* string = callInfo.getArg(0);

        // Direct eval acts as identity on non-string types according to
        // ES5 15.1.2.1 step 1.
        if (!string->mightBeType(MIRType_String)) {
            current->push(string);
            TemporaryTypeSet* types = bytecodeTypes(pc);
            return pushTypeBarrier(string, types, BarrierKind::TypeSet);
        }

        if (!jsop_newtarget())
            return false;
        MDefinition* newTargetValue = current->pop();

        // Try to pattern match 'eval(v + "()")'. In this case v is likely a
        // name on the scope chain and the eval is performing a call on that
        // value. Use a dynamic scope chain lookup rather than a full eval.
        if (string->isConcat() &&
            string->getOperand(1)->isConstantValue() &&
            string->getOperand(1)->constantValue().isString())
        {
            JSAtom* atom = &string->getOperand(1)->constantValue().toString()->asAtom();

            if (StringEqualsAscii(atom, "()")) {
                MDefinition* name = string->getOperand(0);
                MInstruction* dynamicName = MGetDynamicName::New(alloc(), scopeChain, name);
                current->add(dynamicName);

                current->push(dynamicName);
                current->push(constant(UndefinedValue())); // thisv

                CallInfo evalCallInfo(alloc(), /* constructing = */ false);
                if (!evalCallInfo.init(current, /* argc = */ 0))
                    return false;

                return makeCall(nullptr, evalCallInfo);
            }
        }

        MInstruction* ins = MCallDirectEval::New(alloc(), scopeChain, string,
                                                 newTargetValue, pc);
        current->add(ins);
        current->push(ins);

        TemporaryTypeSet* types = bytecodeTypes(pc);
        return resumeAfter(ins) && pushTypeBarrier(ins, types, BarrierKind::TypeSet);
    }

    return jsop_call(argc, /* constructing = */ false);
}

bool
IonBuilder::jsop_compare(JSOp op)
{
    MDefinition* right = current->pop();
    MDefinition* left = current->pop();

    return jsop_compare(op, left, right);
}

bool
IonBuilder::jsop_compare(JSOp op, MDefinition* left, MDefinition* right)
{
    bool emitted = false;

    if (!forceInlineCaches()) {
        if (!compareTrySpecialized(&emitted, op, left, right) || emitted)
            return emitted;
        if (!compareTryBitwise(&emitted, op, left, right) || emitted)
            return emitted;
        if (!compareTrySpecializedOnBaselineInspector(&emitted, op, left, right) || emitted)
            return emitted;
    }

    if (!compareTrySharedStub(&emitted, op, left, right) || emitted)
        return emitted;

    // Not possible to optimize. Do a slow vm call.
    MCompare* ins = MCompare::New(alloc(), left, right, op);
    ins->cacheOperandMightEmulateUndefined(constraints());

    current->add(ins);
    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
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

bool
IonBuilder::compareTrySpecialized(bool* emitted, JSOp op, MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit an compare based on the input types.

    MCompare::CompareType type = MCompare::determineCompareType(op, left, right);
    if (type == MCompare::Compare_Unknown)
        return true;

    MCompare* ins = MCompare::New(alloc(), left, right, op);
    ins->setCompareType(type);
    ins->cacheOperandMightEmulateUndefined(constraints());

    // Some compare types need to have the specific type in the rhs.
    // Swap operands if that is not the case.
    if (type == MCompare::Compare_StrictString && right->type() != MIRType_String)
        ins->swapOperands();
    else if (type == MCompare::Compare_Null && right->type() != MIRType_Null)
        ins->swapOperands();
    else if (type == MCompare::Compare_Undefined && right->type() != MIRType_Undefined)
        ins->swapOperands();
    else if (type == MCompare::Compare_Boolean && right->type() != MIRType_Boolean)
        ins->swapOperands();

    // Replace inputs with unsigned variants if needed.
    if (type == MCompare::Compare_UInt32)
        ins->replaceWithUnsignedOperands();

    current->add(ins);
    current->push(ins);

    MOZ_ASSERT(!ins->isEffectful());
    *emitted = true;
    return true;
}

bool
IonBuilder::compareTryBitwise(bool* emitted, JSOp op, MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a bitwise compare. Check if a bitwise compare equals the wanted
    // result for all observed operand types.

    // Onlye allow loose and strict equality.
    if (op != JSOP_EQ && op != JSOP_NE && op != JSOP_STRICTEQ && op != JSOP_STRICTNE)
        return true;

    // Only primitive (not double/string) or objects are supported.
    // I.e. Undefined/Null/Boolean/Int32 and Object
    if (!ObjectOrSimplePrimitive(left) || !ObjectOrSimplePrimitive(right))
        return true;

    // Objects that emulate undefined are not supported.
    if (left->maybeEmulatesUndefined(constraints()) || right->maybeEmulatesUndefined(constraints()))
        return true;

    // In the loose comparison more values could be the same,
    // but value comparison reporting otherwise.
    if (op == JSOP_EQ || op == JSOP_NE) {

        // Undefined compared loosy to Null is not supported,
        // because tag is different, but value can be the same (undefined == null).
        if ((left->mightBeType(MIRType_Undefined) && right->mightBeType(MIRType_Null)) ||
            (left->mightBeType(MIRType_Null) && right->mightBeType(MIRType_Undefined)))
        {
            return true;
        }

        // Int32 compared loosy to Boolean is not supported,
        // because tag is different, but value can be the same (1 == true).
        if ((left->mightBeType(MIRType_Int32) && right->mightBeType(MIRType_Boolean)) ||
            (left->mightBeType(MIRType_Boolean) && right->mightBeType(MIRType_Int32)))
        {
            return true;
        }

        // For loosy comparison of an object with a Boolean/Number/String
        // the valueOf the object is taken. Therefore not supported.
        bool simpleLHS = left->mightBeType(MIRType_Boolean) || left->mightBeType(MIRType_Int32);
        bool simpleRHS = right->mightBeType(MIRType_Boolean) || right->mightBeType(MIRType_Int32);
        if ((left->mightBeType(MIRType_Object) && simpleRHS) ||
            (right->mightBeType(MIRType_Object) && simpleLHS))
        {
            return true;
        }
    }

    MCompare* ins = MCompare::New(alloc(), left, right, op);
    ins->setCompareType(MCompare::Compare_Bitwise);
    ins->cacheOperandMightEmulateUndefined(constraints());

    current->add(ins);
    current->push(ins);

    MOZ_ASSERT(!ins->isEffectful());
    *emitted = true;
    return true;
}

bool
IonBuilder::compareTrySpecializedOnBaselineInspector(bool* emitted, JSOp op, MDefinition* left,
                                                     MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to specialize based on any baseline caches that have been generated
    // for the opcode. These will cause the instruction's type policy to insert
    // fallible unboxes to the appropriate input types.

    // Strict equality isn't supported.
    if (op == JSOP_STRICTEQ || op == JSOP_STRICTNE)
        return true;

    MCompare::CompareType type = inspector->expectedCompareType(pc);
    if (type == MCompare::Compare_Unknown)
        return true;

    MCompare* ins = MCompare::New(alloc(), left, right, op);
    ins->setCompareType(type);
    ins->cacheOperandMightEmulateUndefined(constraints());

    current->add(ins);
    current->push(ins);

    MOZ_ASSERT(!ins->isEffectful());
    *emitted = true;
    return true;
}

bool
IonBuilder::compareTrySharedStub(bool* emitted, JSOp op, MDefinition* left, MDefinition* right)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a shared stub cache.

    if (JitOptions.disableSharedStubs)
        return true;

    if (JSOp(*pc) == JSOP_CASE)
        return true;

    MBinarySharedStub* stub = MBinarySharedStub::New(alloc(), left, right);
    current->add(stub);
    current->push(stub);
    if (!resumeAfter(stub))
        return false;

    MUnbox* unbox = MUnbox::New(alloc(), current->pop(), MIRType_Boolean, MUnbox::Infallible);
    current->add(unbox);
    current->push(unbox);

    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_newarray(uint32_t length)
{
    JSObject* templateObject = inspector->getTemplateObject(pc);
    gc::InitialHeap heap;
    MConstant* templateConst;

    if (templateObject) {
        heap = templateObject->group()->initialHeap(constraints());
        templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    } else {
        heap = gc::DefaultHeap;
        templateConst = MConstant::New(alloc(), NullValue());
    }
    current->add(templateConst);

    MNewArray* ins = MNewArray::New(alloc(), constraints(), length, templateConst, heap, pc);
    current->add(ins);
    current->push(ins);

    ObjectGroup* templateGroup = inspector->getTemplateObjectGroup(pc);
    if (templateGroup) {
        TemporaryTypeSet* types = MakeSingletonTypeSet(constraints(), templateGroup);
        ins->setResultTypeSet(types);
    }

    return true;
}

bool
IonBuilder::jsop_newarray_copyonwrite()
{
    ArrayObject* templateObject = ObjectGroup::getCopyOnWriteObject(script(), pc);

    // The baseline compiler should have ensured the template object has a type
    // with the copy on write flag set already. During the arguments usage
    // analysis the baseline compiler hasn't run yet, however, though in this
    // case the template object's type doesn't matter.
    MOZ_ASSERT_IF(info().analysisMode() != Analysis_ArgumentsUsage,
                  templateObject->group()->hasAnyFlags(OBJECT_FLAG_COPY_ON_WRITE));

    MNewArrayCopyOnWrite* ins =
        MNewArrayCopyOnWrite::New(alloc(), constraints(), templateObject,
                                  templateObject->group()->initialHeap(constraints()));

    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_newobject()
{
    JSObject* templateObject = inspector->getTemplateObject(pc);
    gc::InitialHeap heap;
    MConstant* templateConst;

    if (templateObject) {
        heap = templateObject->group()->initialHeap(constraints());
        templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    } else {
        heap = gc::DefaultHeap;
        templateConst = MConstant::New(alloc(), NullValue());
    }

    current->add(templateConst);
    MNewObject* ins = MNewObject::New(alloc(), constraints(), templateConst, heap,
                                      MNewObject::ObjectLiteral);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_initelem()
{
    MDefinition* value = current->pop();
    MDefinition* id = current->pop();
    MDefinition* obj = current->peek(-1);

    MInitElem* initElem = MInitElem::New(alloc(), obj, id, value);
    current->add(initElem);

    return resumeAfter(initElem);
}

bool
IonBuilder::jsop_initelem_array()
{
    MDefinition* value = current->pop();
    MDefinition* obj = current->peek(-1);

    // Make sure that arrays have the type being written to them by the
    // intializer, and that arrays are marked as non-packed when writing holes
    // to them during initialization.
    bool needStub = false;
    JSValueType unboxedType = JSVAL_TYPE_MAGIC;
    if (shouldAbortOnPreliminaryGroups(obj)) {
        needStub = true;
    } else if (!obj->resultTypeSet() ||
               obj->resultTypeSet()->unknownObject() ||
               obj->resultTypeSet()->getObjectCount() != 1)
    {
        needStub = true;
    } else {
        MOZ_ASSERT(obj->resultTypeSet()->getObjectCount() == 1);
        TypeSet::ObjectKey* initializer = obj->resultTypeSet()->getObject(0);
        if (initializer->clasp() == &UnboxedArrayObject::class_) {
            if (initializer->group()->unboxedLayout().nativeGroup())
                needStub = true;
            else
                unboxedType = initializer->group()->unboxedLayout().elementType();
        }
        if (value->type() == MIRType_MagicHole) {
            if (!initializer->hasFlags(constraints(), OBJECT_FLAG_NON_PACKED))
                needStub = true;
        } else if (!initializer->unknownProperties()) {
            HeapTypeSetKey elemTypes = initializer->property(JSID_VOID);
            if (!TypeSetIncludes(elemTypes.maybeTypes(), value->type(), value->resultTypeSet())) {
                elemTypes.freeze(constraints());
                needStub = true;
            }
        }
    }

    uint32_t index = GET_UINT32(pc);
    if (needStub) {
        MCallInitElementArray* store = MCallInitElementArray::New(alloc(), obj, index, value);
        current->add(store);
        return resumeAfter(store);
    }

    return initializeArrayElement(obj, index, value, unboxedType, /* addResumePoint = */ true);
}

bool
IonBuilder::initializeArrayElement(MDefinition* obj, size_t index, MDefinition* value,
                                   JSValueType unboxedType,
                                   bool addResumePointAndIncrementInitializedLength)
{
    MConstant* id = MConstant::New(alloc(), Int32Value(index));
    current->add(id);

    // Get the elements vector.
    MElements* elements = MElements::New(alloc(), obj, unboxedType != JSVAL_TYPE_MAGIC);
    current->add(elements);

    if (unboxedType != JSVAL_TYPE_MAGIC) {
        // Note: storeUnboxedValue takes care of any post barriers on the value.
        storeUnboxedValue(obj, elements, 0, id, unboxedType, value, /* preBarrier = */ false);

        if (addResumePointAndIncrementInitializedLength) {
            MInstruction* increment = MIncrementUnboxedArrayInitializedLength::New(alloc(), obj);
            current->add(increment);

            if (!resumeAfter(increment))
                return false;
        }
    } else {
        if (NeedsPostBarrier(value))
            current->add(MPostWriteBarrier::New(alloc(), obj, value));

        if (obj->toNewArray()->convertDoubleElements()) {
            MInstruction* valueDouble = MToDouble::New(alloc(), value);
            current->add(valueDouble);
            value = valueDouble;
        }

        // Store the value.
        MStoreElement* store = MStoreElement::New(alloc(), elements, id, value,
                                                  /* needsHoleCheck = */ false);
        current->add(store);

        if (addResumePointAndIncrementInitializedLength) {
            // Update the initialized length. (The template object for this
            // array has the array's ultimate length, so the length field is
            // already correct: no updating needed.)
            MSetInitializedLength* initLength = MSetInitializedLength::New(alloc(), elements, id);
            current->add(initLength);

            if (!resumeAfter(initLength))
                return false;
        }
    }

    return true;
}

bool
IonBuilder::jsop_mutateproto()
{
    MDefinition* value = current->pop();
    MDefinition* obj = current->peek(-1);

    MMutateProto* mutate = MMutateProto::New(alloc(), obj, value);
    current->add(mutate);
    return resumeAfter(mutate);
}

bool
IonBuilder::jsop_initprop(PropertyName* name)
{
    bool useSlowPath = false;

    MDefinition* value = current->peek(-1);
    MDefinition* obj = current->peek(-2);
    if (obj->isLambda()) {
        useSlowPath = true;
    } else if (JSObject* templateObject = obj->toNewObject()->templateObject()) {
        if (templateObject->is<PlainObject>()) {
            if (!templateObject->as<PlainObject>().containsPure(name))
                useSlowPath = true;
        } else {
            MOZ_ASSERT(templateObject->as<UnboxedPlainObject>().layout().lookup(name));
        }
    } else {
        useSlowPath = true;
    }

    if (useSlowPath) {
        current->pop();
        MInitProp* init = MInitProp::New(alloc(), obj, name, value);
        current->add(init);
        return resumeAfter(init);
    }

    MInstruction* last = *current->rbegin();

    // This is definitely initializing an 'own' property of the object, treat
    // it as an assignment.
    if (!jsop_setprop(name))
        return false;

    // SETPROP pushed the value, instead of the object. Fix this on the stack,
    // and check the most recent resume point to see if it needs updating too.
    current->pop();
    current->push(obj);
    for (MInstructionReverseIterator riter = current->rbegin(); *riter != last; riter++) {
        if (MResumePoint* resumePoint = riter->resumePoint()) {
            MOZ_ASSERT(resumePoint->pc() == pc);
            if (resumePoint->mode() == MResumePoint::ResumeAfter) {
                size_t index = resumePoint->numOperands() - 1;
                resumePoint->replaceOperand(index, obj);
            }
            break;
        }
    }

    return true;
}

bool
IonBuilder::jsop_initprop_getter_setter(PropertyName* name)
{
    MDefinition* value = current->pop();
    MDefinition* obj = current->peek(-1);

    MInitPropGetterSetter* init = MInitPropGetterSetter::New(alloc(), obj, name, value);
    current->add(init);
    return resumeAfter(init);
}

bool
IonBuilder::jsop_initelem_getter_setter()
{
    MDefinition* value = current->pop();
    MDefinition* id = current->pop();
    MDefinition* obj = current->peek(-1);

    MInitElemGetterSetter* init = MInitElemGetterSetter::New(alloc(), obj, id, value);
    current->add(init);
    return resumeAfter(init);
}

MBasicBlock*
IonBuilder::addBlock(MBasicBlock* block, uint32_t loopDepth)
{
    if (!block)
        return nullptr;
    if (block->pc() && script()->hasScriptCounts())
        block->setHitCount(script()->getHitCount(block->pc()));
    graph().addBlock(block);
    block->setLoopDepth(loopDepth);
    return block;
}

MBasicBlock*
IonBuilder::newBlock(MBasicBlock* predecessor, jsbytecode* pc)
{
    MBasicBlock* block = MBasicBlock::New(graph(), &analysis(), info(), predecessor,
                                          bytecodeSite(pc), MBasicBlock::NORMAL);
    return addBlock(block, loopDepth_);
}

MBasicBlock*
IonBuilder::newBlock(MBasicBlock* predecessor, jsbytecode* pc, MResumePoint* priorResumePoint)
{
    MBasicBlock* block = MBasicBlock::NewWithResumePoint(graph(), info(), predecessor,
                                                         bytecodeSite(pc), priorResumePoint);
    return addBlock(block, loopDepth_);
}

MBasicBlock*
IonBuilder::newBlockPopN(MBasicBlock* predecessor, jsbytecode* pc, uint32_t popped)
{
    MBasicBlock* block = MBasicBlock::NewPopN(graph(), info(), predecessor, bytecodeSite(pc),
                                              MBasicBlock::NORMAL, popped);
    return addBlock(block, loopDepth_);
}

MBasicBlock*
IonBuilder::newBlockAfter(MBasicBlock* at, MBasicBlock* predecessor, jsbytecode* pc)
{
    MBasicBlock* block = MBasicBlock::New(graph(), &analysis(), info(), predecessor,
                                          bytecodeSite(pc), MBasicBlock::NORMAL);
    if (!block)
        return nullptr;
    block->setHitCount(0); // osr block
    graph().insertBlockAfter(at, block);
    return block;
}

MBasicBlock*
IonBuilder::newBlock(MBasicBlock* predecessor, jsbytecode* pc, uint32_t loopDepth)
{
    MBasicBlock* block = MBasicBlock::New(graph(), &analysis(), info(), predecessor,
                                          bytecodeSite(pc), MBasicBlock::NORMAL);
    return addBlock(block, loopDepth);
}

MBasicBlock*
IonBuilder::newOsrPreheader(MBasicBlock* predecessor, jsbytecode* loopEntry, jsbytecode* beforeLoopEntry)
{
    MOZ_ASSERT(LoopEntryCanIonOsr(loopEntry));
    MOZ_ASSERT(loopEntry == info().osrPc());

    // Create two blocks: one for the OSR entry with no predecessors, one for
    // the preheader, which has the OSR entry block as a predecessor. The
    // OSR block is always the second block (with id 1).
    MBasicBlock* osrBlock  = newBlockAfter(*graph().begin(), loopEntry);
    MBasicBlock* preheader = newBlock(predecessor, loopEntry);
    if (!osrBlock || !preheader)
        return nullptr;

    // Give the pre-header the same hit count as the code before the loop.
    if (script()->hasScriptCounts())
        preheader->setHitCount(script()->getHitCount(beforeLoopEntry));

    MOsrEntry* entry = MOsrEntry::New(alloc());
    osrBlock->add(entry);

    // Initialize |scopeChain|.
    {
        uint32_t slot = info().scopeChainSlot();

        MInstruction* scopev;
        if (analysis().usesScopeChain()) {
            scopev = MOsrScopeChain::New(alloc(), entry);
        } else {
            // Use an undefined value if the script does not need its scope
            // chain, to match the type that is already being tracked for the
            // slot.
            scopev = MConstant::New(alloc(), UndefinedValue());
        }

        osrBlock->add(scopev);
        osrBlock->initSlot(slot, scopev);
    }
    // Initialize |return value|
    {
        MInstruction* returnValue;
        if (!script()->noScriptRval())
            returnValue = MOsrReturnValue::New(alloc(), entry);
        else
            returnValue = MConstant::New(alloc(), UndefinedValue());
        osrBlock->add(returnValue);
        osrBlock->initSlot(info().returnValueSlot(), returnValue);
    }

    // Initialize arguments object.
    bool needsArgsObj = info().needsArgsObj();
    MInstruction* argsObj = nullptr;
    if (info().hasArguments()) {
        if (needsArgsObj)
            argsObj = MOsrArgumentsObject::New(alloc(), entry);
        else
            argsObj = MConstant::New(alloc(), UndefinedValue());
        osrBlock->add(argsObj);
        osrBlock->initSlot(info().argsObjSlot(), argsObj);
    }

    if (info().funMaybeLazy()) {
        // Initialize |this| parameter.
        MParameter* thisv = MParameter::New(alloc(), MParameter::THIS_SLOT, nullptr);
        osrBlock->add(thisv);
        osrBlock->initSlot(info().thisSlot(), thisv);

        // Initialize arguments.
        for (uint32_t i = 0; i < info().nargs(); i++) {
            uint32_t slot = needsArgsObj ? info().argSlotUnchecked(i) : info().argSlot(i);

            // Only grab arguments from the arguments object if the arguments object
            // aliases formals.  If the argsobj does not alias formals, then the
            // formals may have been assigned to during interpretation, and that change
            // will not be reflected in the argsobj.
            if (needsArgsObj && info().argsObjAliasesFormals()) {
                MOZ_ASSERT(argsObj && argsObj->isOsrArgumentsObject());
                // If this is an aliased formal, then the arguments object
                // contains a hole at this index.  Any references to this
                // variable in the jitcode will come from JSOP_*ALIASEDVAR
                // opcodes, so the slot itself can be set to undefined.  If
                // it's not aliased, it must be retrieved from the arguments
                // object.
                MInstruction* osrv;
                if (script()->formalIsAliased(i))
                    osrv = MConstant::New(alloc(), UndefinedValue());
                else
                    osrv = MGetArgumentsObjectArg::New(alloc(), argsObj, i);

                osrBlock->add(osrv);
                osrBlock->initSlot(slot, osrv);
            } else {
                MParameter* arg = MParameter::New(alloc(), i, nullptr);
                osrBlock->add(arg);
                osrBlock->initSlot(slot, arg);
            }
        }
    }

    // Initialize locals.
    for (uint32_t i = 0; i < info().nlocals(); i++) {
        uint32_t slot = info().localSlot(i);
        ptrdiff_t offset = BaselineFrame::reverseOffsetOfLocal(i);

        MOsrValue* osrv = MOsrValue::New(alloc(), entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Initialize stack.
    uint32_t numStackSlots = preheader->stackDepth() - info().firstStackSlot();
    for (uint32_t i = 0; i < numStackSlots; i++) {
        uint32_t slot = info().stackSlot(i);
        ptrdiff_t offset = BaselineFrame::reverseOffsetOfLocal(info().nlocals() + i);

        MOsrValue* osrv = MOsrValue::New(alloc(), entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Create an MStart to hold the first valid MResumePoint.
    MStart* start = MStart::New(alloc(), MStart::StartType_Osr);
    osrBlock->add(start);

    // MOsrValue instructions are infallible, so the first MResumePoint must
    // occur after they execute, at the point of the MStart.
    if (!resumeAt(start, loopEntry))
        return nullptr;

    // Link the same MResumePoint from the MStart to each MOsrValue.
    // This causes logic in ShouldSpecializeInput() to not replace Uses with
    // Unboxes in the MResumePiont, so that the MStart always sees Values.
    if (!osrBlock->linkOsrValues(start))
        return nullptr;

    // Clone types of the other predecessor of the pre-header to the osr block,
    // such as pre-header phi's won't discard specialized type of the
    // predecessor.
    MOZ_ASSERT(predecessor->stackDepth() == osrBlock->stackDepth());
    MOZ_ASSERT(info().scopeChainSlot() == 0);

    // Treat the OSR values as having the same type as the existing values
    // coming in to the loop. These will be fixed up with appropriate
    // unboxing and type barriers in finishLoop, once the possible types
    // at the loop header are known.
    for (uint32_t i = info().startArgSlot(); i < osrBlock->stackDepth(); i++) {
        MDefinition* existing = current->getSlot(i);
        MDefinition* def = osrBlock->getSlot(i);
        MOZ_ASSERT_IF(!needsArgsObj || !info().isSlotAliasedAtOsr(i), def->type() == MIRType_Value);

        // Aliased slots are never accessed, since they need to go through
        // the callobject. No need to type them here.
        if (info().isSlotAliasedAtOsr(i))
            continue;

        def->setResultType(existing->type());
        def->setResultTypeSet(existing->resultTypeSet());
    }

    // Finish the osrBlock.
    osrBlock->end(MGoto::New(alloc(), preheader));
    if (!preheader->addPredecessor(alloc(), osrBlock))
        return nullptr;
    graph().setOsrBlock(osrBlock);

    return preheader;
}

MBasicBlock*
IonBuilder::newPendingLoopHeader(MBasicBlock* predecessor, jsbytecode* pc, bool osr, bool canOsr,
                                 unsigned stackPhiCount)
{
    loopDepth_++;
    // If this site can OSR, all values on the expression stack are part of the loop.
    if (canOsr)
        stackPhiCount = predecessor->stackDepth() - info().firstStackSlot();
    MBasicBlock* block = MBasicBlock::NewPendingLoopHeader(graph(), info(), predecessor,
                                                           bytecodeSite(pc), stackPhiCount);
    if (!addBlock(block, loopDepth_))
        return nullptr;

    if (osr) {
        // Incorporate type information from the OSR frame into the loop
        // header. The OSR frame may have unexpected types due to type changes
        // within the loop body or due to incomplete profiling information,
        // in which case this may avoid restarts of loop analysis or bailouts
        // during the OSR itself.

        // Unbox the MOsrValue if it is known to be unboxable.
        for (uint32_t i = info().startArgSlot(); i < block->stackDepth(); i++) {

            // The value of aliased args and slots are in the callobject. So we can't
            // the value from the baseline frame.
            if (info().isSlotAliasedAtOsr(i))
                continue;

            // Don't bother with expression stack values. The stack should be
            // empty except for let variables (not Ion-compiled) or iterators.
            if (i >= info().firstStackSlot())
                continue;

            MPhi* phi = block->getSlot(i)->toPhi();

            // Get the type from the baseline frame.
            TypeSet::Type existingType = TypeSet::UndefinedType();
            uint32_t arg = i - info().firstArgSlot();
            uint32_t var = i - info().firstLocalSlot();
            if (info().funMaybeLazy() && i == info().thisSlot())
                existingType = baselineFrame_->thisType;
            else if (arg < info().nargs())
                existingType = baselineFrame_->argTypes[arg];
            else
                existingType = baselineFrame_->varTypes[var];

            if (existingType.isSingletonUnchecked())
                checkNurseryObject(existingType.singleton());

            // Extract typeset from value.
            LifoAlloc* lifoAlloc = alloc().lifoAlloc();
            TemporaryTypeSet* typeSet =
                lifoAlloc->new_<TemporaryTypeSet>(lifoAlloc, existingType);
            if (!typeSet)
                return nullptr;
            MIRType type = typeSet->getKnownMIRType();
            if (!phi->addBackedgeType(type, typeSet))
                return nullptr;
        }
    }

    return block;
}

MTest*
IonBuilder::newTest(MDefinition* ins, MBasicBlock* ifTrue, MBasicBlock* ifFalse)
{
    MTest* test = MTest::New(alloc(), ins, ifTrue, ifFalse);
    test->cacheOperandMightEmulateUndefined(constraints());
    return test;
}

// A resume point is a mapping of stack slots to MDefinitions. It is used to
// capture the environment such that if a guard fails, and IonMonkey needs
// to exit back to the interpreter, the interpreter state can be
// reconstructed.
//
// We capture stack state at critical points:
//   * (1) At the beginning of every basic block.
//   * (2) After every effectful operation.
//
// As long as these two properties are maintained, instructions can
// be moved, hoisted, or, eliminated without problems, and ops without side
// effects do not need to worry about capturing state at precisely the
// right point in time.
//
// Effectful instructions, of course, need to capture state after completion,
// where the interpreter will not attempt to repeat the operation. For this,
// ResumeAfter must be used. The state is attached directly to the effectful
// instruction to ensure that no intermediate instructions could be injected
// in between by a future analysis pass.
//
// During LIR construction, if an instruction can bail back to the interpreter,
// we create an LSnapshot, which uses the last known resume point to request
// register/stack assignments for every live value.
bool
IonBuilder::resume(MInstruction* ins, jsbytecode* pc, MResumePoint::Mode mode)
{
    MOZ_ASSERT(ins->isEffectful() || !ins->isMovable());

    MResumePoint* resumePoint = MResumePoint::New(alloc(), ins->block(), pc,
                                                  mode);
    if (!resumePoint)
        return false;
    ins->setResumePoint(resumePoint);
    return true;
}

bool
IonBuilder::resumeAt(MInstruction* ins, jsbytecode* pc)
{
    return resume(ins, pc, MResumePoint::ResumeAt);
}

bool
IonBuilder::resumeAfter(MInstruction* ins)
{
    return resume(ins, pc, MResumePoint::ResumeAfter);
}

bool
IonBuilder::maybeInsertResume()
{
    // Create a resume point at the current position, without an existing
    // effectful instruction. This resume point is not necessary for correct
    // behavior (see above), but is added to avoid holding any values from the
    // previous resume point which are now dead. This shortens the live ranges
    // of such values and improves register allocation.
    //
    // This optimization is not performed outside of loop bodies, where good
    // register allocation is not as critical, in order to avoid creating
    // excessive resume points.

    if (loopDepth_ == 0)
        return true;

    MNop* ins = MNop::New(alloc());
    current->add(ins);

    return resumeAfter(ins);
}

void
IonBuilder::maybeMarkEmpty(MDefinition* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Value);

    // When one of the operands has no type information, mark the output
    // as having no possible types too. This is to avoid degrading
    // subsequent analysis.
    for (size_t i = 0; i < ins->numOperands(); i++) {
        if (!ins->emptyResultTypeSet())
            continue;

        TemporaryTypeSet* types = alloc().lifoAlloc()->new_<TemporaryTypeSet>();
        if (types)
            ins->setResultTypeSet(types);
    }
}

// Return whether property lookups can be performed effectlessly on clasp.
static bool
ClassHasEffectlessLookup(const Class* clasp)
{
    return (clasp == &UnboxedPlainObject::class_) ||
           (clasp == &UnboxedArrayObject::class_) ||
           IsTypedObjectClass(clasp) ||
           (clasp->isNative() && !clasp->ops.lookupProperty);
}

// Return whether an object might have a property for name which is not
// accounted for by type information.
static bool
ObjectHasExtraOwnProperty(CompileCompartment* comp, TypeSet::ObjectKey* object, jsid id)
{
    // Some typed object properties are not reflected in type information.
    if (object->isGroup() && object->group()->maybeTypeDescr())
        return object->group()->typeDescr().hasProperty(comp->runtime()->names(), id);

    const Class* clasp = object->clasp();

    // Array |length| properties are not reflected in type information.
    if (clasp == &ArrayObject::class_)
        return JSID_IS_ATOM(id, comp->runtime()->names().length);

    // Resolve hooks can install new properties on objects on demand.
    JSObject* singleton = object->isSingleton() ? object->singleton() : nullptr;
    return ClassMayResolveId(comp->runtime()->names(), clasp, id, singleton);
}

void
IonBuilder::insertRecompileCheck()
{
    // No need for recompile checks if this is the highest optimization level.
    OptimizationLevel curLevel = optimizationInfo().level();
    if (IonOptimizations.isLastLevel(curLevel))
        return;

    // Add recompile check.

    // Get the topmost builder. The topmost script will get recompiled when
    // warm-up counter is high enough to justify a higher optimization level.
    IonBuilder* topBuilder = outermostBuilder();

    // Add recompile check to recompile when the warm-up count reaches the
    // threshold of the next optimization level.
    OptimizationLevel nextLevel = IonOptimizations.nextLevel(curLevel);
    const OptimizationInfo* info = IonOptimizations.get(nextLevel);
    uint32_t warmUpThreshold = info->compilerWarmUpThreshold(topBuilder->script());
    MRecompileCheck* check = MRecompileCheck::New(alloc(), topBuilder->script(), warmUpThreshold,
                                MRecompileCheck::RecompileCheck_OptimizationLevel);
    current->add(check);
}

JSObject*
IonBuilder::testSingletonProperty(JSObject* obj, jsid id)
{
    // We would like to completely no-op property/global accesses which can
    // produce only a particular JSObject. When indicating the access result is
    // definitely an object, type inference does not account for the
    // possibility that the property is entirely missing from the input object
    // and its prototypes (if this happens, a semantic trigger would be hit and
    // the pushed types updated, even if there is no type barrier).
    //
    // If the access definitely goes through obj, either directly or on the
    // prototype chain, and the object has singleton type, then the type
    // information for that property reflects the value that will definitely be
    // read on accesses to the object. If the property is later deleted or
    // reconfigured as a getter/setter then the type information for the
    // property will change and trigger invalidation.

    while (obj) {
        if (!ClassHasEffectlessLookup(obj->getClass()))
            return nullptr;

        TypeSet::ObjectKey* objKey = TypeSet::ObjectKey::get(obj);
        if (analysisContext)
            objKey->ensureTrackedProperty(analysisContext, id);

        if (objKey->unknownProperties())
            return nullptr;

        HeapTypeSetKey property = objKey->property(id);
        if (property.isOwnProperty(constraints())) {
            if (obj->isSingleton())
                return property.singleton(constraints());
            return nullptr;
        }

        if (ObjectHasExtraOwnProperty(compartment, objKey, id))
            return nullptr;

        obj = checkNurseryObject(obj->getProto());
    }

    return nullptr;
}

JSObject*
IonBuilder::testSingletonPropertyTypes(MDefinition* obj, jsid id)
{
    // As for TestSingletonProperty, but the input is any value in a type set
    // rather than a specific object.

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (types && types->unknownObject())
        return nullptr;

    JSObject* objectSingleton = types ? types->maybeSingleton() : nullptr;
    if (objectSingleton)
        return testSingletonProperty(objectSingleton, id);

    MIRType objType = obj->type();
    if (objType == MIRType_Value && types)
        objType = types->getKnownMIRType();

    JSProtoKey key;
    switch (objType) {
      case MIRType_String:
        key = JSProto_String;
        break;

      case MIRType_Symbol:
        key = JSProto_Symbol;
        break;

      case MIRType_Int32:
      case MIRType_Double:
        key = JSProto_Number;
        break;

      case MIRType_Boolean:
        key = JSProto_Boolean;
        break;

      case MIRType_Object: {
        if (!types)
            return nullptr;

        // For property accesses which may be on many objects, we just need to
        // find a prototype common to all the objects; if that prototype
        // has the singleton property, the access will not be on a missing property.
        JSObject* singleton = nullptr;
        for (unsigned i = 0; i < types->getObjectCount(); i++) {
            TypeSet::ObjectKey* key = types->getObject(i);
            if (!key)
                continue;
            if (analysisContext)
                key->ensureTrackedProperty(analysisContext, id);

            const Class* clasp = key->clasp();
            if (!ClassHasEffectlessLookup(clasp) || ObjectHasExtraOwnProperty(compartment, key, id))
                return nullptr;
            if (key->unknownProperties())
                return nullptr;
            HeapTypeSetKey property = key->property(id);
            if (property.isOwnProperty(constraints()))
                return nullptr;

            if (JSObject* proto = checkNurseryObject(key->proto().toObjectOrNull())) {
                // Test this type.
                JSObject* thisSingleton = testSingletonProperty(proto, id);
                if (!thisSingleton)
                    return nullptr;
                if (singleton) {
                    if (thisSingleton != singleton)
                        return nullptr;
                } else {
                    singleton = thisSingleton;
                }
            } else {
                // Can't be on the prototype chain with no prototypes...
                return nullptr;
            }
        }
        return singleton;
      }
      default:
        return nullptr;
    }

    JSObject* proto = GetBuiltinPrototypePure(&script()->global(), key);
    if (proto)
        return testSingletonProperty(proto, id);

    return nullptr;
}

bool
IonBuilder::pushTypeBarrier(MDefinition* def, TemporaryTypeSet* observed, BarrierKind kind)
{
    MOZ_ASSERT(def == current->peek(-1));

    MDefinition* replace = addTypeBarrier(current->pop(), observed, kind);
    if (!replace)
        return false;

    current->push(replace);
    return true;
}

// Given an observed type set, annotates the IR as much as possible:
// (1) If no type information is provided, the given value is returned.
// (2) If a single type definitely exists, and no type barrier is needed,
//     then an infallible unbox instruction is returned.
// (3) If a type barrier is needed, but has an unknown type set, the given
//     value is returned.
// (4) Lastly, a type barrier instruction is added and returned.
MDefinition*
IonBuilder::addTypeBarrier(MDefinition* def, TemporaryTypeSet* observed, BarrierKind kind,
                           MTypeBarrier** pbarrier)
{
    // Barriers are never needed for instructions whose result will not be used.
    if (BytecodeIsPopped(pc))
        return def;

    // If the instruction has no side effects, we'll resume the entire operation.
    // The actual type barrier will occur in the interpreter. If the
    // instruction is effectful, even if it has a singleton type, there
    // must be a resume point capturing the original def, and resuming
    // to that point will explicitly monitor the new type.
    if (kind == BarrierKind::NoBarrier) {
        MDefinition* replace = ensureDefiniteType(def, observed->getKnownMIRType());
        replace->setResultTypeSet(observed);
        return replace;
    }

    if (observed->unknown())
        return def;

    MTypeBarrier* barrier = MTypeBarrier::New(alloc(), def, observed, kind);
    current->add(barrier);

    if (pbarrier)
        *pbarrier = barrier;

    if (barrier->type() == MIRType_Undefined)
        return constant(UndefinedValue());
    if (barrier->type() == MIRType_Null)
        return constant(NullValue());

    return barrier;
}

bool
IonBuilder::pushDOMTypeBarrier(MInstruction* ins, TemporaryTypeSet* observed, JSFunction* func)
{
    MOZ_ASSERT(func && func->isNative() && func->jitInfo());

    const JSJitInfo* jitinfo = func->jitInfo();
    bool barrier = DOMCallNeedsBarrier(jitinfo, observed);
    // Need to be a bit careful: if jitinfo->returnType is JSVAL_TYPE_DOUBLE but
    // types->getKnownMIRType() is MIRType_Int32, then don't unconditionally
    // unbox as a double.  Instead, go ahead and barrier on having an int type,
    // since we know we need a barrier anyway due to the type mismatch.  This is
    // the only situation in which TI actually has more information about the
    // JSValueType than codegen can, short of jitinfo->returnType just being
    // JSVAL_TYPE_UNKNOWN.
    MDefinition* replace = ins;
    if (jitinfo->returnType() != JSVAL_TYPE_DOUBLE ||
        observed->getKnownMIRType() != MIRType_Int32) {
        replace = ensureDefiniteType(ins, MIRTypeFromValueType(jitinfo->returnType()));
        if (replace != ins) {
            current->pop();
            current->push(replace);
        }
    } else {
        MOZ_ASSERT(barrier);
    }

    return pushTypeBarrier(replace, observed,
                           barrier ? BarrierKind::TypeSet : BarrierKind::NoBarrier);
}

MDefinition*
IonBuilder::ensureDefiniteType(MDefinition* def, MIRType definiteType)
{
    MInstruction* replace;
    switch (definiteType) {
      case MIRType_Undefined:
        def->setImplicitlyUsedUnchecked();
        replace = MConstant::New(alloc(), UndefinedValue());
        break;

      case MIRType_Null:
        def->setImplicitlyUsedUnchecked();
        replace = MConstant::New(alloc(), NullValue());
        break;

      case MIRType_Value:
        return def;

      default: {
        if (def->type() != MIRType_Value) {
            if (def->type() == MIRType_Int32 && definiteType == MIRType_Double) {
                replace = MToDouble::New(alloc(), def);
                break;
            }
            MOZ_ASSERT(def->type() == definiteType);
            return def;
        }
        replace = MUnbox::New(alloc(), def, definiteType, MUnbox::Infallible);
        break;
      }
    }

    current->add(replace);
    return replace;
}

MDefinition*
IonBuilder::ensureDefiniteTypeSet(MDefinition* def, TemporaryTypeSet* types)
{
    // We cannot arbitrarily add a typeset to a definition. It can be shared
    // in another path. So we always need to create a new MIR.

    // Use ensureDefiniteType to do unboxing. If that happened the type can
    // be added on the newly created unbox operation.
    MDefinition* replace = ensureDefiniteType(def, types->getKnownMIRType());
    if (replace != def) {
        replace->setResultTypeSet(types);
        return replace;
    }

    // Don't replace if input type is more accurate than given typeset.
    if (def->type() != types->getKnownMIRType()) {
        MOZ_ASSERT(types->getKnownMIRType() == MIRType_Value);
        return def;
    }

    // Create a NOP mir instruction to filter the typeset.
    MFilterTypeSet* filter = MFilterTypeSet::New(alloc(), def, types);
    current->add(filter);
    return filter;
}

static size_t
NumFixedSlots(JSObject* object)
{
    // Note: we can't use object->numFixedSlots() here, as this will read the
    // shape and can race with the main thread if we are building off thread.
    // The allocation kind and object class (which goes through the type) can
    // be read freely, however.
    gc::AllocKind kind = object->asTenured().getAllocKind();
    return gc::GetGCKindSlots(kind, object->getClass());
}

static bool
IsUninitializedGlobalLexicalSlot(JSObject* obj, PropertyName* name)
{
    ClonedBlockObject &globalLexical = obj->as<ClonedBlockObject>();
    MOZ_ASSERT(globalLexical.isGlobal());
    Shape* shape = globalLexical.lookupPure(name);
    if (!shape)
        return false;
    return globalLexical.getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL);
}

bool
IonBuilder::getStaticName(JSObject* staticObject, PropertyName* name, bool* psucceeded,
                          MDefinition* lexicalCheck)
{
    MOZ_ASSERT(*psucceeded == false);

    jsid id = NameToId(name);

    bool isGlobalLexical = staticObject->is<ClonedBlockObject>() &&
                           staticObject->as<ClonedBlockObject>().isGlobal();
    MOZ_ASSERT(isGlobalLexical ||
               staticObject->is<GlobalObject>() ||
               staticObject->is<LexicalScopeBase>());
    MOZ_ASSERT(staticObject->isSingleton());

    *psucceeded = true;

    if (staticObject->is<GlobalObject>()) {
        // Known values on the global definitely don't need TDZ checks.
        if (lexicalCheck)
            lexicalCheck->setNotGuardUnchecked();

        // Optimize undefined, NaN, and Infinity.
        if (name == names().undefined)
            return pushConstant(UndefinedValue());
        if (name == names().NaN)
            return pushConstant(compartment->runtime()->NaNValue());
        if (name == names().Infinity)
            return pushConstant(compartment->runtime()->positiveInfinityValue());
    }

    // When not loading a known value on the global with a lexical check,
    // always emit the lexical check. This could be optimized, but is
    // currently not for simplicity's sake.
    if (lexicalCheck) {
        *psucceeded = false;
        return true;
    }

    TypeSet::ObjectKey* staticKey = TypeSet::ObjectKey::get(staticObject);
    if (analysisContext)
        staticKey->ensureTrackedProperty(analysisContext, NameToId(name));

    if (staticKey->unknownProperties()) {
        *psucceeded = false;
        return true;
    }

    HeapTypeSetKey property = staticKey->property(id);
    if (!property.maybeTypes() ||
        !property.maybeTypes()->definiteProperty() ||
        property.nonData(constraints()))
    {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        *psucceeded = false;
        return true;
    }

    // Don't optimize global lexical bindings if they aren't initialized at
    // compile time.
    if (isGlobalLexical && IsUninitializedGlobalLexicalSlot(staticObject, name)) {
        *psucceeded = false;
        return true;
    }

    TemporaryTypeSet* types = bytecodeTypes(pc);
    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(), staticKey,
                                                       name, types, /* updateObserved = */ true);

    JSObject* singleton = types->maybeSingleton();

    MIRType knownType = types->getKnownMIRType();
    if (barrier == BarrierKind::NoBarrier) {
        // Try to inline properties holding a known constant object.
        if (singleton) {
            if (testSingletonProperty(staticObject, id) == singleton)
                return pushConstant(ObjectValue(*singleton));
        }

        // Try to inline properties that have never been overwritten.
        Value constantValue;
        if (property.constant(constraints(), &constantValue))
            return pushConstant(constantValue);

        // Try to inline properties that can only have one value.
        if (knownType == MIRType_Undefined)
            return pushConstant(UndefinedValue());
        if (knownType == MIRType_Null)
            return pushConstant(NullValue());
    }

    MInstruction* obj = constant(ObjectValue(*staticObject));

    MIRType rvalType = types->getKnownMIRType();
    if (barrier != BarrierKind::NoBarrier)
        rvalType = MIRType_Value;

    if (!loadSlot(obj, property.maybeTypes()->definiteSlot(), NumFixedSlots(staticObject),
                  rvalType, barrier, types)) {
        *psucceeded = false;
        return false;
    }

    return true;
}

// Whether a write of the given value may need a post-write barrier for GC purposes.
bool
jit::NeedsPostBarrier(MDefinition* value)
{
    if (!GetJitContext()->runtime->gcNursery().exists())
        return false;
    return value->mightBeType(MIRType_Object);
}

bool
IonBuilder::setStaticName(JSObject* staticObject, PropertyName* name)
{
    jsid id = NameToId(name);

    bool isGlobalLexical = staticObject->is<ClonedBlockObject>() &&
                           staticObject->as<ClonedBlockObject>().isGlobal();
    MOZ_ASSERT(isGlobalLexical ||
               staticObject->is<GlobalObject>() ||
               staticObject->is<CallObject>());

    MDefinition* value = current->peek(-1);

    TypeSet::ObjectKey* staticKey = TypeSet::ObjectKey::get(staticObject);
    if (staticKey->unknownProperties())
        return jsop_setprop(name);

    HeapTypeSetKey property = staticKey->property(id);
    if (!property.maybeTypes() ||
        !property.maybeTypes()->definiteProperty() ||
        property.nonData(constraints()) ||
        property.nonWritable(constraints()))
    {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        return jsop_setprop(name);
    }

    if (!CanWriteProperty(alloc(), constraints(), property, value))
        return jsop_setprop(name);

    // Don't optimize global lexical bindings if they aren't initialized at
    // compile time.
    if (isGlobalLexical && IsUninitializedGlobalLexicalSlot(staticObject, name))
        return jsop_setprop(name);

    current->pop();

    // Pop the bound object on the stack.
    MDefinition* obj = current->pop();
    MOZ_ASSERT(&obj->toConstant()->value().toObject() == staticObject);

    if (NeedsPostBarrier(value))
        current->add(MPostWriteBarrier::New(alloc(), obj, value));

    // If the property has a known type, we may be able to optimize typed stores by not
    // storing the type tag.
    MIRType slotType = MIRType_None;
    MIRType knownType = property.knownMIRType(constraints());
    if (knownType != MIRType_Value)
        slotType = knownType;

    bool needsBarrier = property.needsBarrier(constraints());
    return storeSlot(obj, property.maybeTypes()->definiteSlot(), NumFixedSlots(staticObject),
                     value, needsBarrier, slotType);
}

JSObject*
IonBuilder::testGlobalLexicalBinding(PropertyName* name)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_BINDGNAME ||
               JSOp(*pc) == JSOP_GETGNAME ||
               JSOp(*pc) == JSOP_SETGNAME ||
               JSOp(*pc) == JSOP_STRICTSETGNAME);

    // The global isn't the global lexical scope's prototype, but its
    // enclosing scope. Test for the existence of |name| manually on the
    // global lexical scope. If it is not found, look for it on the global
    // itself.

    NativeObject* obj = &script()->global().lexicalScope();
    TypeSet::ObjectKey* lexicalKey = TypeSet::ObjectKey::get(obj);
    jsid id = NameToId(name);
    if (analysisContext)
        lexicalKey->ensureTrackedProperty(analysisContext, id);
    if (!lexicalKey->unknownProperties()) {
        // If the property is not found on the global lexical scope but it is
        // found on the global and is configurable, freeze the typeset for its
        // non-existence.
        //
        // In the case that it is found on the global but is non-configurable,
        // the binding cannot be shadowed by a global lexical binding.
        HeapTypeSetKey lexicalProperty = lexicalKey->property(id);
        Shape* shape = obj->lookupPure(name);
        if (shape) {
            if ((JSOp(*pc) != JSOP_GETGNAME && !shape->writable()) ||
                obj->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL))
            {
                return nullptr;
            }
        } else {
            shape = script()->global().lookupPure(name);
            if (!shape || shape->configurable())
                MOZ_ALWAYS_FALSE(lexicalProperty.isOwnProperty(constraints()));
            obj = &script()->global();
        }
    }

    return obj;
}

bool
IonBuilder::jsop_getgname(PropertyName* name)
{
    if (JSObject* obj = testGlobalLexicalBinding(name)) {
        bool emitted = false;
        if (!getStaticName(obj, name, &emitted) || emitted)
            return emitted;

        if (!forceInlineCaches() && obj->is<GlobalObject>()) {
            TemporaryTypeSet* types = bytecodeTypes(pc);
            MDefinition* globalObj = constant(ObjectValue(*obj));
            if (!getPropTryCommonGetter(&emitted, globalObj, name, types) || emitted)
                return emitted;
        }
    }

    return jsop_getname(name);
}

bool
IonBuilder::jsop_getname(PropertyName* name)
{
    MDefinition* object;
    if (IsGlobalOp(JSOp(*pc)) && !script()->hasNonSyntacticScope()) {
        MInstruction* global = constant(ObjectValue(script()->global().lexicalScope()));
        object = global;
    } else {
        current->push(current->scopeChain());
        object = current->pop();
    }

    MGetNameCache* ins;
    if (JSOp(*GetNextPc(pc)) == JSOP_TYPEOF)
        ins = MGetNameCache::New(alloc(), object, name, MGetNameCache::NAMETYPEOF);
    else
        ins = MGetNameCache::New(alloc(), object, name, MGetNameCache::NAME);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    TemporaryTypeSet* types = bytecodeTypes(pc);
    return pushTypeBarrier(ins, types, BarrierKind::TypeSet);
}

bool
IonBuilder::jsop_intrinsic(PropertyName* name)
{
    TemporaryTypeSet* types = bytecodeTypes(pc);

    // If we haven't executed this opcode yet, we need to get the intrinsic
    // value and monitor the result.
    if (types->empty()) {
        MCallGetIntrinsicValue* ins = MCallGetIntrinsicValue::New(alloc(), name);

        current->add(ins);
        current->push(ins);

        if (!resumeAfter(ins))
            return false;

        return pushTypeBarrier(ins, types, BarrierKind::TypeSet);
    }

    // Bake in the intrinsic, guaranteed to exist because a non-empty typeset
    // means the intrinsic was successfully gotten in the VM call above.
    // Assert that TI agrees with us on the type.
    Value vp = script()->global().existingIntrinsicValue(name);
    MOZ_ASSERT(types->hasType(TypeSet::GetValueType(vp)));

    pushConstant(vp);
    return true;
}

bool
IonBuilder::jsop_getimport(PropertyName* name)
{
    ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script());
    MOZ_ASSERT(env);

    Shape* shape;
    ModuleEnvironmentObject* targetEnv;
    MOZ_ALWAYS_TRUE(env->lookupImport(NameToId(name), &targetEnv, &shape));

    PropertyName* localName = JSID_TO_STRING(shape->propid())->asAtom().asPropertyName();
    bool emitted = false;
    if (!getStaticName(targetEnv, localName, &emitted))
        return false;

    MOZ_ASSERT(emitted);

    // In the rare case where this import hasn't been initialized already (we
    // have an import cycle where modules reference each other's imports), emit
    // a check.
    if (targetEnv->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MDefinition* checked = addLexicalCheck(current->pop());
        current->push(checked);
    }

    return true;
}

bool
IonBuilder::jsop_bindname(PropertyName* name)
{
    MDefinition* scopeChain;
    if (analysis().usesScopeChain()) {
        scopeChain = current->scopeChain();
    } else {
        // We take the slow path when trying to BINDGNAME a name that resolves
        // to a 'const' or an uninitialized binding.
        MOZ_ASSERT(JSOp(*pc) == JSOP_BINDGNAME);
        scopeChain = constant(ObjectValue(script()->global().lexicalScope()));
    }
    MBindNameCache* ins = MBindNameCache::New(alloc(), scopeChain, name, script(), pc);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

static MIRType
GetElemKnownType(bool needsHoleCheck, TemporaryTypeSet* types)
{
    MIRType knownType = types->getKnownMIRType();

    // Null and undefined have no payload so they can't be specialized.
    // Since folding null/undefined while building SSA is not safe (see the
    // comment in IsPhiObservable), we just add an untyped load instruction
    // and rely on pushTypeBarrier and DCE to replace it with a null/undefined
    // constant.
    if (knownType == MIRType_Undefined || knownType == MIRType_Null)
        knownType = MIRType_Value;

    // Different architectures may want typed element reads which require
    // hole checks to be done as either value or typed reads.
    if (needsHoleCheck && !LIRGenerator::allowTypedElementHoleCheck())
        knownType = MIRType_Value;

    return knownType;
}

bool
IonBuilder::jsop_getelem()
{
    startTrackingOptimizations();

    MDefinition* index = current->pop();
    MDefinition* obj = current->pop();

    trackTypeInfo(TrackedTypeSite::Receiver, obj->type(), obj->resultTypeSet());
    trackTypeInfo(TrackedTypeSite::Index, index->type(), index->resultTypeSet());

    // Always use a call if we are performing analysis and not actually
    // emitting code, to simplify later analysis.
    if (info().isAnalysis() || shouldAbortOnPreliminaryGroups(obj)) {
        MInstruction* ins = MCallGetElement::New(alloc(), obj, index);

        current->add(ins);
        current->push(ins);

        if (!resumeAfter(ins))
            return false;

        TemporaryTypeSet* types = bytecodeTypes(pc);
        return pushTypeBarrier(ins, types, BarrierKind::TypeSet);
    }

    obj = maybeUnboxForPropertyAccess(obj);
    if (obj->type() == MIRType_Object)
        obj = convertUnboxedObjects(obj);

    bool emitted = false;

    if (!forceInlineCaches()) {
        trackOptimizationAttempt(TrackedStrategy::GetElem_TypedObject);
        if (!getElemTryTypedObject(&emitted, obj, index) || emitted)
            return emitted;

        // Note: no trackOptimizationAttempt call is needed, getElemTryGetProp
        // will call it.
        if (!getElemTryGetProp(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_Dense);
        if (!getElemTryDense(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_TypedStatic);
        if (!getElemTryTypedStatic(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_TypedArray);
        if (!getElemTryTypedArray(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_String);
        if (!getElemTryString(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_Arguments);
        if (!getElemTryArguments(&emitted, obj, index) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::GetElem_ArgumentsInlined);
        if (!getElemTryArgumentsInlined(&emitted, obj, index) || emitted)
            return emitted;
    }

    if (script()->argumentsHasVarBinding() && obj->mightBeType(MIRType_MagicOptimizedArguments))
        return abort("Type is not definitely lazy arguments.");

    trackOptimizationAttempt(TrackedStrategy::GetElem_InlineCache);
    if (!getElemTryCache(&emitted, obj, index) || emitted)
        return emitted;

    // Emit call.
    MInstruction* ins = MCallGetElement::New(alloc(), obj, index);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    if (*pc == JSOP_CALLELEM && IsNullOrUndefined(obj->type())) {
        // Due to inlining, it's possible the observed TypeSet is non-empty,
        // even though we know |obj| is null/undefined and the MCallGetElement
        // will throw. Don't push a TypeBarrier in this case, to avoid
        // inlining the following (unreachable) JSOP_CALL.
        return true;
    }

    TemporaryTypeSet* types = bytecodeTypes(pc);
    return pushTypeBarrier(ins, types, BarrierKind::TypeSet);
}

bool
IonBuilder::getElemTryTypedObject(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    // The next several failures are all due to types not predicting that we
    // are definitely doing a getelem access on a typed object.
    trackOptimizationOutcome(TrackedOutcome::AccessNotTypedObject);

    TypedObjectPrediction objPrediction = typedObjectPrediction(obj);
    if (objPrediction.isUseless())
        return true;

    if (!objPrediction.ofArrayKind())
        return true;

    TypedObjectPrediction elemPrediction = objPrediction.arrayElementType();
    if (elemPrediction.isUseless())
        return true;

    int32_t elemSize;
    if (!elemPrediction.hasKnownSize(&elemSize))
        return true;

    switch (elemPrediction.kind()) {
      case type::Simd:
        // FIXME (bug 894105): load into a MIRType_float32x4 etc
        trackOptimizationOutcome(TrackedOutcome::GenericFailure);
        return true;

      case type::Struct:
      case type::Array:
        return getElemTryComplexElemOfTypedObject(emitted,
                                                  obj,
                                                  index,
                                                  objPrediction,
                                                  elemPrediction,
                                                  elemSize);
      case type::Scalar:
        return getElemTryScalarElemOfTypedObject(emitted,
                                                 obj,
                                                 index,
                                                 objPrediction,
                                                 elemPrediction,
                                                 elemSize);

      case type::Reference:
        return getElemTryReferenceElemOfTypedObject(emitted,
                                                    obj,
                                                    index,
                                                    objPrediction,
                                                    elemPrediction);
    }

    MOZ_CRASH("Bad kind");
}

static MIRType
MIRTypeForTypedArrayRead(Scalar::Type arrayType, bool observedDouble);

bool
IonBuilder::checkTypedObjectIndexInBounds(int32_t elemSize,
                                          MDefinition* obj,
                                          MDefinition* index,
                                          TypedObjectPrediction objPrediction,
                                          LinearSum* indexAsByteOffset)
{
    // Ensure index is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), index);
    current->add(idInt32);

    // If we know the length statically from the type, just embed it.
    // Otherwise, load it from the appropriate reserved slot on the
    // typed object.  We know it's an int32, so we can convert from
    // Value to int32 using truncation.
    int32_t lenOfAll;
    MDefinition* length;
    if (objPrediction.hasKnownArrayLength(&lenOfAll)) {
        length = constantInt(lenOfAll);

        // If we are not loading the length from the object itself, only
        // optimize if the array buffer can't have been neutered.
        TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
        if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED)) {
            trackOptimizationOutcome(TrackedOutcome::TypedObjectNeutered);
            return false;
        }
    } else {
        trackOptimizationOutcome(TrackedOutcome::TypedObjectArrayRange);
        return false;
    }

    index = addBoundsCheck(idInt32, length);

    return indexAsByteOffset->add(index, elemSize);
}

bool
IonBuilder::getElemTryScalarElemOfTypedObject(bool* emitted,
                                              MDefinition* obj,
                                              MDefinition* index,
                                              TypedObjectPrediction objPrediction,
                                              TypedObjectPrediction elemPrediction,
                                              int32_t elemSize)
{
    MOZ_ASSERT(objPrediction.ofArrayKind());

    // Must always be loading the same scalar type
    ScalarTypeDescr::Type elemType = elemPrediction.scalarType();
    MOZ_ASSERT(elemSize == ScalarTypeDescr::alignment(elemType));

    LinearSum indexAsByteOffset(alloc());
    if (!checkTypedObjectIndexInBounds(elemSize, obj, index, objPrediction, &indexAsByteOffset))
        return true;

    trackOptimizationSuccess();
    *emitted = true;

    return pushScalarLoadFromTypedObject(obj, indexAsByteOffset, elemType);
}

bool
IonBuilder::getElemTryReferenceElemOfTypedObject(bool* emitted,
                                                 MDefinition* obj,
                                                 MDefinition* index,
                                                 TypedObjectPrediction objPrediction,
                                                 TypedObjectPrediction elemPrediction)
{
    MOZ_ASSERT(objPrediction.ofArrayKind());

    ReferenceTypeDescr::Type elemType = elemPrediction.referenceType();
    size_t elemSize = ReferenceTypeDescr::size(elemType);

    LinearSum indexAsByteOffset(alloc());
    if (!checkTypedObjectIndexInBounds(elemSize, obj, index, objPrediction, &indexAsByteOffset))
        return true;

    trackOptimizationSuccess();
    *emitted = true;

    return pushReferenceLoadFromTypedObject(obj, indexAsByteOffset, elemType, nullptr);
}

bool
IonBuilder::pushScalarLoadFromTypedObject(MDefinition* obj,
                                          const LinearSum& byteOffset,
                                          ScalarTypeDescr::Type elemType)
{
    int32_t size = ScalarTypeDescr::size(elemType);
    MOZ_ASSERT(size == ScalarTypeDescr::alignment(elemType));

    // Find location within the owner object.
    MDefinition* elements;
    MDefinition* scaledOffset;
    int32_t adjustment;
    loadTypedObjectElements(obj, byteOffset, size, &elements, &scaledOffset, &adjustment);

    // Load the element.
    MLoadUnboxedScalar* load = MLoadUnboxedScalar::New(alloc(), elements, scaledOffset,
                                                       elemType,
                                                       DoesNotRequireMemoryBarrier,
                                                       adjustment);
    current->add(load);
    current->push(load);

    // If we are reading in-bounds elements, we can use knowledge about
    // the array type to determine the result type, even if the opcode has
    // never executed. The known pushed type is only used to distinguish
    // uint32 reads that may produce either doubles or integers.
    TemporaryTypeSet* resultTypes = bytecodeTypes(pc);
    bool allowDouble = resultTypes->hasType(TypeSet::DoubleType());

    // Note: knownType is not necessarily in resultTypes; e.g. if we
    // have only observed integers coming out of float array.
    MIRType knownType = MIRTypeForTypedArrayRead(elemType, allowDouble);

    // Note: we can ignore the type barrier here, we know the type must
    // be valid and unbarriered. Also, need not set resultTypeSet,
    // because knownType is scalar and a resultTypeSet would provide
    // no useful additional info.
    load->setResultType(knownType);

    return true;
}

bool
IonBuilder::pushReferenceLoadFromTypedObject(MDefinition* typedObj,
                                             const LinearSum& byteOffset,
                                             ReferenceTypeDescr::Type type,
                                             PropertyName* name)
{
    // Find location within the owner object.
    MDefinition* elements;
    MDefinition* scaledOffset;
    int32_t adjustment;
    size_t alignment = ReferenceTypeDescr::alignment(type);
    loadTypedObjectElements(typedObj, byteOffset, alignment, &elements, &scaledOffset, &adjustment);

    TemporaryTypeSet* observedTypes = bytecodeTypes(pc);

    MInstruction* load = nullptr;  // initialize to silence GCC warning
    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(),
                                                       typedObj, name, observedTypes);

    switch (type) {
      case ReferenceTypeDescr::TYPE_ANY: {
        // Make sure the barrier reflects the possibility of reading undefined.
        bool bailOnUndefined = barrier == BarrierKind::NoBarrier &&
                               !observedTypes->hasType(TypeSet::UndefinedType());
        if (bailOnUndefined)
            barrier = BarrierKind::TypeTagOnly;
        load = MLoadElement::New(alloc(), elements, scaledOffset, false, false, adjustment);
        break;
      }
      case ReferenceTypeDescr::TYPE_OBJECT: {
        // Make sure the barrier reflects the possibility of reading null. When
        // there is no other barrier needed we include the null bailout with
        // MLoadUnboxedObjectOrNull, which avoids the need to box the result
        // for a type barrier instruction.
        MLoadUnboxedObjectOrNull::NullBehavior nullBehavior;
        if (barrier == BarrierKind::NoBarrier && !observedTypes->hasType(TypeSet::NullType()))
            nullBehavior = MLoadUnboxedObjectOrNull::BailOnNull;
        else
            nullBehavior = MLoadUnboxedObjectOrNull::HandleNull;
        load = MLoadUnboxedObjectOrNull::New(alloc(), elements, scaledOffset, nullBehavior,
                                             adjustment);
        break;
      }
      case ReferenceTypeDescr::TYPE_STRING: {
        load = MLoadUnboxedString::New(alloc(), elements, scaledOffset, adjustment);
        observedTypes->addType(TypeSet::StringType(), alloc().lifoAlloc());
        break;
      }
    }

    current->add(load);
    current->push(load);

    return pushTypeBarrier(load, observedTypes, barrier);
}

bool
IonBuilder::getElemTryComplexElemOfTypedObject(bool* emitted,
                                               MDefinition* obj,
                                               MDefinition* index,
                                               TypedObjectPrediction objPrediction,
                                               TypedObjectPrediction elemPrediction,
                                               int32_t elemSize)
{
    MOZ_ASSERT(objPrediction.ofArrayKind());

    MDefinition* type = loadTypedObjectType(obj);
    MDefinition* elemTypeObj = typeObjectForElementFromArrayStructType(type);

    LinearSum indexAsByteOffset(alloc());
    if (!checkTypedObjectIndexInBounds(elemSize, obj, index, objPrediction, &indexAsByteOffset))
        return true;

    return pushDerivedTypedObject(emitted, obj, indexAsByteOffset,
                                  elemPrediction, elemTypeObj);
}

bool
IonBuilder::pushDerivedTypedObject(bool* emitted,
                                   MDefinition* obj,
                                   const LinearSum& baseByteOffset,
                                   TypedObjectPrediction derivedPrediction,
                                   MDefinition* derivedTypeObj)
{
    // Find location within the owner object.
    MDefinition* owner;
    LinearSum ownerByteOffset(alloc());
    loadTypedObjectData(obj, &owner, &ownerByteOffset);

    if (!ownerByteOffset.add(baseByteOffset, 1))
        setForceAbort();

    MDefinition* offset = ConvertLinearSum(alloc(), current, ownerByteOffset,
                                           /* convertConstant = */ true);

    // Create the derived typed object.
    MInstruction* derivedTypedObj = MNewDerivedTypedObject::New(alloc(),
                                                                derivedPrediction,
                                                                derivedTypeObj,
                                                                owner,
                                                                offset);
    current->add(derivedTypedObj);
    current->push(derivedTypedObj);

    // Determine (if possible) the class/proto that `derivedTypedObj` will
    // have. For derived typed objects, the opacity will be the same as the
    // incoming object from which the derived typed object is, well, derived.
    // The prototype will be determined based on the type descriptor (and is
    // immutable).
    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    const Class* expectedClass = nullptr;
    if (const Class* objClass = objTypes ? objTypes->getKnownClass(constraints()) : nullptr) {
        MOZ_ASSERT(IsTypedObjectClass(objClass));
        expectedClass = GetOutlineTypedObjectClass(IsOpaqueTypedObjectClass(objClass));
    }
    const TypedProto* expectedProto = derivedPrediction.getKnownPrototype();
    MOZ_ASSERT_IF(expectedClass, IsTypedObjectClass(expectedClass));

    // Determine (if possible) the class/proto that the observed type set
    // describes.
    TemporaryTypeSet* observedTypes = bytecodeTypes(pc);
    const Class* observedClass = observedTypes->getKnownClass(constraints());

    // If expectedClass/expectedProto are both non-null (and hence known), we
    // can predict precisely what object group derivedTypedObj will have.
    // Therefore, if we observe that this group is already contained in the set
    // of observedTypes, we can skip the barrier.
    //
    // Barriers still wind up being needed in some relatively
    // rare cases:
    //
    // - if multiple kinds of typed objects flow into this point,
    //   in which case we will not be able to predict expectedClass
    //   nor expectedProto.
    //
    // - if the code has never executed, in which case the set of
    //   observed types will be incomplete.
    //
    // Barriers are particularly expensive here because they prevent
    // us from optimizing the MNewDerivedTypedObject away.
    JSObject* observedProto;
    if (observedTypes->getCommonPrototype(constraints(), &observedProto) &&
        observedClass && observedProto && observedClass == expectedClass &&
        observedProto == expectedProto)
    {
        derivedTypedObj->setResultTypeSet(observedTypes);
    } else {
        if (!pushTypeBarrier(derivedTypedObj, observedTypes, BarrierKind::TypeSet))
            return false;
    }

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getElemTryGetProp(bool* emitted, MDefinition* obj, MDefinition* index)
{
    // If index is a constant string or symbol, try to optimize this GETELEM
    // as a GETPROP.

    MOZ_ASSERT(*emitted == false);

    if (!index->isConstantValue())
        return true;

    jsid id;
    if (!ValueToIdPure(index->constantValue(), &id))
        return true;

    if (id != IdToTypeId(id))
        return true;

    TemporaryTypeSet* types = bytecodeTypes(pc);

    trackOptimizationAttempt(TrackedStrategy::GetProp_Constant);
    if (!getPropTryConstant(emitted, obj, id, types) || *emitted) {
        if (*emitted)
            index->setImplicitlyUsedUnchecked();
        return *emitted;
    }

    return true;
}

bool
IonBuilder::getElemTryDense(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    JSValueType unboxedType = UnboxedArrayElementType(constraints(), obj, index);
    if (unboxedType == JSVAL_TYPE_MAGIC) {
        if (!ElementAccessIsDenseNative(constraints(), obj, index)) {
            trackOptimizationOutcome(TrackedOutcome::AccessNotDense);
            return true;
        }
    }

    // Don't generate a fast path if there have been bounds check failures
    // and this access might be on a sparse property.
    if (ElementAccessHasExtraIndexedProperty(this, obj) && failedBoundsCheck_) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return true;
    }

    // Don't generate a fast path if this pc has seen negative indexes accessed,
    // which will not appear to be extra indexed properties.
    if (inspector->hasSeenNegativeIndexGetElement(pc)) {
        trackOptimizationOutcome(TrackedOutcome::ArraySeenNegativeIndex);
        return true;
    }

    if (!jsop_getelem_dense(obj, index, unboxedType))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

JSObject*
IonBuilder::getStaticTypedArrayObject(MDefinition* obj, MDefinition* index)
{
    Scalar::Type arrayType;
    if (!ElementAccessIsAnyTypedArray(constraints(), obj, index, &arrayType)) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotTypedArray);
        return nullptr;
    }

    if (!LIRGenerator::allowStaticTypedArrayAccesses()) {
        trackOptimizationOutcome(TrackedOutcome::Disabled);
        return nullptr;
    }

    if (ElementAccessHasExtraIndexedProperty(this, obj)) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return nullptr;
    }

    if (!obj->resultTypeSet()) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return nullptr;
    }

    JSObject* tarrObj = obj->resultTypeSet()->maybeSingleton();
    if (!tarrObj) {
        trackOptimizationOutcome(TrackedOutcome::NotSingleton);
        return nullptr;
    }

    TypeSet::ObjectKey* tarrKey = TypeSet::ObjectKey::get(tarrObj);
    if (tarrKey->unknownProperties()) {
        trackOptimizationOutcome(TrackedOutcome::UnknownProperties);
        return nullptr;
    }

    return tarrObj;
}

bool
IonBuilder::getElemTryTypedStatic(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    JSObject* tarrObj = getStaticTypedArrayObject(obj, index);
    if (!tarrObj)
        return true;

    // LoadTypedArrayElementStatic currently treats uint32 arrays as int32.
    Scalar::Type viewType = AnyTypedArrayType(tarrObj);
    if (viewType == Scalar::Uint32) {
        trackOptimizationOutcome(TrackedOutcome::StaticTypedArrayUint32);
        return true;
    }

    MDefinition* ptr = convertShiftToMaskForStaticTypedArray(index, viewType);
    if (!ptr)
        return true;

    // Emit LoadTypedArrayElementStatic.

    if (tarrObj->is<TypedArrayObject>()) {
        TypeSet::ObjectKey* tarrKey = TypeSet::ObjectKey::get(tarrObj);
        tarrKey->watchStateChangeForTypedArrayData(constraints());
    }

    obj->setImplicitlyUsedUnchecked();
    index->setImplicitlyUsedUnchecked();

    MLoadTypedArrayElementStatic* load = MLoadTypedArrayElementStatic::New(alloc(), tarrObj, ptr);
    current->add(load);
    current->push(load);

    // The load is infallible if an undefined result will be coerced to the
    // appropriate numeric type if the read is out of bounds. The truncation
    // analysis picks up some of these cases, but is incomplete with respect
    // to others. For now, sniff the bytecode for simple patterns following
    // the load which guarantee a truncation or numeric conversion.
    if (viewType == Scalar::Float32 || viewType == Scalar::Float64) {
        jsbytecode* next = pc + JSOP_GETELEM_LENGTH;
        if (*next == JSOP_POS)
            load->setInfallible();
    } else {
        jsbytecode* next = pc + JSOP_GETELEM_LENGTH;
        if (*next == JSOP_ZERO && *(next + JSOP_ZERO_LENGTH) == JSOP_BITOR)
            load->setInfallible();
    }

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getElemTryTypedArray(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    Scalar::Type arrayType;
    if (!ElementAccessIsAnyTypedArray(constraints(), obj, index, &arrayType)) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotTypedArray);
        return true;
    }

    // Emit typed getelem variant.
    if (!jsop_getelem_typed(obj, index, arrayType))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getElemTryString(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    if (obj->type() != MIRType_String || !IsNumberType(index->type())) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotString);
        return true;
    }

    // If the index is expected to be out-of-bounds, don't optimize to avoid
    // frequent bailouts.
    if (bytecodeTypes(pc)->hasType(TypeSet::UndefinedType())) {
        trackOptimizationOutcome(TrackedOutcome::OutOfBounds);
        return true;
    }

    // Emit fast path for string[index].
    MInstruction* idInt32 = MToInt32::New(alloc(), index);
    current->add(idInt32);
    index = idInt32;

    MStringLength* length = MStringLength::New(alloc(), obj);
    current->add(length);

    index = addBoundsCheck(index, length);

    MCharCodeAt* charCode = MCharCodeAt::New(alloc(), obj, index);
    current->add(charCode);

    MFromCharCode* result = MFromCharCode::New(alloc(), charCode);
    current->add(result);
    current->push(result);

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getElemTryArguments(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    if (inliningDepth_ > 0)
        return true;

    if (obj->type() != MIRType_MagicOptimizedArguments)
        return true;

    // Emit GetFrameArgument.

    MOZ_ASSERT(!info().argsObjAliasesFormals());

    // Type Inference has guaranteed this is an optimized arguments object.
    obj->setImplicitlyUsedUnchecked();

    // To ensure that we are not looking above the number of actual arguments.
    MArgumentsLength* length = MArgumentsLength::New(alloc());
    current->add(length);

    // Ensure index is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), index);
    current->add(idInt32);
    index = idInt32;

    // Bailouts if we read more than the number of actual arguments.
    index = addBoundsCheck(index, length);

    // Load the argument from the actual arguments.
    MGetFrameArgument* load = MGetFrameArgument::New(alloc(), index, analysis_.hasSetArg());
    current->add(load);
    current->push(load);

    TemporaryTypeSet* types = bytecodeTypes(pc);
    if (!pushTypeBarrier(load, types, BarrierKind::TypeSet))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getElemTryArgumentsInlined(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    if (inliningDepth_ == 0)
        return true;

    if (obj->type() != MIRType_MagicOptimizedArguments)
        return true;

    // Emit inlined arguments.
    obj->setImplicitlyUsedUnchecked();

    MOZ_ASSERT(!info().argsObjAliasesFormals());

    // When the id is constant, we can just return the corresponding inlined argument
    if (index->isConstantValue() && index->constantValue().isInt32()) {
        MOZ_ASSERT(inliningDepth_ > 0);

        int32_t id = index->constantValue().toInt32();
        index->setImplicitlyUsedUnchecked();

        if (id < (int32_t)inlineCallInfo_->argc() && id >= 0)
            current->push(inlineCallInfo_->getArg(id));
        else
            pushConstant(UndefinedValue());

        trackOptimizationSuccess();
        *emitted = true;
        return true;
    }

    // inlined not constant not supported, yet.
    return abort("NYI inlined not constant get argument element");
}

bool
IonBuilder::getElemTryCache(bool* emitted, MDefinition* obj, MDefinition* index)
{
    MOZ_ASSERT(*emitted == false);

    // Make sure we have at least an object.
    if (!obj->mightBeType(MIRType_Object)) {
        trackOptimizationOutcome(TrackedOutcome::NotObject);
        return true;
    }

    // Don't cache for strings.
    if (obj->mightBeType(MIRType_String)) {
        trackOptimizationOutcome(TrackedOutcome::GetElemStringNotCached);
        return true;
    }

    // Index should be integer, string, or symbol
    if (!index->mightBeType(MIRType_Int32) &&
        !index->mightBeType(MIRType_String) &&
        !index->mightBeType(MIRType_Symbol))
    {
        trackOptimizationOutcome(TrackedOutcome::IndexType);
        return true;
    }

    // Turn off cacheing if the element is int32 and we've seen non-native objects as the target
    // of this getelem.
    bool nonNativeGetElement = inspector->hasSeenNonNativeGetElement(pc);
    if (index->mightBeType(MIRType_Int32) && nonNativeGetElement) {
        trackOptimizationOutcome(TrackedOutcome::NonNativeReceiver);
        return true;
    }

    // Emit GetElementCache.

    TemporaryTypeSet* types = bytecodeTypes(pc);
    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(), obj,
                                                       nullptr, types);

    // Always add a barrier if the index might be a string or symbol, so that
    // the cache can attach stubs for particular properties.
    if (index->mightBeType(MIRType_String) || index->mightBeType(MIRType_Symbol))
        barrier = BarrierKind::TypeSet;

    MGetPropertyCache* ins = MGetPropertyCache::New(alloc(), obj, index,
                                                    barrier == BarrierKind::TypeSet);
    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    // Spice up type information.
    if (index->type() == MIRType_Int32 && barrier == BarrierKind::NoBarrier) {
        bool needHoleCheck = !ElementAccessIsPacked(constraints(), obj);
        MIRType knownType = GetElemKnownType(needHoleCheck, types);

        if (knownType != MIRType_Value && knownType != MIRType_Double)
            ins->setResultType(knownType);
    }

    if (!pushTypeBarrier(ins, types, barrier))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

TemporaryTypeSet*
IonBuilder::computeHeapType(const TemporaryTypeSet* objTypes, const jsid id)
{
    if (objTypes->unknownObject() || objTypes->getObjectCount() == 0)
        return nullptr;

    TemporaryTypeSet empty;
    TemporaryTypeSet* acc = &empty;
    LifoAlloc* lifoAlloc = alloc().lifoAlloc();

    Vector<HeapTypeSetKey, 4, SystemAllocPolicy> properties;
    if (!properties.reserve(objTypes->getObjectCount()))
        return nullptr;

    for (unsigned i = 0; i < objTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = objTypes->getObject(i);

        if (key->unknownProperties())
            return nullptr;

        HeapTypeSetKey property = key->property(id);
        HeapTypeSet* currentSet = property.maybeTypes();

        if (!currentSet || currentSet->unknown())
            return nullptr;

        properties.infallibleAppend(property);
        acc = TypeSet::unionSets(acc, currentSet, lifoAlloc);
        if (!acc)
            return nullptr;
    }

    // Freeze all the properties associated with the refined type set.
    for (HeapTypeSetKey* i = properties.begin(); i != properties.end(); i++)
        i->freeze(constraints());

    return acc;
}

bool
IonBuilder::jsop_getelem_dense(MDefinition* obj, MDefinition* index, JSValueType unboxedType)
{
    TemporaryTypeSet* types = bytecodeTypes(pc);

    MOZ_ASSERT(index->type() == MIRType_Int32 || index->type() == MIRType_Double);
    if (JSOp(*pc) == JSOP_CALLELEM) {
        // Indexed call on an element of an array. Populate the observed types
        // with any objects that could be in the array, to avoid extraneous
        // type barriers.
        AddObjectsForPropertyRead(obj, nullptr, types);
    }

    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(), obj,
                                                       nullptr, types);
    bool needsHoleCheck = !ElementAccessIsPacked(constraints(), obj);

    // Reads which are on holes in the object do not have to bail out if
    // undefined values have been observed at this access site and the access
    // cannot hit another indexed property on the object or its prototypes.
    bool readOutOfBounds =
        types->hasType(TypeSet::UndefinedType()) &&
        !ElementAccessHasExtraIndexedProperty(this, obj);

    MIRType knownType = MIRType_Value;
    if (unboxedType == JSVAL_TYPE_MAGIC && barrier == BarrierKind::NoBarrier)
        knownType = GetElemKnownType(needsHoleCheck, types);

    // Ensure index is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), index);
    current->add(idInt32);
    index = idInt32;

    // Get the elements vector.
    MInstruction* elements = MElements::New(alloc(), obj, unboxedType != JSVAL_TYPE_MAGIC);
    current->add(elements);

    // Note: to help GVN, use the original MElements instruction and not
    // MConvertElementsToDoubles as operand. This is fine because converting
    // elements to double does not change the initialized length.
    MInstruction* initLength = initializedLength(obj, elements, unboxedType);

    // If we can load the element as a definite double, make sure to check that
    // the array has been converted to homogenous doubles first.
    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    bool inBounds = !readOutOfBounds && !needsHoleCheck;

    if (inBounds) {
        TemporaryTypeSet* heapTypes = computeHeapType(objTypes, JSID_VOID);
        if (heapTypes && heapTypes->isSubset(types)) {
            knownType = heapTypes->getKnownMIRType();
            types = heapTypes;
        }
    }

    bool loadDouble =
        unboxedType == JSVAL_TYPE_MAGIC &&
        barrier == BarrierKind::NoBarrier &&
        loopDepth_ &&
        inBounds &&
        knownType == MIRType_Double &&
        objTypes &&
        objTypes->convertDoubleElements(constraints()) == TemporaryTypeSet::AlwaysConvertToDoubles;
    if (loadDouble)
        elements = addConvertElementsToDoubles(elements);

    MInstruction* load;

    if (!readOutOfBounds) {
        // This load should not return undefined, so likely we're reading
        // in-bounds elements, and the array is packed or its holes are not
        // read. This is the best case: we can separate the bounds check for
        // hoisting.
        index = addBoundsCheck(index, initLength);

        if (unboxedType != JSVAL_TYPE_MAGIC) {
            load = loadUnboxedValue(elements, 0, index, unboxedType, barrier, types);
        } else {
            load = MLoadElement::New(alloc(), elements, index, needsHoleCheck, loadDouble);
            current->add(load);
        }
    } else {
        // This load may return undefined, so assume that we *can* read holes,
        // or that we can read out-of-bounds accesses. In this case, the bounds
        // check is part of the opcode.
        load = MLoadElementHole::New(alloc(), elements, index, initLength,
                                     unboxedType, needsHoleCheck);
        current->add(load);

        // If maybeUndefined was true, the typeset must have undefined, and
        // then either additional types or a barrier. This means we should
        // never have a typed version of LoadElementHole.
        MOZ_ASSERT(knownType == MIRType_Value);
    }

    if (knownType != MIRType_Value) {
        load->setResultType(knownType);
        load->setResultTypeSet(types);
    }

    current->push(load);
    return pushTypeBarrier(load, types, barrier);
}

void
IonBuilder::addTypedArrayLengthAndData(MDefinition* obj,
                                       BoundsChecking checking,
                                       MDefinition** index,
                                       MInstruction** length, MInstruction** elements)
{
    MOZ_ASSERT((index != nullptr) == (elements != nullptr));
    JSObject* tarr = nullptr;

    if (obj->isConstantValue() && obj->constantValue().isObject())
        tarr = &obj->constantValue().toObject();
    else if (obj->resultTypeSet())
        tarr = obj->resultTypeSet()->maybeSingleton();

    if (tarr) {
        SharedMem<void*> data = AnyTypedArrayViewData(tarr);
        // Bug 979449 - Optimistically embed the elements and use TI to
        //              invalidate if we move them.
        bool isTenured = !tarr->runtimeFromMainThread()->gc.nursery.isInside(data);
        if (isTenured && tarr->isSingleton()) {
            // The 'data' pointer of TypedArrayObject can change in rare circumstances
            // (ArrayBufferObject::changeContents).
            TypeSet::ObjectKey* tarrKey = TypeSet::ObjectKey::get(tarr);
            if (!tarrKey->unknownProperties()) {
                if (tarr->is<TypedArrayObject>())
                    tarrKey->watchStateChangeForTypedArrayData(constraints());

                obj->setImplicitlyUsedUnchecked();

                int32_t len = AssertedCast<int32_t>(AnyTypedArrayLength(tarr));
                *length = MConstant::New(alloc(), Int32Value(len));
                current->add(*length);

                if (index) {
                    if (checking == DoBoundsCheck)
                        *index = addBoundsCheck(*index, *length);

                    *elements = MConstantElements::New(alloc(), data);
                    current->add(*elements);
                }
                return;
            }
        }
    }

    *length = MTypedArrayLength::New(alloc(), obj);
    current->add(*length);

    if (index) {
        if (checking == DoBoundsCheck)
            *index = addBoundsCheck(*index, *length);

        *elements = MTypedArrayElements::New(alloc(), obj);
        current->add(*elements);
    }
}

MDefinition*
IonBuilder::convertShiftToMaskForStaticTypedArray(MDefinition* id,
                                                  Scalar::Type viewType)
{
    trackOptimizationOutcome(TrackedOutcome::StaticTypedArrayCantComputeMask);

    // No shifting is necessary if the typed array has single byte elements.
    if (TypedArrayShift(viewType) == 0)
        return id;

    // If the index is an already shifted constant, undo the shift to get the
    // absolute offset being accessed.
    if (id->isConstantValue() && id->constantValue().isInt32()) {
        int32_t index = id->constantValue().toInt32();
        MConstant* offset = MConstant::New(alloc(), Int32Value(index << TypedArrayShift(viewType)));
        current->add(offset);
        return offset;
    }

    if (!id->isRsh() || id->isEffectful())
        return nullptr;
    if (!id->getOperand(1)->isConstantValue())
        return nullptr;
    const Value& value = id->getOperand(1)->constantValue();
    if (!value.isInt32() || uint32_t(value.toInt32()) != TypedArrayShift(viewType))
        return nullptr;

    // Instead of shifting, mask off the low bits of the index so that
    // a non-scaled access on the typed array can be performed.
    MConstant* mask = MConstant::New(alloc(), Int32Value(~((1 << value.toInt32()) - 1)));
    MBitAnd* ptr = MBitAnd::New(alloc(), id->getOperand(0), mask);

    ptr->infer(nullptr, nullptr);
    MOZ_ASSERT(!ptr->isEffectful());

    current->add(mask);
    current->add(ptr);

    return ptr;
}

static MIRType
MIRTypeForTypedArrayRead(Scalar::Type arrayType, bool observedDouble)
{
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
        return MIRType_Int32;
      case Scalar::Uint32:
        return observedDouble ? MIRType_Double : MIRType_Int32;
      case Scalar::Float32:
        return MIRType_Float32;
      case Scalar::Float64:
        return MIRType_Double;
      default:
        break;
    }
    MOZ_CRASH("Unknown typed array type");
}

bool
IonBuilder::jsop_getelem_typed(MDefinition* obj, MDefinition* index,
                               Scalar::Type arrayType)
{
    TemporaryTypeSet* types = bytecodeTypes(pc);

    bool maybeUndefined = types->hasType(TypeSet::UndefinedType());

    // Reading from an Uint32Array will result in a double for values
    // that don't fit in an int32. We have to bailout if this happens
    // and the instruction is not known to return a double.
    bool allowDouble = types->hasType(TypeSet::DoubleType());

    // Ensure id is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), index);
    current->add(idInt32);
    index = idInt32;

    if (!maybeUndefined) {
        // Assume the index is in range, so that we can hoist the length,
        // elements vector and bounds check.

        // If we are reading in-bounds elements, we can use knowledge about
        // the array type to determine the result type, even if the opcode has
        // never executed. The known pushed type is only used to distinguish
        // uint32 reads that may produce either doubles or integers.
        MIRType knownType = MIRTypeForTypedArrayRead(arrayType, allowDouble);

        // Get length, bounds-check, then get elements, and add all instructions.
        MInstruction* length;
        MInstruction* elements;
        addTypedArrayLengthAndData(obj, DoBoundsCheck, &index, &length, &elements);

        // Load the element.
        MLoadUnboxedScalar* load = MLoadUnboxedScalar::New(alloc(), elements, index, arrayType);
        current->add(load);
        current->push(load);

        // Note: we can ignore the type barrier here, we know the type must
        // be valid and unbarriered.
        load->setResultType(knownType);
        return true;
    } else {
        // We need a type barrier if the array's element type has never been
        // observed (we've only read out-of-bounds values). Note that for
        // Uint32Array, we only check for int32: if allowDouble is false we
        // will bailout when we read a double.
        BarrierKind barrier = BarrierKind::TypeSet;
        switch (arrayType) {
          case Scalar::Int8:
          case Scalar::Uint8:
          case Scalar::Uint8Clamped:
          case Scalar::Int16:
          case Scalar::Uint16:
          case Scalar::Int32:
          case Scalar::Uint32:
            if (types->hasType(TypeSet::Int32Type()))
                barrier = BarrierKind::NoBarrier;
            break;
          case Scalar::Float32:
          case Scalar::Float64:
            if (allowDouble)
                barrier = BarrierKind::NoBarrier;
            break;
          default:
            MOZ_CRASH("Unknown typed array type");
        }

        // Assume we will read out-of-bound values. In this case the
        // bounds check will be part of the instruction, and the instruction
        // will always return a Value.
        MLoadTypedArrayElementHole* load =
            MLoadTypedArrayElementHole::New(alloc(), obj, index, arrayType, allowDouble);
        current->add(load);
        current->push(load);

        return pushTypeBarrier(load, types, barrier);
    }
}

bool
IonBuilder::jsop_setelem()
{
    bool emitted = false;
    startTrackingOptimizations();

    MDefinition* value = current->pop();
    MDefinition* index = current->pop();
    MDefinition* object = convertUnboxedObjects(current->pop());

    trackTypeInfo(TrackedTypeSite::Receiver, object->type(), object->resultTypeSet());
    trackTypeInfo(TrackedTypeSite::Index, index->type(), index->resultTypeSet());
    trackTypeInfo(TrackedTypeSite::Value, value->type(), value->resultTypeSet());

    if (shouldAbortOnPreliminaryGroups(object)) {
        MInstruction* ins = MCallSetElement::New(alloc(), object, index, value, IsStrictSetPC(pc));
        current->add(ins);
        current->push(value);
        return resumeAfter(ins);
    }

    if (!forceInlineCaches()) {
        trackOptimizationAttempt(TrackedStrategy::SetElem_TypedObject);
        if (!setElemTryTypedObject(&emitted, object, index, value) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::SetElem_TypedStatic);
        if (!setElemTryTypedStatic(&emitted, object, index, value) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::SetElem_TypedArray);
        if (!setElemTryTypedArray(&emitted, object, index, value) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::SetElem_Dense);
        SetElemICInspector icInspect(inspector->setElemICInspector(pc));
        bool writeHole = icInspect.sawOOBDenseWrite();
        if (!setElemTryDense(&emitted, object, index, value, writeHole) || emitted)
            return emitted;

        trackOptimizationAttempt(TrackedStrategy::SetElem_Arguments);
        if (!setElemTryArguments(&emitted, object, index, value) || emitted)
            return emitted;
    }

    if (script()->argumentsHasVarBinding() &&
        object->mightBeType(MIRType_MagicOptimizedArguments) &&
        info().analysisMode() != Analysis_ArgumentsUsage)
    {
        return abort("Type is not definitely lazy arguments.");
    }

    trackOptimizationAttempt(TrackedStrategy::SetElem_InlineCache);
    if (!setElemTryCache(&emitted, object, index, value) || emitted)
        return emitted;

    // Emit call.
    MInstruction* ins = MCallSetElement::New(alloc(), object, index, value, IsStrictSetPC(pc));
    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::setElemTryTypedObject(bool* emitted, MDefinition* obj,
                                  MDefinition* index, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    // The next several failures are all due to types not predicting that we
    // are definitely doing a getelem access on a typed object.
    trackOptimizationOutcome(TrackedOutcome::AccessNotTypedObject);

    TypedObjectPrediction objPrediction = typedObjectPrediction(obj);
    if (objPrediction.isUseless())
        return true;

    if (!objPrediction.ofArrayKind())
        return true;

    TypedObjectPrediction elemPrediction = objPrediction.arrayElementType();
    if (elemPrediction.isUseless())
        return true;

    int32_t elemSize;
    if (!elemPrediction.hasKnownSize(&elemSize))
        return true;

    switch (elemPrediction.kind()) {
      case type::Simd:
        // FIXME (bug 894105): store a MIRType_float32x4 etc
        trackOptimizationOutcome(TrackedOutcome::GenericFailure);
        return true;

      case type::Reference:
        return setElemTryReferenceElemOfTypedObject(emitted, obj, index,
                                                    objPrediction, value, elemPrediction);

      case type::Scalar:
        return setElemTryScalarElemOfTypedObject(emitted,
                                                 obj,
                                                 index,
                                                 objPrediction,
                                                 value,
                                                 elemPrediction,
                                                 elemSize);

      case type::Struct:
      case type::Array:
        // Not yet optimized.
        trackOptimizationOutcome(TrackedOutcome::GenericFailure);
        return true;
    }

    MOZ_CRASH("Bad kind");
}

bool
IonBuilder::setElemTryReferenceElemOfTypedObject(bool* emitted,
                                                 MDefinition* obj,
                                                 MDefinition* index,
                                                 TypedObjectPrediction objPrediction,
                                                 MDefinition* value,
                                                 TypedObjectPrediction elemPrediction)
{
    ReferenceTypeDescr::Type elemType = elemPrediction.referenceType();
    size_t elemSize = ReferenceTypeDescr::size(elemType);

    LinearSum indexAsByteOffset(alloc());
    if (!checkTypedObjectIndexInBounds(elemSize, obj, index, objPrediction, &indexAsByteOffset))
        return true;

    if (!storeReferenceTypedObjectValue(obj, indexAsByteOffset, elemType, value, nullptr))
        return true;

    current->push(value);

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setElemTryScalarElemOfTypedObject(bool* emitted,
                                              MDefinition* obj,
                                              MDefinition* index,
                                              TypedObjectPrediction objPrediction,
                                              MDefinition* value,
                                              TypedObjectPrediction elemPrediction,
                                              int32_t elemSize)
{
    // Must always be loading the same scalar type
    ScalarTypeDescr::Type elemType = elemPrediction.scalarType();
    MOZ_ASSERT(elemSize == ScalarTypeDescr::alignment(elemType));

    LinearSum indexAsByteOffset(alloc());
    if (!checkTypedObjectIndexInBounds(elemSize, obj, index, objPrediction, &indexAsByteOffset))
        return true;

    // Store the element
    if (!storeScalarTypedObjectValue(obj, indexAsByteOffset, elemType, value))
        return false;

    current->push(value);

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setElemTryTypedStatic(bool* emitted, MDefinition* object,
                                  MDefinition* index, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    JSObject* tarrObj = getStaticTypedArrayObject(object, index);
    if (!tarrObj)
        return true;

    if (tarrObj->runtimeFromMainThread()->gc.nursery.isInside(AnyTypedArrayViewData(tarrObj)))
        return true;

    Scalar::Type viewType = AnyTypedArrayType(tarrObj);
    MDefinition* ptr = convertShiftToMaskForStaticTypedArray(index, viewType);
    if (!ptr)
        return true;

    // Emit StoreTypedArrayElementStatic.

    if (tarrObj->is<TypedArrayObject>()) {
        TypeSet::ObjectKey* tarrKey = TypeSet::ObjectKey::get(tarrObj);
        tarrKey->watchStateChangeForTypedArrayData(constraints());
    }

    object->setImplicitlyUsedUnchecked();
    index->setImplicitlyUsedUnchecked();

    // Clamp value to [0, 255] for Uint8ClampedArray.
    MDefinition* toWrite = value;
    if (viewType == Scalar::Uint8Clamped) {
        toWrite = MClampToUint8::New(alloc(), value);
        current->add(toWrite->toInstruction());
    }

    MInstruction* store = MStoreTypedArrayElementStatic::New(alloc(), tarrObj, ptr, toWrite);
    current->add(store);
    current->push(value);

    if (!resumeAfter(store))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setElemTryTypedArray(bool* emitted, MDefinition* object,
                                 MDefinition* index, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    Scalar::Type arrayType;
    if (!ElementAccessIsAnyTypedArray(constraints(), object, index, &arrayType)) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotTypedArray);
        return true;
    }

    // Emit typed setelem variant.
    if (!jsop_setelem_typed(arrayType, object, index, value))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setElemTryDense(bool* emitted, MDefinition* object,
                            MDefinition* index, MDefinition* value, bool writeHole)
{
    MOZ_ASSERT(*emitted == false);

    JSValueType unboxedType = UnboxedArrayElementType(constraints(), object, index);
    if (unboxedType == JSVAL_TYPE_MAGIC) {
        if (!ElementAccessIsDenseNative(constraints(), object, index)) {
            trackOptimizationOutcome(TrackedOutcome::AccessNotDense);
            return true;
        }
    }

    if (PropertyWriteNeedsTypeBarrier(alloc(), constraints(), current,
                                      &object, nullptr, &value, /* canModify = */ true))
    {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
        return true;
    }

    if (!object->resultTypeSet()) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return true;
    }

    TemporaryTypeSet::DoubleConversion conversion =
        object->resultTypeSet()->convertDoubleElements(constraints());

    // If AmbiguousDoubleConversion, only handle int32 values for now.
    if (conversion == TemporaryTypeSet::AmbiguousDoubleConversion &&
        value->type() != MIRType_Int32)
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayDoubleConversion);
        return true;
    }

    // Don't generate a fast path if there have been bounds check failures
    // and this access might be on a sparse property.
    if (ElementAccessHasExtraIndexedProperty(this, object) && failedBoundsCheck_) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return true;
    }

    // Emit dense setelem variant.
    if (!jsop_setelem_dense(conversion, object, index, value, unboxedType, writeHole))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setElemTryArguments(bool* emitted, MDefinition* object,
                                MDefinition* index, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    if (object->type() != MIRType_MagicOptimizedArguments)
        return true;

    // Arguments are not supported yet.
    return abort("NYI arguments[]=");
}

bool
IonBuilder::setElemTryCache(bool* emitted, MDefinition* object,
                            MDefinition* index, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    if (!object->mightBeType(MIRType_Object)) {
        trackOptimizationOutcome(TrackedOutcome::NotObject);
        return true;
    }

    if (!index->mightBeType(MIRType_Int32) &&
        !index->mightBeType(MIRType_String) &&
        !index->mightBeType(MIRType_Symbol))
    {
        trackOptimizationOutcome(TrackedOutcome::IndexType);
        return true;
    }

    bool barrier = true;

    if (index->type() == MIRType_Int32 &&
        !PropertyWriteNeedsTypeBarrier(alloc(), constraints(), current,
                                       &object, nullptr, &value, /* canModify = */ true))
    {
        barrier = false;
    }

    // We can avoid worrying about holes in the IC if we know a priori we are safe
    // from them. If TI can guard that there are no indexed properties on the prototype
    // chain, we know that we anen't missing any setters by overwriting the hole with
    // another value.
    bool guardHoles = ElementAccessHasExtraIndexedProperty(this, object);

    // Make sure the object being written to doesn't have copy on write elements.
    const Class* clasp = object->resultTypeSet() ? object->resultTypeSet()->getKnownClass(constraints()) : nullptr;
    bool checkNative = !clasp || !clasp->isNative();
    object = addMaybeCopyElementsForWrite(object, checkNative);

    if (NeedsPostBarrier(value))
        current->add(MPostWriteBarrier::New(alloc(), object, value));

    // Emit SetPropertyCache.
    bool strict = JSOp(*pc) == JSOP_STRICTSETELEM;
    MSetPropertyCache* ins =
        MSetPropertyCache::New(alloc(), object, index, value, strict, barrier, guardHoles);
    current->add(ins);
    current->push(value);

    if (!resumeAfter(ins))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_setelem_dense(TemporaryTypeSet::DoubleConversion conversion,
                               MDefinition* obj, MDefinition* id, MDefinition* value,
                               JSValueType unboxedType, bool writeHole)
{
    MIRType elementType = MIRType_None;
    if (unboxedType == JSVAL_TYPE_MAGIC)
        elementType = DenseNativeElementType(constraints(), obj);
    bool packed = ElementAccessIsPacked(constraints(), obj);

    // Writes which are on holes in the object do not have to bail out if they
    // cannot hit another indexed property on the object or its prototypes.
    bool writeOutOfBounds = !ElementAccessHasExtraIndexedProperty(this, obj);

    if (NeedsPostBarrier(value))
        current->add(MPostWriteBarrier::New(alloc(), obj, value));

    // Ensure id is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), id);
    current->add(idInt32);
    id = idInt32;

    // Copy the elements vector if necessary.
    obj = addMaybeCopyElementsForWrite(obj, /* checkNative = */ false);

    // Get the elements vector.
    MElements* elements = MElements::New(alloc(), obj, unboxedType != JSVAL_TYPE_MAGIC);
    current->add(elements);

    // Ensure the value is a double, if double conversion might be needed.
    MDefinition* newValue = value;
    switch (conversion) {
      case TemporaryTypeSet::AlwaysConvertToDoubles:
      case TemporaryTypeSet::MaybeConvertToDoubles: {
        MInstruction* valueDouble = MToDouble::New(alloc(), value);
        current->add(valueDouble);
        newValue = valueDouble;
        break;
      }

      case TemporaryTypeSet::AmbiguousDoubleConversion: {
        MOZ_ASSERT(value->type() == MIRType_Int32);
        MInstruction* maybeDouble = MMaybeToDoubleElement::New(alloc(), elements, value);
        current->add(maybeDouble);
        newValue = maybeDouble;
        break;
      }

      case TemporaryTypeSet::DontConvertToDoubles:
        break;

      default:
        MOZ_CRASH("Unknown double conversion");
    }

    // Use MStoreElementHole if this SETELEM has written to out-of-bounds
    // indexes in the past. Otherwise, use MStoreElement so that we can hoist
    // the initialized length and bounds check.
    MInstruction* store;
    MStoreElementCommon *common = nullptr;
    if (writeHole && writeOutOfBounds) {
        MStoreElementHole* ins = MStoreElementHole::New(alloc(), obj, elements, id, newValue, unboxedType);
        store = ins;
        common = ins;

        current->add(ins);
        current->push(value);
    } else {
        MInstruction* initLength = initializedLength(obj, elements, unboxedType);

        id = addBoundsCheck(id, initLength);
        bool needsHoleCheck = !packed && !writeOutOfBounds;

        if (unboxedType != JSVAL_TYPE_MAGIC) {
            store = storeUnboxedValue(obj, elements, 0, id, unboxedType, newValue);
        } else {
            MStoreElement* ins = MStoreElement::New(alloc(), elements, id, newValue, needsHoleCheck);
            store = ins;
            common = ins;

            current->add(store);
        }

        current->push(value);
    }

    if (!resumeAfter(store))
        return false;

    if (common) {
        // Determine whether a write barrier is required.
        if (obj->resultTypeSet()->propertyNeedsBarrier(constraints(), JSID_VOID))
            common->setNeedsBarrier();

        if (elementType != MIRType_None && packed)
            common->setElementType(elementType);
    }

    return true;
}


bool
IonBuilder::jsop_setelem_typed(Scalar::Type arrayType,
                               MDefinition* obj, MDefinition* id, MDefinition* value)
{
    SetElemICInspector icInspect(inspector->setElemICInspector(pc));
    bool expectOOB = icInspect.sawOOBTypedArrayWrite();

    if (expectOOB)
        spew("Emitting OOB TypedArray SetElem");

    // Ensure id is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), id);
    current->add(idInt32);
    id = idInt32;

    // Get length, bounds-check, then get elements, and add all instructions.
    MInstruction* length;
    MInstruction* elements;
    BoundsChecking checking = expectOOB ? SkipBoundsCheck : DoBoundsCheck;
    addTypedArrayLengthAndData(obj, checking, &id, &length, &elements);

    // Clamp value to [0, 255] for Uint8ClampedArray.
    MDefinition* toWrite = value;
    if (arrayType == Scalar::Uint8Clamped) {
        toWrite = MClampToUint8::New(alloc(), value);
        current->add(toWrite->toInstruction());
    }

    // Store the value.
    MInstruction* ins;
    if (expectOOB) {
        ins = MStoreTypedArrayElementHole::New(alloc(), elements, length, id, toWrite, arrayType);
    } else {
        MStoreUnboxedScalar* store =
            MStoreUnboxedScalar::New(alloc(), elements, id, toWrite, arrayType,
                                     MStoreUnboxedScalar::TruncateInput);
        ins = store;
    }

    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_length()
{
    if (jsop_length_fastPath())
        return true;

    PropertyName* name = info().getAtom(pc)->asPropertyName();
    return jsop_getprop(name);
}

bool
IonBuilder::jsop_length_fastPath()
{
    TemporaryTypeSet* types = bytecodeTypes(pc);

    if (types->getKnownMIRType() != MIRType_Int32)
        return false;

    MDefinition* obj = current->peek(-1);

    if (shouldAbortOnPreliminaryGroups(obj))
        return false;

    if (obj->mightBeType(MIRType_String)) {
        if (obj->mightBeType(MIRType_Object))
            return false;
        current->pop();
        MStringLength* ins = MStringLength::New(alloc(), obj);
        current->add(ins);
        current->push(ins);
        return true;
    }

    if (obj->mightBeType(MIRType_Object)) {
        TemporaryTypeSet* objTypes = obj->resultTypeSet();

        // Compute the length for array objects.
        if (objTypes &&
            objTypes->getKnownClass(constraints()) == &ArrayObject::class_ &&
            !objTypes->hasObjectFlags(constraints(), OBJECT_FLAG_LENGTH_OVERFLOW))
        {
            current->pop();
            MElements* elements = MElements::New(alloc(), obj);
            current->add(elements);

            // Read length.
            MArrayLength* length = MArrayLength::New(alloc(), elements);
            current->add(length);
            current->push(length);
            return true;
        }

        // Compute the length for unboxed array objects.
        if (UnboxedArrayElementType(constraints(), obj, nullptr) != JSVAL_TYPE_MAGIC &&
            !objTypes->hasObjectFlags(constraints(), OBJECT_FLAG_LENGTH_OVERFLOW))
        {
            current->pop();

            MUnboxedArrayLength* length = MUnboxedArrayLength::New(alloc(), obj);
            current->add(length);
            current->push(length);
            return true;
        }

        // Compute the length for array typed objects.
        TypedObjectPrediction prediction = typedObjectPrediction(obj);
        if (!prediction.isUseless()) {
            TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
            if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
                return false;

            MInstruction* length;
            int32_t sizedLength;
            if (prediction.hasKnownArrayLength(&sizedLength)) {
                obj->setImplicitlyUsedUnchecked();
                length = MConstant::New(alloc(), Int32Value(sizedLength));
            } else {
                return false;
            }

            current->pop();
            current->add(length);
            current->push(length);
            return true;
        }
    }

    return false;
}

bool
IonBuilder::jsop_arguments()
{
    if (info().needsArgsObj()) {
        current->push(current->argumentsObject());
        return true;
    }
    MOZ_ASSERT(lazyArguments_);
    current->push(lazyArguments_);
    return true;
}

bool
IonBuilder::jsop_newtarget()
{
    if (!info().funMaybeLazy()) {
        MOZ_ASSERT(!info().script()->isForEval());
        pushConstant(NullValue());
        return true;
    }

    MOZ_ASSERT(info().funMaybeLazy());

    if (info().funMaybeLazy()->isArrow()) {
        MArrowNewTarget* arrowNewTarget = MArrowNewTarget::New(alloc(), getCallee());
        current->add(arrowNewTarget);
        current->push(arrowNewTarget);
        return true;
    }

    if (inliningDepth_ == 0) {
        MNewTarget* newTarget = MNewTarget::New(alloc());
        current->add(newTarget);
        current->push(newTarget);
        return true;
    }

    if (!info().constructing()) {
        pushConstant(UndefinedValue());
        return true;
    }

    current->push(inlineCallInfo_->getNewTarget());
    return true;
}

bool
IonBuilder::jsop_rest()
{
    if (info().analysisMode() == Analysis_ArgumentsUsage) {
        // There's no BaselineScript with the template object. Just push a
        // dummy value, it does not affect the arguments analysis.
        MUnknownValue* unknown = MUnknownValue::New(alloc());
        current->add(unknown);
        current->push(unknown);
        return true;
    }

    ArrayObject* templateObject = &inspector->getTemplateObject(pc)->as<ArrayObject>();

    if (inliningDepth_ == 0) {
        // We don't know anything about the callee.
        MArgumentsLength* numActuals = MArgumentsLength::New(alloc());
        current->add(numActuals);

        // Pass in the number of actual arguments, the number of formals (not
        // including the rest parameter slot itself), and the template object.
        MRest* rest = MRest::New(alloc(), constraints(), numActuals, info().nargs() - 1,
                                 templateObject);
        current->add(rest);
        current->push(rest);
        return true;
    }

    // We know the exact number of arguments the callee pushed.
    unsigned numActuals = inlineCallInfo_->argc();
    unsigned numFormals = info().nargs() - 1;
    unsigned numRest = numActuals > numFormals ? numActuals - numFormals : 0;

    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    current->add(templateConst);

    MNewArray* array = MNewArray::New(alloc(), constraints(), numRest, templateConst,
                                      templateObject->group()->initialHeap(constraints()), pc);
    current->add(array);

    if (numRest == 0) {
        // No more updating to do. (Note that in this one case the length from
        // the template object is already correct.)
        current->push(array);
        return true;
    }

    MElements* elements = MElements::New(alloc(), array);
    current->add(elements);

    // Unroll the argument copy loop. We don't need to do any bounds or hole
    // checking here.
    MConstant* index = nullptr;
    for (unsigned i = numFormals; i < numActuals; i++) {
        index = MConstant::New(alloc(), Int32Value(i - numFormals));
        current->add(index);

        MDefinition* arg = inlineCallInfo_->argv()[i];
        MStoreElement* store = MStoreElement::New(alloc(), elements, index, arg,
                                                  /* needsHoleCheck = */ false);
        current->add(store);

        if (NeedsPostBarrier(arg))
            current->add(MPostWriteBarrier::New(alloc(), array, arg));
    }

    // The array's length is incorrectly 0 now, from the template object
    // created by BaselineCompiler::emit_JSOP_REST() before the actual argument
    // count was known. Set the correct length now that we know that count.
    MSetArrayLength* length = MSetArrayLength::New(alloc(), elements, index);
    current->add(length);

    // Update the initialized length for all the (necessarily non-hole)
    // elements added.
    MSetInitializedLength* initLength = MSetInitializedLength::New(alloc(), elements, index);
    current->add(initLength);

    current->push(array);
    return true;
}

bool
IonBuilder::jsop_checkobjcoercible()
{
    MDefinition* toCheck = current->peek(-1);

    if (!toCheck->mightBeType(MIRType_Undefined) &&
        !toCheck->mightBeType(MIRType_Null))
    {
        toCheck->setImplicitlyUsedUnchecked();
        return true;
    }

    MOZ_ASSERT(toCheck->type() == MIRType_Value ||
               toCheck->type() == MIRType_Null  ||
               toCheck->type() == MIRType_Undefined);

    // If we want to squeeze more perf here, we can throw without checking,
    // if IsNullOrUndefined(toCheck->type()). Since this is a failure case,
    // it should be OK.
    MCheckObjCoercible* check = MCheckObjCoercible::New(alloc(), current->pop());
    current->add(check);
    current->push(check);
    return resumeAfter(check);
}

uint32_t
IonBuilder::getDefiniteSlot(TemporaryTypeSet* types, PropertyName* name, uint32_t* pnfixed)
{
    if (!types || types->unknownObject()) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return UINT32_MAX;
    }

    uint32_t slot = UINT32_MAX;

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties()) {
            trackOptimizationOutcome(TrackedOutcome::UnknownProperties);
            return UINT32_MAX;
        }

        if (key->isSingleton()) {
            trackOptimizationOutcome(TrackedOutcome::Singleton);
            return UINT32_MAX;
        }

        HeapTypeSetKey property = key->property(NameToId(name));
        if (!property.maybeTypes() ||
            !property.maybeTypes()->definiteProperty() ||
            property.nonData(constraints()))
        {
            trackOptimizationOutcome(TrackedOutcome::NotFixedSlot);
            return UINT32_MAX;
        }

        // Definite slots will always be fixed slots when they are in the
        // allowable range for fixed slots, except for objects which were
        // converted from unboxed objects and have a smaller allocation size.
        size_t nfixed = NativeObject::MAX_FIXED_SLOTS;
        if (ObjectGroup* group = key->group()->maybeOriginalUnboxedGroup())
            nfixed = gc::GetGCKindSlots(group->unboxedLayout().getAllocKind());

        uint32_t propertySlot = property.maybeTypes()->definiteSlot();
        if (slot == UINT32_MAX) {
            slot = propertySlot;
            *pnfixed = nfixed;
        } else if (slot != propertySlot || nfixed != *pnfixed) {
            trackOptimizationOutcome(TrackedOutcome::InconsistentFixedSlot);
            return UINT32_MAX;
        }
    }

    return slot;
}

uint32_t
IonBuilder::getUnboxedOffset(TemporaryTypeSet* types, PropertyName* name, JSValueType* punboxedType)
{
    if (!types || types->unknownObject()) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return UINT32_MAX;
    }

    uint32_t offset = UINT32_MAX;

    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties()) {
            trackOptimizationOutcome(TrackedOutcome::UnknownProperties);
            return UINT32_MAX;
        }

        if (key->isSingleton()) {
            trackOptimizationOutcome(TrackedOutcome::Singleton);
            return UINT32_MAX;
        }

        UnboxedLayout* layout = key->group()->maybeUnboxedLayout();
        if (!layout) {
            trackOptimizationOutcome(TrackedOutcome::NotUnboxed);
            return UINT32_MAX;
        }

        const UnboxedLayout::Property* property = layout->lookup(name);
        if (!property) {
            trackOptimizationOutcome(TrackedOutcome::StructNoField);
            return UINT32_MAX;
        }

        if (layout->nativeGroup()) {
            trackOptimizationOutcome(TrackedOutcome::UnboxedConvertedToNative);
            return UINT32_MAX;
        }

        key->watchStateChangeForUnboxedConvertedToNative(constraints());

        if (offset == UINT32_MAX) {
            offset = property->offset;
            *punboxedType = property->type;
        } else if (offset != property->offset) {
            trackOptimizationOutcome(TrackedOutcome::InconsistentFieldOffset);
            return UINT32_MAX;
        } else if (*punboxedType != property->type) {
            trackOptimizationOutcome(TrackedOutcome::InconsistentFieldType);
            return UINT32_MAX;
        }
    }

    return offset;
}

bool
IonBuilder::jsop_runonce()
{
    MRunOncePrologue* ins = MRunOncePrologue::New(alloc());
    current->add(ins);
    return resumeAfter(ins);
}

bool
IonBuilder::jsop_not()
{
    MDefinition* value = current->pop();

    MNot* ins = MNot::New(alloc(), value, constraints());
    current->add(ins);
    current->push(ins);
    return true;
}

bool
IonBuilder::objectsHaveCommonPrototype(TemporaryTypeSet* types, PropertyName* name,
                                       bool isGetter, JSObject* foundProto, bool* guardGlobal)
{
    // With foundProto a prototype with a getter or setter for name, return
    // whether looking up name on any object in |types| will go through
    // foundProto, i.e. all the objects have foundProto on their prototype
    // chain and do not have a property for name before reaching foundProto.

    // No sense looking if we don't know what's going on.
    if (!types || types->unknownObject())
        return false;
    *guardGlobal = false;

    for (unsigned i = 0; i < types->getObjectCount(); i++) {
        if (types->getSingleton(i) == foundProto)
            continue;

        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        while (key) {
            if (key->unknownProperties())
                return false;

            const Class* clasp = key->clasp();
            if (!ClassHasEffectlessLookup(clasp))
                return false;
            JSObject* singleton = key->isSingleton() ? key->singleton() : nullptr;
            if (ObjectHasExtraOwnProperty(compartment, key, NameToId(name))) {
                if (!singleton || !singleton->is<GlobalObject>())
                    return false;
                *guardGlobal = true;
            }

            // Look for a getter/setter on the class itself which may need
            // to be called.
            if (isGetter && clasp->ops.getProperty)
                return false;
            if (!isGetter && clasp->ops.setProperty)
                return false;

            // Test for isOwnProperty() without freezing. If we end up
            // optimizing, freezePropertiesForCommonPropFunc will freeze the
            // property type sets later on.
            HeapTypeSetKey property = key->property(NameToId(name));
            if (TypeSet* types = property.maybeTypes()) {
                if (!types->empty() || types->nonDataProperty())
                    return false;
            }
            if (singleton) {
                if (CanHaveEmptyPropertyTypesForOwnProperty(singleton)) {
                    MOZ_ASSERT(singleton->is<GlobalObject>());
                    *guardGlobal = true;
                }
            }

            JSObject* proto = checkNurseryObject(key->proto().toObjectOrNull());

            if (proto == foundProto)
                break;
            if (!proto) {
                // The foundProto being searched for did not show up on the
                // object's prototype chain.
                return false;
            }
            key = TypeSet::ObjectKey::get(proto);
        }
    }

    return true;
}

void
IonBuilder::freezePropertiesForCommonPrototype(TemporaryTypeSet* types, PropertyName* name,
                                               JSObject* foundProto,
                                               bool allowEmptyTypesforGlobal/* = false*/)
{
    for (unsigned i = 0; i < types->getObjectCount(); i++) {
        // If we found a Singleton object's own-property, there's nothing to
        // freeze.
        if (types->getSingleton(i) == foundProto)
            continue;

        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        while (true) {
            HeapTypeSetKey property = key->property(NameToId(name));
            JS_ALWAYS_TRUE(!property.isOwnProperty(constraints(), allowEmptyTypesforGlobal));

            // Don't mark the proto. It will be held down by the shape
            // guard. This allows us to use properties found on prototypes
            // with properties unknown to TI.
            if (key->proto() == TaggedProto(foundProto))
                break;
            key = TypeSet::ObjectKey::get(key->proto().toObjectOrNull());
        }
    }
}

bool
IonBuilder::testCommonGetterSetter(TemporaryTypeSet* types, PropertyName* name,
                                   bool isGetter, JSObject* foundProto, Shape* lastProperty,
                                   JSFunction* getterOrSetter,
                                   MDefinition** guard,
                                   Shape* globalShape/* = nullptr*/,
                                   MDefinition** globalGuard/* = nullptr */)
{
    MOZ_ASSERT_IF(globalShape, globalGuard);
    bool guardGlobal;

    // Check if all objects being accessed will lookup the name through foundProto.
    if (!objectsHaveCommonPrototype(types, name, isGetter, foundProto, &guardGlobal) ||
        (guardGlobal && !globalShape))
    {
        trackOptimizationOutcome(TrackedOutcome::MultiProtoPaths);
        return false;
    }

    // We can optimize the getter/setter, so freeze all involved properties to
    // ensure there isn't a lower shadowing getter or setter installed in the
    // future.
    freezePropertiesForCommonPrototype(types, name, foundProto, guardGlobal);

    // Add a shape guard on the prototype we found the property on. The rest of
    // the prototype chain is guarded by TI freezes, except when name is a global
    // name. In this case, we also have to guard on the globals shape to be able
    // to optimize, because the way global property sets are handled means
    // freezing doesn't work for what we want here. Note that a shape guard is
    // good enough here, even in the proxy case, because we have ensured there
    // are no lookup hooks for this property.
    if (guardGlobal) {
        JSObject* obj = &script()->global();
        MDefinition* globalObj = constant(ObjectValue(*obj));
        *globalGuard = addShapeGuard(globalObj, globalShape, Bailout_ShapeGuard);
    }

    if (foundProto->isNative()) {
        NativeObject& nativeProto = foundProto->as<NativeObject>();
        if (nativeProto.lastProperty() == lastProperty) {
            // The proto shape is the same as it was at the point when we
            // created the baseline IC, so looking up the prop on the object as
            // it is now should be safe.
            Shape* propShape = nativeProto.lookupPure(name);
            MOZ_ASSERT_IF(isGetter, propShape->getterObject() == getterOrSetter);
            MOZ_ASSERT_IF(!isGetter, propShape->setterObject() == getterOrSetter);
            if (propShape && !propShape->configurable())
                return true;
        }
    }

    MInstruction* wrapper = constant(ObjectValue(*foundProto));
    *guard = addShapeGuard(wrapper, lastProperty, Bailout_ShapeGuard);
    return true;
}

void
IonBuilder::replaceMaybeFallbackFunctionGetter(MGetPropertyCache* cache)
{
    // Discard the last prior resume point of the previous MGetPropertyCache.
    WrapMGetPropertyCache rai(maybeFallbackFunctionGetter_);
    maybeFallbackFunctionGetter_ = cache;
}

bool
IonBuilder::annotateGetPropertyCache(MDefinition* obj, PropertyName* name,
                                     MGetPropertyCache* getPropCache, TemporaryTypeSet* objTypes,
                                     TemporaryTypeSet* pushedTypes)
{
    // Ensure every pushed value is a singleton.
    if (pushedTypes->unknownObject() || pushedTypes->baseFlags() != 0)
        return true;

    for (unsigned i = 0; i < pushedTypes->getObjectCount(); i++) {
        if (pushedTypes->getGroup(i) != nullptr)
            return true;
    }

    // Object's typeset should be a proper object
    if (!objTypes || objTypes->baseFlags() || objTypes->unknownObject())
        return true;

    unsigned int objCount = objTypes->getObjectCount();
    if (objCount == 0)
        return true;

    InlinePropertyTable* inlinePropTable = getPropCache->initInlinePropertyTable(alloc(), pc);
    if (!inlinePropTable)
        return false;

    // Ensure that the relevant property typeset for each group is
    // is a single-object typeset containing a JSFunction
    for (unsigned int i = 0; i < objCount; i++) {
        ObjectGroup* group = objTypes->getGroup(i);
        if (!group)
            continue;
        TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(group);
        if (key->unknownProperties() || !key->proto().isObject())
            continue;
        JSObject* proto = checkNurseryObject(key->proto().toObject());

        const Class* clasp = key->clasp();
        if (!ClassHasEffectlessLookup(clasp) || ObjectHasExtraOwnProperty(compartment, key, NameToId(name)))
            continue;

        HeapTypeSetKey ownTypes = key->property(NameToId(name));
        if (ownTypes.isOwnProperty(constraints()))
            continue;

        JSObject* singleton = testSingletonProperty(proto, NameToId(name));
        if (!singleton || !singleton->is<JSFunction>())
            continue;

        // Don't add cases corresponding to non-observed pushes
        if (!pushedTypes->hasType(TypeSet::ObjectType(singleton)))
            continue;

        if (!inlinePropTable->addEntry(alloc(), group, &singleton->as<JSFunction>()))
            return false;
    }

    if (inlinePropTable->numEntries() == 0) {
        getPropCache->clearInlinePropertyTable();
        return true;
    }

#ifdef JS_JITSPEW
    if (inlinePropTable->numEntries() > 0)
        JitSpew(JitSpew_Inlining, "Annotated GetPropertyCache with %d/%d inline cases",
                                    (int) inlinePropTable->numEntries(), (int) objCount);
#endif

    // If we successfully annotated the GetPropertyCache and there are inline cases,
    // then keep a resume point of the state right before this instruction for use
    // later when we have to bail out to this point in the fallback case of a
    // PolyInlineDispatch.
    if (inlinePropTable->numEntries() > 0) {
        // Push the object back onto the stack temporarily to capture the resume point.
        current->push(obj);
        MResumePoint* resumePoint = MResumePoint::New(alloc(), current, pc,
                                                      MResumePoint::ResumeAt);
        if (!resumePoint)
            return false;
        inlinePropTable->setPriorResumePoint(resumePoint);
        replaceMaybeFallbackFunctionGetter(getPropCache);
        current->pop();
    }
    return true;
}

// Returns true if an idempotent cache has ever invalidated this script
// or an outer script.
bool
IonBuilder::invalidatedIdempotentCache()
{
    IonBuilder* builder = this;
    do {
        if (builder->script()->invalidatedIdempotentCache())
            return true;
        builder = builder->callerBuilder_;
    } while (builder);

    return false;
}

bool
IonBuilder::loadSlot(MDefinition* obj, size_t slot, size_t nfixed, MIRType rvalType,
                     BarrierKind barrier, TemporaryTypeSet* types)
{
    if (slot < nfixed) {
        MLoadFixedSlot* load = MLoadFixedSlot::New(alloc(), obj, slot);
        current->add(load);
        current->push(load);

        load->setResultType(rvalType);
        return pushTypeBarrier(load, types, barrier);
    }

    MSlots* slots = MSlots::New(alloc(), obj);
    current->add(slots);

    MLoadSlot* load = MLoadSlot::New(alloc(), slots, slot - nfixed);
    current->add(load);
    current->push(load);

    load->setResultType(rvalType);
    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::loadSlot(MDefinition* obj, Shape* shape, MIRType rvalType,
                     BarrierKind barrier, TemporaryTypeSet* types)
{
    return loadSlot(obj, shape->slot(), shape->numFixedSlots(), rvalType, barrier, types);
}

bool
IonBuilder::storeSlot(MDefinition* obj, size_t slot, size_t nfixed,
                      MDefinition* value, bool needsBarrier,
                      MIRType slotType /* = MIRType_None */)
{
    if (slot < nfixed) {
        MStoreFixedSlot* store = MStoreFixedSlot::New(alloc(), obj, slot, value);
        current->add(store);
        current->push(value);
        if (needsBarrier)
            store->setNeedsBarrier();
        return resumeAfter(store);
    }

    MSlots* slots = MSlots::New(alloc(), obj);
    current->add(slots);

    MStoreSlot* store = MStoreSlot::New(alloc(), slots, slot - nfixed, value);
    current->add(store);
    current->push(value);
    if (needsBarrier)
        store->setNeedsBarrier();
    if (slotType != MIRType_None)
        store->setSlotType(slotType);
    return resumeAfter(store);
}

bool
IonBuilder::storeSlot(MDefinition* obj, Shape* shape, MDefinition* value, bool needsBarrier,
                      MIRType slotType /* = MIRType_None */)
{
    MOZ_ASSERT(shape->writable());
    return storeSlot(obj, shape->slot(), shape->numFixedSlots(), value, needsBarrier, slotType);
}

bool
IonBuilder::shouldAbortOnPreliminaryGroups(MDefinition *obj)
{
    // Watch for groups which still have preliminary object information and
    // have not had the new script properties or unboxed layout analyses
    // performed. Normally this is done after a small number of the objects
    // have been created, but if only a few have been created we can still
    // perform the analysis with a smaller object population. The analysis can
    // have side effects so we will end up aborting compilation after building
    // finishes and retrying later.
    TemporaryTypeSet *types = obj->resultTypeSet();
    if (!types || types->unknownObject())
        return false;

    bool preliminary = false;
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = types->getObject(i);
        if (!key)
            continue;

        if (ObjectGroup* group = key->maybeGroup()) {
            if (group->hasUnanalyzedPreliminaryObjects()) {
                addAbortedPreliminaryGroup(group);
                preliminary = true;
            }
        }
    }

    return preliminary;
}

MDefinition*
IonBuilder::maybeUnboxForPropertyAccess(MDefinition* def)
{
    if (def->type() != MIRType_Value)
        return def;

    MIRType type = inspector->expectedPropertyAccessInputType(pc);
    if (type == MIRType_Value || !def->mightBeType(type))
        return def;

    MUnbox* unbox = MUnbox::New(alloc(), def, type, MUnbox::Fallible);
    current->add(unbox);

    // Fixup type information for a common case where a property call
    // is converted to the following bytecodes
    //
    //      a.foo()
    // ================= Compiles to ================
    //      LOAD "a"
    //      DUP
    //      CALLPROP "foo"
    //      SWAP
    //      CALL 0
    //
    // If we have better type information to unbox the first copy going into
    // the CALLPROP operation, we can replace the duplicated copy on the
    // stack as well.
    if (*pc == JSOP_CALLPROP || *pc == JSOP_CALLELEM) {
        uint32_t idx = current->stackDepth() - 1;
        MOZ_ASSERT(current->getSlot(idx) == def);
        current->setSlot(idx, unbox);
    }

    return unbox;
}

bool
IonBuilder::jsop_getprop(PropertyName* name)
{
    bool emitted = false;
    startTrackingOptimizations();

    MDefinition* obj = current->pop();
    TemporaryTypeSet* types = bytecodeTypes(pc);

    trackTypeInfo(TrackedTypeSite::Receiver, obj->type(), obj->resultTypeSet());

    if (!info().isAnalysis()) {
        // The calls below can abort compilation, so we only try this if we're
        // not analyzing.
        // Try to optimize arguments.length.
        trackOptimizationAttempt(TrackedStrategy::GetProp_ArgumentsLength);
        if (!getPropTryArgumentsLength(&emitted, obj) || emitted)
            return emitted;

        // Try to optimize arguments.callee.
        trackOptimizationAttempt(TrackedStrategy::GetProp_ArgumentsCallee);
        if (!getPropTryArgumentsCallee(&emitted, obj, name) || emitted)
            return emitted;
    }

    obj = maybeUnboxForPropertyAccess(obj);
    if (obj->type() == MIRType_Object)
        obj = convertUnboxedObjects(obj);

    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(),
                                                       obj, name, types);

    // Try to optimize to a specific constant.
    trackOptimizationAttempt(TrackedStrategy::GetProp_InferredConstant);
    if (barrier == BarrierKind::NoBarrier) {
        if (!getPropTryInferredConstant(&emitted, obj, name, types) || emitted)
            return emitted;
    } else {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
    }

    // Always use a call if we are performing analysis and
    // not actually emitting code, to simplify later analysis. Also skip deeper
    // analysis if there are no known types for this operation, as it will
    // always invalidate when executing.
    if (info().isAnalysis() || types->empty() || shouldAbortOnPreliminaryGroups(obj)) {
        if (types->empty()) {
            // Since no further optimizations will be tried, use the IC
            // strategy, which would have been the last one to be tried, as a
            // sentinel value for why everything failed.
            trackOptimizationAttempt(TrackedStrategy::GetProp_InlineCache);
            trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        }

        MCallGetProperty* call = MCallGetProperty::New(alloc(), obj, name);
        current->add(call);

        // During the definite properties analysis we can still try to bake in
        // constants read off the prototype chain, to allow inlining later on.
        // In this case we still need the getprop call so that the later
        // analysis knows when the |this| value has been read from.
        if (info().isAnalysis()) {
            if (!getPropTryConstant(&emitted, obj, NameToId(name), types) || emitted)
                return emitted;
        }

        current->push(call);
        return resumeAfter(call) && pushTypeBarrier(call, types, BarrierKind::TypeSet);
    }

    // Try to optimize accesses on outer window proxies, for example window.foo.
    // This needs to come before the various strategies getPropTryInnerize tries
    // internally, since some of those strategies will "succeed" in silly ways
    // even for an outer object.
    trackOptimizationAttempt(TrackedStrategy::GetProp_Innerize);
    if (!getPropTryInnerize(&emitted, obj, name, types) || emitted)
        return emitted;

    if (!forceInlineCaches()) {
        // Try to hardcode known constants.
        trackOptimizationAttempt(TrackedStrategy::GetProp_Constant);
        if (!getPropTryConstant(&emitted, obj, NameToId(name), types) || emitted)
            return emitted;

        // Try to emit SIMD getter loads
        trackOptimizationAttempt(TrackedStrategy::GetProp_SimdGetter);
        if (!getPropTrySimdGetter(&emitted, obj, name) || emitted)
            return emitted;

        // Try to emit loads from known binary data blocks
        trackOptimizationAttempt(TrackedStrategy::GetProp_TypedObject);
        if (!getPropTryTypedObject(&emitted, obj, name) || emitted)
            return emitted;

        // Try to emit loads from definite slots.
        trackOptimizationAttempt(TrackedStrategy::GetProp_DefiniteSlot);
        if (!getPropTryDefiniteSlot(&emitted, obj, name, barrier, types) || emitted)
            return emitted;

        // Try to emit loads from unboxed objects.
        trackOptimizationAttempt(TrackedStrategy::GetProp_Unboxed);
        if (!getPropTryUnboxed(&emitted, obj, name, barrier, types) || emitted)
            return emitted;

        // Try to inline a common property getter, or make a call.
        trackOptimizationAttempt(TrackedStrategy::GetProp_CommonGetter);
        if (!getPropTryCommonGetter(&emitted, obj, name, types) || emitted)
            return emitted;

        // Try to emit a monomorphic/polymorphic access based on baseline caches.
        trackOptimizationAttempt(TrackedStrategy::GetProp_InlineAccess);
        if (!getPropTryInlineAccess(&emitted, obj, name, barrier, types) || emitted)
            return emitted;

        // Try to emit loads from a module namespace.
        trackOptimizationAttempt(TrackedStrategy::GetProp_ModuleNamespace);
        if (!getPropTryModuleNamespace(&emitted, obj, name, barrier, types) || emitted)
            return emitted;
    }

    // Try to emit a polymorphic cache.
    trackOptimizationAttempt(TrackedStrategy::GetProp_InlineCache);
    if (!getPropTryCache(&emitted, obj, name, barrier, types) || emitted)
        return emitted;

    // Try to emit a shared stub.
    trackOptimizationAttempt(TrackedStrategy::GetProp_SharedCache);
    if (!getPropTrySharedStub(&emitted, obj) || emitted)
        return emitted;

    // Emit a call.
    MCallGetProperty* call = MCallGetProperty::New(alloc(), obj, name);
    current->add(call);
    current->push(call);
    if (!resumeAfter(call))
        return false;

    if (*pc == JSOP_CALLPROP && IsNullOrUndefined(obj->type())) {
        // Due to inlining, it's possible the observed TypeSet is non-empty,
        // even though we know |obj| is null/undefined and the MCallGetProperty
        // will throw. Don't push a TypeBarrier in this case, to avoid
        // inlining the following (unreachable) JSOP_CALL.
        return true;
    }

    return pushTypeBarrier(call, types, BarrierKind::TypeSet);
}

bool
IonBuilder::improveThisTypesForCall()
{
    // After a CALLPROP (or CALLELEM) for obj.prop(), the this-value and callee
    // for the call are on top of the stack:
    //
    // ... [this: obj], [callee: obj.prop]
    //
    // If obj is null or undefined, obj.prop would have thrown an exception so
    // at this point we can remove null and undefined from obj's TypeSet, to
    // improve type information for the call that will follow.

    MOZ_ASSERT(*pc == JSOP_CALLPROP || *pc == JSOP_CALLELEM);

    // Ensure |this| has types {object, null/undefined}.
    MDefinition* thisDef = current->peek(-2);
    if (thisDef->type() != MIRType_Value ||
        !thisDef->mightBeType(MIRType_Object) ||
        !thisDef->resultTypeSet() ||
        !thisDef->resultTypeSet()->objectOrSentinel())
    {
        return true;
    }

    // Remove null/undefined from the TypeSet.
    TemporaryTypeSet* types = thisDef->resultTypeSet()->cloneObjectsOnly(alloc_->lifoAlloc());
    if (!types)
        return false;

    MFilterTypeSet* filter = MFilterTypeSet::New(alloc(), thisDef, types);
    current->add(filter);
    current->rewriteAtDepth(-2, filter);

    // FilterTypeSetPolicy::adjustInputs will insert an infallible Unbox(Object)
    // for the input. Don't hoist this unbox above the getprop or getelem
    // operation.
    filter->setDependency(current->peek(-1)->toInstruction());
    return true;
}

bool
IonBuilder::checkIsDefinitelyOptimizedArguments(MDefinition* obj, bool* isOptimizedArgs)
{
    if (obj->type() != MIRType_MagicOptimizedArguments) {
        if (script()->argumentsHasVarBinding() &&
            obj->mightBeType(MIRType_MagicOptimizedArguments))
        {
            return abort("Type is not definitely lazy arguments.");
        }

        *isOptimizedArgs = false;
        return true;
    }

    *isOptimizedArgs = true;
    return true;
}

bool
IonBuilder::getPropTryInferredConstant(bool* emitted, MDefinition* obj, PropertyName* name,
                                       TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    // Need a result typeset to optimize.
    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    if (!objTypes) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return true;
    }

    JSObject* singleton = objTypes->maybeSingleton();
    if (!singleton) {
        trackOptimizationOutcome(TrackedOutcome::NotSingleton);
        return true;
    }

    TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(singleton);
    if (key->unknownProperties()) {
        trackOptimizationOutcome(TrackedOutcome::UnknownProperties);
        return true;
    }

    HeapTypeSetKey property = key->property(NameToId(name));

    Value constantValue = UndefinedValue();
    if (property.constant(constraints(), &constantValue)) {
        spew("Optimized constant property");
        obj->setImplicitlyUsedUnchecked();
        if (!pushConstant(constantValue))
            return false;
        types->addType(TypeSet::GetValueType(constantValue), alloc_->lifoAlloc());
        trackOptimizationSuccess();
        *emitted = true;
    }

    return true;
}

bool
IonBuilder::getPropTryArgumentsLength(bool* emitted, MDefinition* obj)
{
    MOZ_ASSERT(*emitted == false);

    bool isOptimizedArgs = false;
    if (!checkIsDefinitelyOptimizedArguments(obj, &isOptimizedArgs))
        return false;
    if (!isOptimizedArgs)
        return true;

    if (JSOp(*pc) != JSOP_LENGTH)
        return true;

    trackOptimizationSuccess();
    *emitted = true;

    obj->setImplicitlyUsedUnchecked();

    // We don't know anything from the callee
    if (inliningDepth_ == 0) {
        MInstruction* ins = MArgumentsLength::New(alloc());
        current->add(ins);
        current->push(ins);
        return true;
    }

    // We are inlining and know the number of arguments the callee pushed
    return pushConstant(Int32Value(inlineCallInfo_->argv().length()));
}

bool
IonBuilder::getPropTryArgumentsCallee(bool* emitted, MDefinition* obj, PropertyName* name)
{
    MOZ_ASSERT(*emitted == false);

    bool isOptimizedArgs = false;
    if (!checkIsDefinitelyOptimizedArguments(obj, &isOptimizedArgs))
        return false;
    if (!isOptimizedArgs)
        return true;

    if (name != names().callee)
        return true;

    MOZ_ASSERT(script()->hasMappedArgsObj());

    obj->setImplicitlyUsedUnchecked();
    current->push(getCallee());

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryConstant(bool* emitted, MDefinition* obj, jsid id, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    if (!types->mightBeMIRType(MIRType_Object)) {
        // If we have not observed an object result here, don't look for a
        // singleton constant.
        trackOptimizationOutcome(TrackedOutcome::NotObject);
        return true;
    }

    JSObject* singleton = testSingletonPropertyTypes(obj, id);
    if (!singleton) {
        trackOptimizationOutcome(TrackedOutcome::NotSingleton);
        return true;
    }

    // Property access is a known constant -- safe to emit.
    obj->setImplicitlyUsedUnchecked();

    pushConstant(ObjectValue(*singleton));

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

MIRType
IonBuilder::SimdTypeDescrToMIRType(SimdTypeDescr::Type type)
{
    switch (type) {
      case SimdTypeDescr::Int32x4:   return MIRType_Int32x4;
      case SimdTypeDescr::Float32x4: return MIRType_Float32x4;
      case SimdTypeDescr::Int8x16:
      case SimdTypeDescr::Int16x8:
      case SimdTypeDescr::Float64x2: return MIRType_Undefined;
    }
    MOZ_CRASH("unimplemented MIR type for a SimdTypeDescr::Type");
}

bool
IonBuilder::getPropTrySimdGetter(bool* emitted, MDefinition* obj, PropertyName* name)
{
    MOZ_ASSERT(!*emitted);

    if (!JitSupportsSimd()) {
        trackOptimizationOutcome(TrackedOutcome::NoSimdJitSupport);
        return true;
    }

    TypedObjectPrediction objPrediction = typedObjectPrediction(obj);
    if (objPrediction.isUseless()) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotTypedObject);
        return true;
    }

    if (objPrediction.kind() != type::Simd) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotSimdObject);
        return true;
    }

    MIRType type = SimdTypeDescrToMIRType(objPrediction.simdType());
    if (type == MIRType_Undefined) {
        trackOptimizationOutcome(TrackedOutcome::SimdTypeNotOptimized);
        return true;
    }

    const JSAtomState& names = compartment->runtime()->names();

    // Reading the signMask property.
    if (name != names.signMask) {
        // Unknown getprop access on a SIMD value
        trackOptimizationOutcome(TrackedOutcome::UnknownSimdProperty);
        return true;
    }

    MSimdSignMask* ins = MSimdSignMask::New(alloc(), obj, type);
    current->add(ins);
    current->push(ins);
    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryTypedObject(bool* emitted,
                                  MDefinition* obj,
                                  PropertyName* name)
{
    TypedObjectPrediction fieldPrediction;
    size_t fieldOffset;
    size_t fieldIndex;
    if (!typedObjectHasField(obj, name, &fieldOffset, &fieldPrediction, &fieldIndex))
        return true;

    switch (fieldPrediction.kind()) {
      case type::Simd:
        // FIXME (bug 894104): load into a MIRType_float32x4 etc
        return true;

      case type::Struct:
      case type::Array:
        return getPropTryComplexPropOfTypedObject(emitted,
                                                  obj,
                                                  fieldOffset,
                                                  fieldPrediction,
                                                  fieldIndex);

      case type::Reference:
        return getPropTryReferencePropOfTypedObject(emitted,
                                                    obj,
                                                    fieldOffset,
                                                    fieldPrediction,
                                                    name);

      case type::Scalar:
        return getPropTryScalarPropOfTypedObject(emitted,
                                                 obj,
                                                 fieldOffset,
                                                 fieldPrediction);
    }

    MOZ_CRASH("Bad kind");
}

bool
IonBuilder::getPropTryScalarPropOfTypedObject(bool* emitted, MDefinition* typedObj,
                                              int32_t fieldOffset,
                                              TypedObjectPrediction fieldPrediction)
{
    // Must always be loading the same scalar type
    Scalar::Type fieldType = fieldPrediction.scalarType();

    // Don't optimize if the typed object might be neutered.
    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
        return true;

    trackOptimizationSuccess();
    *emitted = true;

    LinearSum byteOffset(alloc());
    if (!byteOffset.add(fieldOffset))
        setForceAbort();

    return pushScalarLoadFromTypedObject(typedObj, byteOffset, fieldType);
}

bool
IonBuilder::getPropTryReferencePropOfTypedObject(bool* emitted, MDefinition* typedObj,
                                                 int32_t fieldOffset,
                                                 TypedObjectPrediction fieldPrediction,
                                                 PropertyName* name)
{
    ReferenceTypeDescr::Type fieldType = fieldPrediction.referenceType();

    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
        return true;

    trackOptimizationSuccess();
    *emitted = true;

    LinearSum byteOffset(alloc());
    if (!byteOffset.add(fieldOffset))
        setForceAbort();

    return pushReferenceLoadFromTypedObject(typedObj, byteOffset, fieldType, name);
}

bool
IonBuilder::getPropTryComplexPropOfTypedObject(bool* emitted,
                                               MDefinition* typedObj,
                                               int32_t fieldOffset,
                                               TypedObjectPrediction fieldPrediction,
                                               size_t fieldIndex)
{
    // Don't optimize if the typed object might be neutered.
    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
        return true;

    // OK, perform the optimization

    // Identify the type object for the field.
    MDefinition* type = loadTypedObjectType(typedObj);
    MDefinition* fieldTypeObj = typeObjectForFieldFromStructType(type, fieldIndex);

    LinearSum byteOffset(alloc());
    if (!byteOffset.add(fieldOffset))
        setForceAbort();

    return pushDerivedTypedObject(emitted, typedObj, byteOffset,
                                  fieldPrediction, fieldTypeObj);
}

MDefinition*
IonBuilder::convertUnboxedObjects(MDefinition* obj)
{
    // If obj might be in any particular unboxed group which should be
    // converted to a native representation, perform that conversion. This does
    // not guarantee the object will not have such a group afterwards, if the
    // object's possible groups are not precisely known.
    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types || types->unknownObject())
        return obj;

    BaselineInspector::ObjectGroupVector list(alloc());
    for (size_t i = 0; i < types->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = obj->resultTypeSet()->getObject(i);
        if (!key || !key->isGroup())
            continue;

        if (UnboxedLayout* layout = key->group()->maybeUnboxedLayout()) {
            AutoEnterOOMUnsafeRegion oomUnsafe;
            if (layout->nativeGroup() && !list.append(key->group()))
                oomUnsafe.crash("IonBuilder::convertUnboxedObjects");
        }
    }

    return convertUnboxedObjects(obj, list);
}

MDefinition*
IonBuilder::convertUnboxedObjects(MDefinition* obj,
                                  const BaselineInspector::ObjectGroupVector& list)
{
    for (size_t i = 0; i < list.length(); i++) {
        ObjectGroup* group = list[i];
        if (TemporaryTypeSet* types = obj->resultTypeSet()) {
            if (!types->hasType(TypeSet::ObjectType(group)))
                continue;
        }
        obj = MConvertUnboxedObjectToNative::New(alloc(), obj, group);
        current->add(obj->toInstruction());
    }
    return obj;
}

bool
IonBuilder::getPropTryDefiniteSlot(bool* emitted, MDefinition* obj, PropertyName* name,
                                   BarrierKind barrier, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    uint32_t nfixed;
    uint32_t slot = getDefiniteSlot(obj->resultTypeSet(), name, &nfixed);
    if (slot == UINT32_MAX)
        return true;

    if (obj->type() != MIRType_Object) {
        MGuardObject* guard = MGuardObject::New(alloc(), obj);
        current->add(guard);
        obj = guard;
    }

    MInstruction* load;
    if (slot < nfixed) {
        load = MLoadFixedSlot::New(alloc(), obj, slot);
    } else {
        MInstruction* slots = MSlots::New(alloc(), obj);
        current->add(slots);

        load = MLoadSlot::New(alloc(), slots, slot - nfixed);
    }

    if (barrier == BarrierKind::NoBarrier)
        load->setResultType(types->getKnownMIRType());

    current->add(load);
    current->push(load);

    if (!pushTypeBarrier(load, types, barrier))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryModuleNamespace(bool* emitted, MDefinition* obj, PropertyName* name,
                                      BarrierKind barrier, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    if (!objTypes) {
        trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
        return true;
    }

    JSObject* singleton = objTypes->maybeSingleton();
    if (!singleton) {
        trackOptimizationOutcome(TrackedOutcome::NotSingleton);
        return true;
    }

    if (!singleton->is<ModuleNamespaceObject>()) {
        trackOptimizationOutcome(TrackedOutcome::NotModuleNamespace);
        return true;
    }

    ModuleNamespaceObject* ns = &singleton->as<ModuleNamespaceObject>();
    ModuleEnvironmentObject* env;
    Shape* shape;
    if (!ns->bindings().lookup(NameToId(name), &env, &shape)) {
        trackOptimizationOutcome(TrackedOutcome::UnknownProperty);
        return true;
    }

    obj->setImplicitlyUsedUnchecked();
    MConstant* envConst = constant(ObjectValue(*env));
    uint32_t slot = shape->slot();
    uint32_t nfixed = env->numFixedSlots();
    if (!loadSlot(envConst, slot, nfixed, types->getKnownMIRType(), barrier, types))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

MInstruction*
IonBuilder::loadUnboxedProperty(MDefinition* obj, size_t offset, JSValueType unboxedType,
                                BarrierKind barrier, TemporaryTypeSet* types)
{
    // loadUnboxedValue is designed to load any value as if it were contained in
    // an array. Thus a property offset is converted to an index, when the
    // object is reinterpreted as an array of properties of the same size.
    size_t index = offset / UnboxedTypeSize(unboxedType);
    MInstruction* indexConstant = MConstant::New(alloc(), Int32Value(index));
    current->add(indexConstant);

    return loadUnboxedValue(obj, UnboxedPlainObject::offsetOfData(),
                            indexConstant, unboxedType, barrier, types);
}

MInstruction*
IonBuilder::loadUnboxedValue(MDefinition* elements, size_t elementsOffset,
                             MDefinition* index, JSValueType unboxedType,
                             BarrierKind barrier, TemporaryTypeSet* types)
{
    MInstruction* load;
    switch (unboxedType) {
      case JSVAL_TYPE_BOOLEAN:
        load = MLoadUnboxedScalar::New(alloc(), elements, index, Scalar::Uint8,
                                       DoesNotRequireMemoryBarrier, elementsOffset);
        load->setResultType(MIRType_Boolean);
        break;

      case JSVAL_TYPE_INT32:
        load = MLoadUnboxedScalar::New(alloc(), elements, index, Scalar::Int32,
                                       DoesNotRequireMemoryBarrier, elementsOffset);
        load->setResultType(MIRType_Int32);
        break;

      case JSVAL_TYPE_DOUBLE:
        load = MLoadUnboxedScalar::New(alloc(), elements, index, Scalar::Float64,
                                       DoesNotRequireMemoryBarrier, elementsOffset,
                                       /* canonicalizeDoubles = */ false);
        load->setResultType(MIRType_Double);
        break;

      case JSVAL_TYPE_STRING:
        load = MLoadUnboxedString::New(alloc(), elements, index, elementsOffset);
        break;

      case JSVAL_TYPE_OBJECT: {
        MLoadUnboxedObjectOrNull::NullBehavior nullBehavior;
        if (types->hasType(TypeSet::NullType()) || barrier != BarrierKind::NoBarrier)
            nullBehavior = MLoadUnboxedObjectOrNull::HandleNull;
        else
            nullBehavior = MLoadUnboxedObjectOrNull::NullNotPossible;
        load = MLoadUnboxedObjectOrNull::New(alloc(), elements, index, nullBehavior,
                                             elementsOffset);
        break;
      }

      default:
        MOZ_CRASH();
    }

    current->add(load);
    return load;
}

bool
IonBuilder::getPropTryUnboxed(bool* emitted, MDefinition* obj, PropertyName* name,
                              BarrierKind barrier, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    JSValueType unboxedType;
    uint32_t offset = getUnboxedOffset(obj->resultTypeSet(), name, &unboxedType);
    if (offset == UINT32_MAX)
        return true;

    if (obj->type() != MIRType_Object) {
        MGuardObject* guard = MGuardObject::New(alloc(), obj);
        current->add(guard);
        obj = guard;
    }

    MInstruction* load = loadUnboxedProperty(obj, offset, unboxedType, barrier, types);
    current->push(load);

    if (!pushTypeBarrier(load, types, barrier))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

MDefinition*
IonBuilder::addShapeGuardsForGetterSetter(MDefinition* obj, JSObject* holder, Shape* holderShape,
                const BaselineInspector::ReceiverVector& receivers,
                const BaselineInspector::ObjectGroupVector& convertUnboxedGroups,
                bool isOwnProperty)
{
    MOZ_ASSERT(holder);
    MOZ_ASSERT(holderShape);

    obj = convertUnboxedObjects(obj, convertUnboxedGroups);

    if (isOwnProperty) {
        MOZ_ASSERT(receivers.empty());
        return addShapeGuard(obj, holderShape, Bailout_ShapeGuard);
    }

    MDefinition* holderDef = constant(ObjectValue(*holder));
    addShapeGuard(holderDef, holderShape, Bailout_ShapeGuard);

    return addGuardReceiverPolymorphic(obj, receivers);
}

bool
IonBuilder::getPropTryCommonGetter(bool* emitted, MDefinition* obj, PropertyName* name,
                                   TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    Shape* lastProperty = nullptr;
    JSFunction* commonGetter = nullptr;
    Shape* globalShape = nullptr;
    JSObject* foundProto = nullptr;
    bool isOwnProperty = false;
    BaselineInspector::ReceiverVector receivers(alloc());
    BaselineInspector::ObjectGroupVector convertUnboxedGroups(alloc());
    if (!inspector->commonGetPropFunction(pc, &foundProto, &lastProperty, &commonGetter,
                                          &globalShape, &isOwnProperty,
                                          receivers, convertUnboxedGroups))
    {
        return true;
    }

    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    MDefinition* guard = nullptr;
    MDefinition* globalGuard = nullptr;
    bool canUseTIForGetter =
        testCommonGetterSetter(objTypes, name, /* isGetter = */ true,
                               foundProto, lastProperty, commonGetter, &guard,
                               globalShape, &globalGuard);
    if (!canUseTIForGetter) {
        // If type information is bad, we can still optimize the getter if we
        // shape guard.
        obj = addShapeGuardsForGetterSetter(obj, foundProto, lastProperty,
                                            receivers, convertUnboxedGroups,
                                            isOwnProperty);
        if (!obj)
            return false;
    }

    bool isDOM = objTypes && objTypes->isDOMClass(constraints());

    if (isDOM && testShouldDOMCall(objTypes, commonGetter, JSJitInfo::Getter)) {
        const JSJitInfo* jitinfo = commonGetter->jitInfo();
        MInstruction* get;
        if (jitinfo->isAlwaysInSlot) {
            // If our object is a singleton and we know the property is
            // constant (which is true if and only if the get doesn't alias
            // anything), we can just read the slot here and use that constant.
            JSObject* singleton = objTypes->maybeSingleton();
            if (singleton && jitinfo->aliasSet() == JSJitInfo::AliasNone) {
                size_t slot = jitinfo->slotIndex;
                *emitted = true;
                return pushConstant(GetReservedSlot(singleton, slot));
            }

            // We can't use MLoadFixedSlot here because it might not have the
            // right aliasing behavior; we want to alias DOM setters as needed.
            get = MGetDOMMember::New(alloc(), jitinfo, obj, guard, globalGuard);
        } else {
            get = MGetDOMProperty::New(alloc(), jitinfo, obj, guard, globalGuard);
        }
        if (!get) {
            return false;
        }
        current->add(get);
        current->push(get);

        if (get->isEffectful() && !resumeAfter(get))
            return false;

        if (!pushDOMTypeBarrier(get, types, commonGetter))
            return false;

        trackOptimizationOutcome(TrackedOutcome::DOM);
        *emitted = true;
        return true;
    }

    // Don't call the getter with a primitive value.
    if (obj->type() != MIRType_Object) {
        MGuardObject* guardObj = MGuardObject::New(alloc(), obj);
        current->add(guardObj);
        obj = guardObj;
    }

    // Spoof stack to expected state for call.

    // Make sure there's enough room
    if (!current->ensureHasSlots(2))
        return false;
    current->push(constant(ObjectValue(*commonGetter)));

    current->push(obj);

    CallInfo callInfo(alloc(), false);
    if (!callInfo.init(current, 0))
        return false;

    if (commonGetter->isNative()) {
        InliningStatus status = inlineNativeGetter(callInfo, commonGetter);
        switch (status) {
          case InliningStatus_Error:
            return false;
          case InliningStatus_WarmUpCountTooLow:
          case InliningStatus_NotInlined:
            break;
          case InliningStatus_Inlined:
            trackOptimizationOutcome(TrackedOutcome::Inlined);
            *emitted = true;
            return true;
        }
    }

    // Inline if we can, otherwise, forget it and just generate a call.
    if (commonGetter->isInterpreted()) {
        InliningDecision decision = makeInliningDecision(commonGetter, callInfo);
        switch (decision) {
          case InliningDecision_Error:
            return false;
          case InliningDecision_DontInline:
          case InliningDecision_WarmUpCountTooLow:
            break;
          case InliningDecision_Inline:
            if (!inlineScriptedCall(callInfo, commonGetter))
                return false;
            *emitted = true;
            return true;
        }
    }

    if (!makeCall(commonGetter, callInfo))
        return false;

    // If the getter could have been inlined, don't track success. The call to
    // makeInliningDecision above would have tracked a specific reason why we
    // couldn't inline.
    if (!commonGetter->isInterpreted())
        trackOptimizationSuccess();

    *emitted = true;
    return true;
}

bool
IonBuilder::canInlinePropertyOpShapes(const BaselineInspector::ReceiverVector& receivers)
{
    if (receivers.empty()) {
        trackOptimizationOutcome(TrackedOutcome::NoShapeInfo);
        return false;
    }

    for (size_t i = 0; i < receivers.length(); i++) {
        // We inline the property access as long as the shape is not in
        // dictionary mode. We cannot be sure that the shape is still a
        // lastProperty, and calling Shape::search() on dictionary mode
        // shapes that aren't lastProperty is invalid.
        if (receivers[i].shape && receivers[i].shape->inDictionary()) {
            trackOptimizationOutcome(TrackedOutcome::InDictionaryMode);
            return false;
        }
    }

    return true;
}

static Shape*
PropertyShapesHaveSameSlot(const BaselineInspector::ReceiverVector& receivers, jsid id)
{
    Shape* firstShape = nullptr;
    for (size_t i = 0; i < receivers.length(); i++) {
        if (receivers[i].group)
            return nullptr;

        Shape* shape = receivers[i].shape->searchLinear(id);
        MOZ_ASSERT(shape);

        if (i == 0) {
            firstShape = shape;
        } else if (shape->slot() != firstShape->slot() ||
                   shape->numFixedSlots() != firstShape->numFixedSlots())
        {
            return nullptr;
        }
    }

    return firstShape;
}

bool
IonBuilder::getPropTryInlineAccess(bool* emitted, MDefinition* obj, PropertyName* name,
                                   BarrierKind barrier, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    BaselineInspector::ReceiverVector receivers(alloc());
    BaselineInspector::ObjectGroupVector convertUnboxedGroups(alloc());
    if (!inspector->maybeInfoForPropertyOp(pc, receivers, convertUnboxedGroups))
        return false;

    if (!canInlinePropertyOpShapes(receivers))
        return true;

    obj = convertUnboxedObjects(obj, convertUnboxedGroups);

    MIRType rvalType = types->getKnownMIRType();
    if (barrier != BarrierKind::NoBarrier || IsNullOrUndefined(rvalType))
        rvalType = MIRType_Value;

    if (receivers.length() == 1) {
        if (!receivers[0].group) {
            // Monomorphic load from a native object.
            spew("Inlining monomorphic native GETPROP");

            obj = addShapeGuard(obj, receivers[0].shape, Bailout_ShapeGuard);

            Shape* shape = receivers[0].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(shape);

            if (!loadSlot(obj, shape, rvalType, barrier, types))
                return false;

            trackOptimizationOutcome(TrackedOutcome::Monomorphic);
            *emitted = true;
            return true;
        }

        if (receivers[0].shape) {
            // Monomorphic load from an unboxed object expando.
            spew("Inlining monomorphic unboxed expando GETPROP");

            obj = addGroupGuard(obj, receivers[0].group, Bailout_ShapeGuard);
            obj = addUnboxedExpandoGuard(obj, /* hasExpando = */ true, Bailout_ShapeGuard);

            MInstruction* expando = MLoadUnboxedExpando::New(alloc(), obj);
            current->add(expando);

            expando = addShapeGuard(expando, receivers[0].shape, Bailout_ShapeGuard);

            Shape* shape = receivers[0].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(shape);

            if (!loadSlot(expando, shape, rvalType, barrier, types))
                return false;

            trackOptimizationOutcome(TrackedOutcome::Monomorphic);
            *emitted = true;
            return true;
        }

        // Monomorphic load from an unboxed object.
        obj = addGroupGuard(obj, receivers[0].group, Bailout_ShapeGuard);

        const UnboxedLayout::Property* property = receivers[0].group->unboxedLayout().lookup(name);
        MInstruction* load = loadUnboxedProperty(obj, property->offset, property->type, barrier, types);
        current->push(load);

        if (!pushTypeBarrier(load, types, barrier))
            return false;

        trackOptimizationOutcome(TrackedOutcome::Monomorphic);
        *emitted = true;
        return true;
    }

    MOZ_ASSERT(receivers.length() > 1);
    spew("Inlining polymorphic GETPROP");

    if (Shape* propShape = PropertyShapesHaveSameSlot(receivers, NameToId(name))) {
        obj = addGuardReceiverPolymorphic(obj, receivers);
        if (!obj)
            return false;

        if (!loadSlot(obj, propShape, rvalType, barrier, types))
            return false;

        trackOptimizationOutcome(TrackedOutcome::Polymorphic);
        *emitted = true;
        return true;
    }

    MGetPropertyPolymorphic* load = MGetPropertyPolymorphic::New(alloc(), obj, name);
    current->add(load);
    current->push(load);

    for (size_t i = 0; i < receivers.length(); i++) {
        Shape* propShape = nullptr;
        if (receivers[i].shape) {
            propShape = receivers[i].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(propShape);
        }
        if (!load->addReceiver(receivers[i], propShape))
            return false;
    }

    if (failedShapeGuard_)
        load->setNotMovable();

    load->setResultType(rvalType);
    if (!pushTypeBarrier(load, types, barrier))
        return false;

    trackOptimizationOutcome(TrackedOutcome::Polymorphic);
    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryCache(bool* emitted, MDefinition* obj, PropertyName* name,
                            BarrierKind barrier, TemporaryTypeSet* types)
{
    MOZ_ASSERT(*emitted == false);

    // The input value must either be an object, or we should have strong suspicions
    // that it can be safely unboxed to an object.
    if (obj->type() != MIRType_Object) {
        TemporaryTypeSet* types = obj->resultTypeSet();
        if (!types || !types->objectOrSentinel()) {
            trackOptimizationOutcome(TrackedOutcome::NoTypeInfo);
            return true;
        }
    }

    // Since getters have no guaranteed return values, we must barrier in order to be
    // able to attach stubs for them.
    if (inspector->hasSeenAccessedGetter(pc))
        barrier = BarrierKind::TypeSet;

    // Caches can read values from prototypes, so update the barrier to
    // reflect such possible values.
    if (barrier != BarrierKind::TypeSet) {
        BarrierKind protoBarrier =
            PropertyReadOnPrototypeNeedsTypeBarrier(this, obj, name, types);
        if (protoBarrier != BarrierKind::NoBarrier) {
            MOZ_ASSERT(barrier <= protoBarrier);
            barrier = protoBarrier;
        }
    }

    MConstant* id = constant(StringValue(name));
    MGetPropertyCache* load = MGetPropertyCache::New(alloc(), obj, id,
                                                     barrier == BarrierKind::TypeSet);

    // Try to mark the cache as idempotent.
    if (obj->type() == MIRType_Object && !invalidatedIdempotentCache()) {
        if (PropertyReadIsIdempotent(constraints(), obj, name))
            load->setIdempotent();
    }

    // When we are in the context of making a call from the value returned from
    // a property, we query the typeObject for the given property name to fill
    // the InlinePropertyTable of the GetPropertyCache.  This information is
    // then used in inlineCallsite and inlineCalls, if the "this" definition is
    // matching the "object" definition of the GetPropertyCache (see
    // CanInlineGetPropertyCache).
    //
    // If this GetPropertyCache is idempotent, then we can dispatch to the right
    // function only by checking the typed object, instead of querying the value
    // of the property.  Thus this GetPropertyCache can be moved into the
    // fallback path (see inlineObjectGroupFallback).  Otherwise, we always have
    // to do the GetPropertyCache, and we can dispatch based on the JSFunction
    // value.
    if (JSOp(*pc) == JSOP_CALLPROP && load->idempotent()) {
        if (!annotateGetPropertyCache(obj, name, load, obj->resultTypeSet(), types))
            return false;
    }

    current->add(load);
    current->push(load);

    if (load->isEffectful() && !resumeAfter(load))
        return false;

    MIRType rvalType = types->getKnownMIRType();
    if (barrier != BarrierKind::NoBarrier || IsNullOrUndefined(rvalType))
        rvalType = MIRType_Value;
    load->setResultType(rvalType);

    if (!pushTypeBarrier(load, types, barrier))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTrySharedStub(bool* emitted, MDefinition* obj)
{
    MOZ_ASSERT(*emitted == false);

    // Try to emit a shared stub cache.

    if (JitOptions.disableSharedStubs)
        return true;

    MInstruction* stub = MUnarySharedStub::New(alloc(), obj);
    current->add(stub);
    current->push(stub);

    if (!resumeAfter(stub))
        return false;

    *emitted = true;
    return true;
}

MDefinition*
IonBuilder::tryInnerizeWindow(MDefinition* obj)
{
    // Try to optimize accesses on outer window proxies (window.foo, for
    // example) to go directly to the inner window, the global.
    //
    // Callers should be careful not to pass the inner object to getters or
    // setters that require outerization.

    if (obj->type() != MIRType_Object)
        return obj;

    TemporaryTypeSet* types = obj->resultTypeSet();
    if (!types)
        return obj;

    JSObject* singleton = types->maybeSingleton();
    if (!singleton)
        return obj;

    if (!IsWindowProxy(singleton))
        return obj;

    // This must be a WindowProxy for the current Window/global. Else it'd be
    // a cross-compartment wrapper and IsWindowProxy returns false for those.
    MOZ_ASSERT(ToWindowIfWindowProxy(singleton) == &script()->global());

    // When we navigate, the WindowProxy is brain transplanted and we'll mark
    // its ObjectGroup as having unknown properties. The type constraint we add
    // here will invalidate JIT code when this happens.
    TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(singleton);
    if (key->hasFlags(constraints(), OBJECT_FLAG_UNKNOWN_PROPERTIES))
        return obj;

    obj->setImplicitlyUsedUnchecked();
    return constant(ObjectValue(script()->global()));
}

bool
IonBuilder::getPropTryInnerize(bool* emitted, MDefinition* obj, PropertyName* name,
                               TemporaryTypeSet* types)
{
    // See the comment in tryInnerizeWindow for how this works.

    // Note that it's important that we do this _before_ we'd try to
    // do the optimizations below on obj normally, since some of those
    // optimizations have fallback paths that are slower than the path
    // we'd produce here.

    MOZ_ASSERT(*emitted == false);

    MDefinition* inner = tryInnerizeWindow(obj);
    if (inner == obj)
        return true;

    if (!forceInlineCaches()) {
        trackOptimizationAttempt(TrackedStrategy::GetProp_Constant);
        if (!getPropTryConstant(emitted, inner, NameToId(name), types) || *emitted)
            return *emitted;

        trackOptimizationAttempt(TrackedStrategy::GetProp_StaticName);
        if (!getStaticName(&script()->global(), name, emitted) || *emitted)
            return *emitted;

        trackOptimizationAttempt(TrackedStrategy::GetProp_CommonGetter);
        if (!getPropTryCommonGetter(emitted, inner, name, types) || *emitted)
            return *emitted;
    }

    // Passing the inner object to GetProperty IC is safe, see the
    // needsOuterizedThisObject check in IsCacheableGetPropCallNative.
    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(),
                                                       inner, name, types);
    trackOptimizationAttempt(TrackedStrategy::GetProp_InlineCache);
    if (!getPropTryCache(emitted, inner, name, barrier, types) || *emitted)
        return *emitted;

    MOZ_ASSERT(*emitted == false);
    return true;
}

bool
IonBuilder::jsop_setprop(PropertyName* name)
{
    MDefinition* value = current->pop();
    MDefinition* obj = convertUnboxedObjects(current->pop());

    bool emitted = false;
    startTrackingOptimizations();
    trackTypeInfo(TrackedTypeSite::Receiver, obj->type(), obj->resultTypeSet());
    trackTypeInfo(TrackedTypeSite::Value, value->type(), value->resultTypeSet());

    // Always use a call if we are doing the definite properties analysis and
    // not actually emitting code, to simplify later analysis.
    if (info().isAnalysis() || shouldAbortOnPreliminaryGroups(obj)) {
        bool strict = IsStrictSetPC(pc);
        MInstruction* ins = MCallSetProperty::New(alloc(), obj, value, name, strict);
        current->add(ins);
        current->push(value);
        return resumeAfter(ins);
    }

    if (!forceInlineCaches()) {
        // Try to inline a common property setter, or make a call.
        trackOptimizationAttempt(TrackedStrategy::SetProp_CommonSetter);
        if (!setPropTryCommonSetter(&emitted, obj, name, value) || emitted)
            return emitted;

        // Try to emit stores to known binary data blocks
        trackOptimizationAttempt(TrackedStrategy::SetProp_TypedObject);
        if (!setPropTryTypedObject(&emitted, obj, name, value) || emitted)
            return emitted;
    }

    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    bool barrier = PropertyWriteNeedsTypeBarrier(alloc(), constraints(), current, &obj, name, &value,
                                                 /* canModify = */ true);

    if (!forceInlineCaches()) {
        // Try to emit stores to unboxed objects.
        trackOptimizationAttempt(TrackedStrategy::SetProp_Unboxed);
        if (!setPropTryUnboxed(&emitted, obj, name, value, barrier, objTypes) || emitted)
            return emitted;
    }

    // Add post barrier if needed. The instructions above manage any post
    // barriers they need directly.
    if (NeedsPostBarrier(value))
        current->add(MPostWriteBarrier::New(alloc(), obj, value));

    if (!forceInlineCaches()) {
        // Try to emit store from definite slots.
        trackOptimizationAttempt(TrackedStrategy::SetProp_DefiniteSlot);
        if (!setPropTryDefiniteSlot(&emitted, obj, name, value, barrier, objTypes) || emitted)
            return emitted;

        // Try to emit a monomorphic/polymorphic store based on baseline caches.
        trackOptimizationAttempt(TrackedStrategy::SetProp_InlineAccess);
        if (!setPropTryInlineAccess(&emitted, obj, name, value, barrier, objTypes) || emitted)
            return emitted;
    }

    // Emit a polymorphic cache.
    trackOptimizationAttempt(TrackedStrategy::SetProp_InlineCache);
    return setPropTryCache(&emitted, obj, name, value, barrier, objTypes);
}

bool
IonBuilder::setPropTryCommonSetter(bool* emitted, MDefinition* obj,
                                   PropertyName* name, MDefinition* value)
{
    MOZ_ASSERT(*emitted == false);

    Shape* lastProperty = nullptr;
    JSFunction* commonSetter = nullptr;
    JSObject* foundProto = nullptr;
    bool isOwnProperty;
    BaselineInspector::ReceiverVector receivers(alloc());
    BaselineInspector::ObjectGroupVector convertUnboxedGroups(alloc());
    if (!inspector->commonSetPropFunction(pc, &foundProto, &lastProperty, &commonSetter,
                                          &isOwnProperty,
                                          receivers, convertUnboxedGroups))
    {
        trackOptimizationOutcome(TrackedOutcome::NoProtoFound);
        return true;
    }

    TemporaryTypeSet* objTypes = obj->resultTypeSet();
    MDefinition* guard = nullptr;
    bool canUseTIForSetter =
        testCommonGetterSetter(objTypes, name, /* isGetter = */ false,
                               foundProto, lastProperty, commonSetter, &guard);
    if (!canUseTIForSetter) {
        // If type information is bad, we can still optimize the setter if we
        // shape guard.
        obj = addShapeGuardsForGetterSetter(obj, foundProto, lastProperty,
                                            receivers, convertUnboxedGroups,
                                            isOwnProperty);
        if (!obj)
            return false;
    }

    // Emit common setter.

    // Setters can be called even if the property write needs a type
    // barrier, as calling the setter does not actually write any data
    // properties.

    // Try emitting dom call.
    if (!setPropTryCommonDOMSetter(emitted, obj, value, commonSetter, objTypes))
        return false;

    if (*emitted) {
        trackOptimizationOutcome(TrackedOutcome::DOM);
        return true;
    }

    // Don't call the setter with a primitive value.
    if (obj->type() != MIRType_Object) {
        MGuardObject* guardObj = MGuardObject::New(alloc(), obj);
        current->add(guardObj);
        obj = guardObj;
    }

    // Dummy up the stack, as in getprop. We are pushing an extra value, so
    // ensure there is enough space.
    if (!current->ensureHasSlots(3))
        return false;

    current->push(constant(ObjectValue(*commonSetter)));
    current->push(obj);
    current->push(value);

    // Call the setter. Note that we have to push the original value, not
    // the setter's return value.
    CallInfo callInfo(alloc(), false);
    if (!callInfo.init(current, 1))
        return false;

    // Ensure that we know we are calling a setter in case we inline it.
    callInfo.markAsSetter();

    // Inline the setter if we can.
    if (commonSetter->isInterpreted()) {
        InliningDecision decision = makeInliningDecision(commonSetter, callInfo);
        switch (decision) {
          case InliningDecision_Error:
            return false;
          case InliningDecision_DontInline:
          case InliningDecision_WarmUpCountTooLow:
            break;
          case InliningDecision_Inline:
            if (!inlineScriptedCall(callInfo, commonSetter))
                return false;
            *emitted = true;
            return true;
        }
    }

    MCall* call = makeCallHelper(commonSetter, callInfo);
    if (!call)
        return false;

    current->push(value);
    if (!resumeAfter(call))
        return false;

    // If the setter could have been inlined, don't track success. The call to
    // makeInliningDecision above would have tracked a specific reason why we
    // couldn't inline.
    if (!commonSetter->isInterpreted())
        trackOptimizationSuccess();

    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryCommonDOMSetter(bool* emitted, MDefinition* obj,
                                      MDefinition* value, JSFunction* setter,
                                      TemporaryTypeSet* objTypes)
{
    MOZ_ASSERT(*emitted == false);

    if (!objTypes || !objTypes->isDOMClass(constraints()))
        return true;

    if (!testShouldDOMCall(objTypes, setter, JSJitInfo::Setter))
        return true;

    // Emit SetDOMProperty.
    MOZ_ASSERT(setter->jitInfo()->type() == JSJitInfo::Setter);
    MSetDOMProperty* set = MSetDOMProperty::New(alloc(), setter->jitInfo()->setter, obj, value);

    current->add(set);
    current->push(value);

    if (!resumeAfter(set))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryTypedObject(bool* emitted, MDefinition* obj,
                                  PropertyName* name, MDefinition* value)
{
    TypedObjectPrediction fieldPrediction;
    size_t fieldOffset;
    size_t fieldIndex;
    if (!typedObjectHasField(obj, name, &fieldOffset, &fieldPrediction, &fieldIndex))
        return true;

    switch (fieldPrediction.kind()) {
      case type::Simd:
        // FIXME (bug 894104): store into a MIRType_float32x4 etc
        return true;

      case type::Reference:
        return setPropTryReferencePropOfTypedObject(emitted, obj, fieldOffset,
                                                    value, fieldPrediction, name);

      case type::Scalar:
        return setPropTryScalarPropOfTypedObject(emitted, obj, fieldOffset,
                                                 value, fieldPrediction);

      case type::Struct:
      case type::Array:
        return true;
    }

    MOZ_CRASH("Unknown kind");
}

bool
IonBuilder::setPropTryReferencePropOfTypedObject(bool* emitted,
                                                 MDefinition* obj,
                                                 int32_t fieldOffset,
                                                 MDefinition* value,
                                                 TypedObjectPrediction fieldPrediction,
                                                 PropertyName* name)
{
    ReferenceTypeDescr::Type fieldType = fieldPrediction.referenceType();

    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
        return true;

    LinearSum byteOffset(alloc());
    if (!byteOffset.add(fieldOffset))
        setForceAbort();

    if (!storeReferenceTypedObjectValue(obj, byteOffset, fieldType, value, name))
        return true;

    current->push(value);

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryScalarPropOfTypedObject(bool* emitted,
                                              MDefinition* obj,
                                              int32_t fieldOffset,
                                              MDefinition* value,
                                              TypedObjectPrediction fieldPrediction)
{
    // Must always be loading the same scalar type
    Scalar::Type fieldType = fieldPrediction.scalarType();

    // Don't optimize if the typed object might be neutered.
    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (globalKey->hasFlags(constraints(), OBJECT_FLAG_TYPED_OBJECT_NEUTERED))
        return true;

    LinearSum byteOffset(alloc());
    if (!byteOffset.add(fieldOffset))
        setForceAbort();

    if (!storeScalarTypedObjectValue(obj, byteOffset, fieldType, value))
        return false;

    current->push(value);

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryDefiniteSlot(bool* emitted, MDefinition* obj,
                                   PropertyName* name, MDefinition* value,
                                   bool barrier, TemporaryTypeSet* objTypes)
{
    MOZ_ASSERT(*emitted == false);

    if (barrier) {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
        return true;
    }

    uint32_t nfixed;
    uint32_t slot = getDefiniteSlot(obj->resultTypeSet(), name, &nfixed);
    if (slot == UINT32_MAX)
        return true;

    bool writeBarrier = false;
    for (size_t i = 0; i < obj->resultTypeSet()->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = obj->resultTypeSet()->getObject(i);
        if (!key)
            continue;

        HeapTypeSetKey property = key->property(NameToId(name));
        if (property.nonWritable(constraints())) {
            trackOptimizationOutcome(TrackedOutcome::NonWritableProperty);
            return true;
        }
        writeBarrier |= property.needsBarrier(constraints());
    }

    MInstruction* store;
    if (slot < nfixed) {
        store = MStoreFixedSlot::New(alloc(), obj, slot, value);
        if (writeBarrier)
            store->toStoreFixedSlot()->setNeedsBarrier();
    } else {
        MInstruction* slots = MSlots::New(alloc(), obj);
        current->add(slots);

        store = MStoreSlot::New(alloc(), slots, slot - nfixed, value);
        if (writeBarrier)
            store->toStoreSlot()->setNeedsBarrier();
    }

    current->add(store);
    current->push(value);

    if (!resumeAfter(store))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

MInstruction*
IonBuilder::storeUnboxedProperty(MDefinition* obj, size_t offset, JSValueType unboxedType,
                                 MDefinition* value)
{
    size_t scaledOffsetConstant = offset / UnboxedTypeSize(unboxedType);
    MInstruction* scaledOffset = MConstant::New(alloc(), Int32Value(scaledOffsetConstant));
    current->add(scaledOffset);

    return storeUnboxedValue(obj, obj, UnboxedPlainObject::offsetOfData(),
                             scaledOffset, unboxedType, value);
}

MInstruction*
IonBuilder::storeUnboxedValue(MDefinition* obj, MDefinition* elements, int32_t elementsOffset,
                              MDefinition* scaledOffset, JSValueType unboxedType,
                              MDefinition* value, bool preBarrier /* = true */)
{
    MInstruction* store;
    switch (unboxedType) {
      case JSVAL_TYPE_BOOLEAN:
        store = MStoreUnboxedScalar::New(alloc(), elements, scaledOffset, value, Scalar::Uint8,
                                         MStoreUnboxedScalar::DontTruncateInput,
                                         DoesNotRequireMemoryBarrier, elementsOffset);
        break;

      case JSVAL_TYPE_INT32:
        store = MStoreUnboxedScalar::New(alloc(), elements, scaledOffset, value, Scalar::Int32,
                                         MStoreUnboxedScalar::DontTruncateInput,
                                         DoesNotRequireMemoryBarrier, elementsOffset);
        break;

      case JSVAL_TYPE_DOUBLE:
        store = MStoreUnboxedScalar::New(alloc(), elements, scaledOffset, value, Scalar::Float64,
                                         MStoreUnboxedScalar::DontTruncateInput,
                                         DoesNotRequireMemoryBarrier, elementsOffset);
        break;

      case JSVAL_TYPE_STRING:
        store = MStoreUnboxedString::New(alloc(), elements, scaledOffset, value,
                                         elementsOffset, preBarrier);
        break;

      case JSVAL_TYPE_OBJECT:
        store = MStoreUnboxedObjectOrNull::New(alloc(), elements, scaledOffset, value, obj,
                                               elementsOffset, preBarrier);
        break;

      default:
        MOZ_CRASH();
    }

    current->add(store);
    return store;
}

bool
IonBuilder::setPropTryUnboxed(bool* emitted, MDefinition* obj,
                              PropertyName* name, MDefinition* value,
                              bool barrier, TemporaryTypeSet* objTypes)
{
    MOZ_ASSERT(*emitted == false);

    if (barrier) {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
        return true;
    }

    JSValueType unboxedType;
    uint32_t offset = getUnboxedOffset(obj->resultTypeSet(), name, &unboxedType);
    if (offset == UINT32_MAX)
        return true;

    if (obj->type() != MIRType_Object) {
        MGuardObject* guard = MGuardObject::New(alloc(), obj);
        current->add(guard);
        obj = guard;
    }

    MInstruction* store = storeUnboxedProperty(obj, offset, unboxedType, value);

    current->push(value);

    if (!resumeAfter(store))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryInlineAccess(bool* emitted, MDefinition* obj,
                                   PropertyName* name, MDefinition* value,
                                   bool barrier, TemporaryTypeSet* objTypes)
{
    MOZ_ASSERT(*emitted == false);

    if (barrier) {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
        return true;
    }

    BaselineInspector::ReceiverVector receivers(alloc());
    BaselineInspector::ObjectGroupVector convertUnboxedGroups(alloc());
    if (!inspector->maybeInfoForPropertyOp(pc, receivers, convertUnboxedGroups))
        return false;

    if (!canInlinePropertyOpShapes(receivers))
        return true;

    obj = convertUnboxedObjects(obj, convertUnboxedGroups);

    if (receivers.length() == 1) {
        if (!receivers[0].group) {
            // Monomorphic store to a native object.
            spew("Inlining monomorphic native SETPROP");

            obj = addShapeGuard(obj, receivers[0].shape, Bailout_ShapeGuard);

            Shape* shape = receivers[0].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(shape);

            bool needsBarrier = objTypes->propertyNeedsBarrier(constraints(), NameToId(name));
            if (!storeSlot(obj, shape, value, needsBarrier))
                return false;

            trackOptimizationOutcome(TrackedOutcome::Monomorphic);
            *emitted = true;
            return true;
        }

        if (receivers[0].shape) {
            // Monomorphic store to an unboxed object expando.
            spew("Inlining monomorphic unboxed expando SETPROP");

            obj = addGroupGuard(obj, receivers[0].group, Bailout_ShapeGuard);
            obj = addUnboxedExpandoGuard(obj, /* hasExpando = */ true, Bailout_ShapeGuard);

            MInstruction* expando = MLoadUnboxedExpando::New(alloc(), obj);
            current->add(expando);

            expando = addShapeGuard(expando, receivers[0].shape, Bailout_ShapeGuard);

            Shape* shape = receivers[0].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(shape);

            bool needsBarrier = objTypes->propertyNeedsBarrier(constraints(), NameToId(name));
            if (!storeSlot(expando, shape, value, needsBarrier))
                return false;

            trackOptimizationOutcome(TrackedOutcome::Monomorphic);
            *emitted = true;
            return true;
        }

        // Monomorphic store to an unboxed object.
        spew("Inlining monomorphic unboxed SETPROP");

        ObjectGroup* group = receivers[0].group;
        obj = addGroupGuard(obj, group, Bailout_ShapeGuard);

        const UnboxedLayout::Property* property = group->unboxedLayout().lookup(name);
        storeUnboxedProperty(obj, property->offset, property->type, value);

        current->push(value);

        trackOptimizationOutcome(TrackedOutcome::Monomorphic);
        *emitted = true;
        return true;
    }

    MOZ_ASSERT(receivers.length() > 1);
    spew("Inlining polymorphic SETPROP");

    if (Shape* propShape = PropertyShapesHaveSameSlot(receivers, NameToId(name))) {
        obj = addGuardReceiverPolymorphic(obj, receivers);
        if (!obj)
            return false;

        bool needsBarrier = objTypes->propertyNeedsBarrier(constraints(), NameToId(name));
        if (!storeSlot(obj, propShape, value, needsBarrier))
            return false;

        trackOptimizationOutcome(TrackedOutcome::Polymorphic);
        *emitted = true;
        return true;
    }

    MSetPropertyPolymorphic* ins = MSetPropertyPolymorphic::New(alloc(), obj, value, name);
    current->add(ins);
    current->push(value);

    for (size_t i = 0; i < receivers.length(); i++) {
        Shape* propShape = nullptr;
        if (receivers[i].shape) {
            propShape = receivers[i].shape->searchLinear(NameToId(name));
            MOZ_ASSERT(propShape);
        }
        if (!ins->addReceiver(receivers[i], propShape))
            return false;
    }

    if (objTypes->propertyNeedsBarrier(constraints(), NameToId(name)))
        ins->setNeedsBarrier();

    if (!resumeAfter(ins))
        return false;

    trackOptimizationOutcome(TrackedOutcome::Polymorphic);
    *emitted = true;
    return true;
}

bool
IonBuilder::setPropTryCache(bool* emitted, MDefinition* obj,
                            PropertyName* name, MDefinition* value,
                            bool barrier, TemporaryTypeSet* objTypes)
{
    MOZ_ASSERT(*emitted == false);

    bool strict = IsStrictSetPC(pc);

    MConstant* id = constant(StringValue(name));
    MSetPropertyCache* ins = MSetPropertyCache::New(alloc(), obj, id, value, strict, barrier,
                                                    /* guardHoles = */ false);
    current->add(ins);
    current->push(value);

    if (!resumeAfter(ins))
        return false;

    trackOptimizationSuccess();
    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_delprop(PropertyName* name)
{
    MDefinition* obj = current->pop();

    bool strict = JSOp(*pc) == JSOP_STRICTDELPROP;
    MInstruction* ins = MDeleteProperty::New(alloc(), obj, name, strict);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_delelem()
{
    MDefinition* index = current->pop();
    MDefinition* obj = current->pop();

    bool strict = JSOp(*pc) == JSOP_STRICTDELELEM;
    MDeleteElement* ins = MDeleteElement::New(alloc(), obj, index, strict);
    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_regexp(RegExpObject* reobj)
{
    // JS semantics require regular expression literals to create different
    // objects every time they execute. We only need to do this cloning if the
    // script could actually observe the effect of such cloning, for instance
    // by getting or setting properties on it.
    //
    // First, make sure the regex is one we can safely optimize. Lowering can
    // then check if this regex object only flows into known natives and can
    // avoid cloning in this case.

    bool mustClone = true;
    TypeSet::ObjectKey* globalKey = TypeSet::ObjectKey::get(&script()->global());
    if (!globalKey->hasFlags(constraints(), OBJECT_FLAG_REGEXP_FLAGS_SET)) {
#ifdef DEBUG
        // Only compare the statics if the one on script()->global() has been
        // instantiated.
        if (script()->global().hasRegExpStatics()) {
            RegExpStatics* res = script()->global().getAlreadyCreatedRegExpStatics();
            MOZ_ASSERT(res);
            uint32_t origFlags = reobj->getFlags();
            uint32_t staticsFlags = res->getFlags();
            MOZ_ASSERT((origFlags & staticsFlags) == staticsFlags);
        }
#endif

        if (!reobj->global() && !reobj->sticky())
            mustClone = false;
    }

    MRegExp* regexp = MRegExp::New(alloc(), constraints(), reobj, mustClone);
    current->add(regexp);
    current->push(regexp);

    return true;
}

bool
IonBuilder::jsop_object(JSObject* obj)
{
    if (options.cloneSingletons()) {
        MCloneLiteral* clone = MCloneLiteral::New(alloc(), constant(ObjectValue(*obj)));
        current->add(clone);
        current->push(clone);
        return resumeAfter(clone);
    }

    compartment->setSingletonsAsValues();
    pushConstant(ObjectValue(*obj));
    return true;
}

bool
IonBuilder::jsop_lambda(JSFunction* fun)
{
    MOZ_ASSERT(analysis().usesScopeChain());
    MOZ_ASSERT(!fun->isArrow());

    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
        return abort("asm.js module function");

    MConstant* cst = MConstant::NewConstraintlessObject(alloc(), fun);
    current->add(cst);
    MLambda* ins = MLambda::New(alloc(), constraints(), current->scopeChain(), cst);
    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_lambda_arrow(JSFunction* fun)
{
    MOZ_ASSERT(analysis().usesScopeChain());
    MOZ_ASSERT(fun->isArrow());
    MOZ_ASSERT(!fun->isNative());

    MDefinition* newTargetDef = current->pop();
    MLambdaArrow* ins = MLambdaArrow::New(alloc(), constraints(), current->scopeChain(),
                                          newTargetDef, fun);
    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_setarg(uint32_t arg)
{
    // To handle this case, we should spill the arguments to the space where
    // actual arguments are stored. The tricky part is that if we add a MIR
    // to wrap the spilling action, we don't want the spilling to be
    // captured by the GETARG and by the resume point, only by
    // MGetFrameArgument.
    MOZ_ASSERT(analysis_.hasSetArg());
    MDefinition* val = current->peek(-1);

    // If an arguments object is in use, and it aliases formals, then all SETARGs
    // must go through the arguments object.
    if (info().argsObjAliasesFormals()) {
        if (NeedsPostBarrier(val))
            current->add(MPostWriteBarrier::New(alloc(), current->argumentsObject(), val));
        current->add(MSetArgumentsObjectArg::New(alloc(), current->argumentsObject(),
                                                 GET_ARGNO(pc), val));
        return true;
    }

    // :TODO: if hasArguments() is true, and the script has a JSOP_SETARG, then
    // convert all arg accesses to go through the arguments object. (see Bug 957475)
    if (info().hasArguments())
	return abort("NYI: arguments & setarg.");

    // Otherwise, if a magic arguments is in use, and it aliases formals, and there exist
    // arguments[...] GETELEM expressions in the script, then SetFrameArgument must be used.
    // If no arguments[...] GETELEM expressions are in the script, and an argsobj is not
    // required, then it means that any aliased argument set can never be observed, and
    // the frame does not actually need to be updated with the new arg value.
    if (info().argumentsAliasesFormals()) {
        // JSOP_SETARG with magic arguments within inline frames is not yet supported.
        MOZ_ASSERT(script()->uninlineable() && !isInlineBuilder());

        MSetFrameArgument* store = MSetFrameArgument::New(alloc(), arg, val);
        modifiesFrameArguments_ = true;
        current->add(store);
        current->setArg(arg);
        return true;
    }

    // If this assignment is at the start of the function and is coercing
    // the original value for the argument which was passed in, loosen
    // the type information for that original argument if it is currently
    // empty due to originally executing in the interpreter.
    if (graph().numBlocks() == 1 &&
        (val->isBitOr() || val->isBitAnd() || val->isMul() /* for JSOP_POS */))
     {
         for (size_t i = 0; i < val->numOperands(); i++) {
            MDefinition* op = val->getOperand(i);
            if (op->isParameter() &&
                op->toParameter()->index() == (int32_t)arg &&
                op->resultTypeSet() &&
                op->resultTypeSet()->empty())
            {
                bool otherUses = false;
                for (MUseDefIterator iter(op); iter; iter++) {
                    MDefinition* def = iter.def();
                    if (def == val)
                        continue;
                    otherUses = true;
                }
                if (!otherUses) {
                    MOZ_ASSERT(op->resultTypeSet() == &argTypes[arg]);
                    argTypes[arg].addType(TypeSet::UnknownType(), alloc_->lifoAlloc());
                    if (val->isMul()) {
                        val->setResultType(MIRType_Double);
                        val->toMul()->setSpecialization(MIRType_Double);
                    } else {
                        MOZ_ASSERT(val->type() == MIRType_Int32);
                    }
                    val->setResultTypeSet(nullptr);
                }
            }
        }
    }

    current->setArg(arg);
    return true;
}

bool
IonBuilder::jsop_defvar(uint32_t index)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_DEFVAR);

    PropertyName* name = script()->getName(index);

    // Bake in attrs.
    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    MOZ_ASSERT(!script()->isForEval());

    // Pass the ScopeChain.
    MOZ_ASSERT(analysis().usesScopeChain());

    // Bake the name pointer into the MDefVar.
    MDefVar* defvar = MDefVar::New(alloc(), name, attrs, current->scopeChain());
    current->add(defvar);

    return resumeAfter(defvar);
}

bool
IonBuilder::jsop_deflexical(uint32_t index)
{
    MOZ_ASSERT(!script()->hasNonSyntacticScope());
    MOZ_ASSERT(JSOp(*pc) == JSOP_DEFLET || JSOp(*pc) == JSOP_DEFCONST);

    PropertyName* name = script()->getName(index);
    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    if (JSOp(*pc) == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    MDefLexical* deflex = MDefLexical::New(alloc(), name, attrs);
    current->add(deflex);

    return resumeAfter(deflex);
}

bool
IonBuilder::jsop_deffun(uint32_t index)
{
    JSFunction* fun = script()->getFunction(index);
    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
        return abort("asm.js module function");

    MOZ_ASSERT(analysis().usesScopeChain());

    MDefFun* deffun = MDefFun::New(alloc(), fun, current->scopeChain());
    current->add(deffun);

    return resumeAfter(deffun);
}

bool
IonBuilder::jsop_throwsetconst()
{
    current->peek(-1)->setImplicitlyUsedUnchecked();
    MInstruction* lexicalError = MThrowRuntimeLexicalError::New(alloc(), JSMSG_BAD_CONST_ASSIGN);
    current->add(lexicalError);
    return resumeAfter(lexicalError);
}

bool
IonBuilder::jsop_checklexical()
{
    uint32_t slot = info().localSlot(GET_LOCALNO(pc));
    MDefinition* lexical = addLexicalCheck(current->getSlot(slot));
    if (!lexical)
        return false;
    current->setSlot(slot, lexical);
    return true;
}

bool
IonBuilder::jsop_checkaliasedlet(ScopeCoordinate sc)
{
    MDefinition* let = addLexicalCheck(getAliasedVar(sc));
    if (!let)
        return false;

    jsbytecode* nextPc = pc + JSOP_CHECKALIASEDLEXICAL_LENGTH;
    MOZ_ASSERT(JSOp(*nextPc) == JSOP_GETALIASEDVAR ||
               JSOp(*nextPc) == JSOP_SETALIASEDVAR ||
               JSOp(*nextPc) == JSOP_THROWSETALIASEDCONST);
    MOZ_ASSERT(sc == ScopeCoordinate(nextPc));

    // If we are checking for a load, push the checked let so that the load
    // can use it.
    if (JSOp(*nextPc) == JSOP_GETALIASEDVAR)
        setLexicalCheck(let);

    return true;
}

bool
IonBuilder::jsop_functionthis()
{
    MOZ_ASSERT(info().funMaybeLazy());
    MOZ_ASSERT(!info().funMaybeLazy()->isArrow());

    if (script()->strict() || info().funMaybeLazy()->isSelfHostedBuiltin()) {
        // No need to wrap primitive |this| in strict mode or self-hosted code.
        current->pushSlot(info().thisSlot());
        return true;
    }

    if (thisTypes && (thisTypes->getKnownMIRType() == MIRType_Object ||
        (thisTypes->empty() && baselineFrame_ && baselineFrame_->thisType.isSomeObject())))
    {
        // This is safe, because if the entry type of |this| is an object, it
        // will necessarily be an object throughout the entire function. OSR
        // can introduce a phi, but this phi will be specialized.
        current->pushSlot(info().thisSlot());
        return true;
    }

    // If we are doing an analysis, we might not yet know the type of |this|.
    // Instead of bailing out just push the |this| slot, as this code won't
    // actually execute and it does not matter whether |this| is primitive.
    if (info().isAnalysis()) {
        current->pushSlot(info().thisSlot());
        return true;
    }

    // Hard case: |this| may be a primitive we have to wrap.
    MDefinition* def = current->getSlot(info().thisSlot());

    if (def->type() == MIRType_Object) {
        current->push(def);
        return true;
    }

    MComputeThis* thisObj = MComputeThis::New(alloc(), def);
    current->add(thisObj);
    current->push(thisObj);

    return resumeAfter(thisObj);
}

bool
IonBuilder::jsop_globalthis()
{
    if (script()->hasNonSyntacticScope()) {
        // Ion does not compile global scripts with a non-syntactic scope, but
        // we can end up here when we're compiling an arrow function.
        return abort("JSOP_GLOBALTHIS in script with non-syntactic scope");
    }

    ClonedBlockObject* globalLexical = &script()->global().lexicalScope();
    pushConstant(globalLexical->thisValue());
    return true;
}

bool
IonBuilder::jsop_typeof()
{
    MDefinition* input = current->pop();
    MTypeOf* ins = MTypeOf::New(alloc(), input, input->type());

    ins->cacheInputMaybeCallableOrEmulatesUndefined(constraints());

    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_toid()
{
    // No-op if the index is an integer.
    if (current->peek(-1)->type() == MIRType_Int32)
        return true;

    MDefinition* index = current->pop();
    MToId* ins = MToId::New(alloc(), index);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iter(uint8_t flags)
{
    if (flags != JSITER_ENUMERATE)
        nonStringIteration_ = true;

    MDefinition* obj = current->pop();
    MInstruction* ins = MIteratorStart::New(alloc(), obj, flags);

    if (!iterators_.append(ins))
        return false;

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_itermore()
{
    MDefinition* iter = current->peek(-1);
    MInstruction* ins = MIteratorMore::New(alloc(), iter);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_isnoiter()
{
    MDefinition* def = current->peek(-1);
    MOZ_ASSERT(def->isIteratorMore());

    MInstruction* ins = MIsNoIter::New(alloc(), def);
    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_iterend()
{
    MDefinition* iter = current->pop();
    MInstruction* ins = MIteratorEnd::New(alloc(), iter);

    current->add(ins);

    return resumeAfter(ins);
}

MDefinition*
IonBuilder::walkScopeChain(unsigned hops)
{
    MDefinition* scope = current->getSlot(info().scopeChainSlot());

    for (unsigned i = 0; i < hops; i++) {
        MInstruction* ins = MEnclosingScope::New(alloc(), scope);
        current->add(ins);
        scope = ins;
    }

    return scope;
}

bool
IonBuilder::hasStaticScopeObject(ScopeCoordinate sc, JSObject** pcall)
{
    JSScript* outerScript = ScopeCoordinateFunctionScript(script(), pc);
    if (!outerScript || !outerScript->treatAsRunOnce())
        return false;

    TypeSet::ObjectKey* funKey =
        TypeSet::ObjectKey::get(outerScript->functionNonDelazifying());
    if (funKey->hasFlags(constraints(), OBJECT_FLAG_RUNONCE_INVALIDATED))
        return false;

    // The script this aliased var operation is accessing will run only once,
    // so there will be only one call object and the aliased var access can be
    // compiled in the same manner as a global access. We still need to find
    // the call object though.

    // Look for the call object on the current script's function's scope chain.
    // If the current script is inner to the outer script and the function has
    // singleton type then it should show up here.

    MDefinition* scope = current->getSlot(info().scopeChainSlot());
    scope->setImplicitlyUsedUnchecked();

    JSObject* environment = script()->functionNonDelazifying()->environment();
    while (environment && !environment->is<GlobalObject>()) {
        if (environment->is<CallObject>() &&
            !environment->as<CallObject>().isForEval() &&
            environment->as<CallObject>().callee().nonLazyScript() == outerScript)
        {
            MOZ_ASSERT(environment->isSingleton());
            *pcall = environment;
            return true;
        }
        environment = environment->enclosingScope();
    }

    // Look for the call object on the current frame, if we are compiling the
    // outer script itself. Don't do this if we are at entry to the outer
    // script, as the call object we see will not be the real one --- after
    // entering the Ion code a different call object will be created.

    if (script() == outerScript && baselineFrame_ && info().osrPc()) {
        JSObject* singletonScope = baselineFrame_->singletonScopeChain;
        if (singletonScope &&
            singletonScope->is<CallObject>() &&
            singletonScope->as<CallObject>().callee().nonLazyScript() == outerScript)
        {
            MOZ_ASSERT(singletonScope->isSingleton());
            *pcall = singletonScope;
            return true;
        }
    }

    return true;
}

MDefinition*
IonBuilder::getAliasedVar(ScopeCoordinate sc)
{
    MDefinition* obj = walkScopeChain(sc.hops());

    Shape* shape = ScopeCoordinateToStaticScopeShape(script(), pc);

    MInstruction* load;
    if (shape->numFixedSlots() <= sc.slot()) {
        MInstruction* slots = MSlots::New(alloc(), obj);
        current->add(slots);

        load = MLoadSlot::New(alloc(), slots, sc.slot() - shape->numFixedSlots());
    } else {
        load = MLoadFixedSlot::New(alloc(), obj, sc.slot());
    }

    current->add(load);
    return load;
}

bool
IonBuilder::jsop_getaliasedvar(ScopeCoordinate sc)
{
    JSObject* call = nullptr;
    if (hasStaticScopeObject(sc, &call) && call) {
        PropertyName* name = ScopeCoordinateName(scopeCoordinateNameCache, script(), pc);
        bool emitted = false;
        if (!getStaticName(call, name, &emitted, takeLexicalCheck()) || emitted)
            return emitted;
    }

    // See jsop_checkaliasedlet.
    MDefinition* load = takeLexicalCheck();
    if (!load)
        load = getAliasedVar(sc);
    current->push(load);

    TemporaryTypeSet* types = bytecodeTypes(pc);
    return pushTypeBarrier(load, types, BarrierKind::TypeSet);
}

bool
IonBuilder::jsop_setaliasedvar(ScopeCoordinate sc)
{
    JSObject* call = nullptr;
    if (hasStaticScopeObject(sc, &call)) {
        uint32_t depth = current->stackDepth() + 1;
        if (depth > current->nslots()) {
            if (!current->increaseSlots(depth - current->nslots()))
                return false;
        }
        MDefinition* value = current->pop();
        PropertyName* name = ScopeCoordinateName(scopeCoordinateNameCache, script(), pc);

        if (call) {
            // Push the object on the stack to match the bound object expected in
            // the global and property set cases.
            pushConstant(ObjectValue(*call));
            current->push(value);
            return setStaticName(call, name);
        }

        // The call object has type information we need to respect but we
        // couldn't find it. Just do a normal property assign.
        MDefinition* obj = walkScopeChain(sc.hops());
        current->push(obj);
        current->push(value);
        return jsop_setprop(name);
    }

    MDefinition* rval = current->peek(-1);
    MDefinition* obj = walkScopeChain(sc.hops());

    Shape* shape = ScopeCoordinateToStaticScopeShape(script(), pc);

    if (NeedsPostBarrier(rval))
        current->add(MPostWriteBarrier::New(alloc(), obj, rval));

    MInstruction* store;
    if (shape->numFixedSlots() <= sc.slot()) {
        MInstruction* slots = MSlots::New(alloc(), obj);
        current->add(slots);

        store = MStoreSlot::NewBarriered(alloc(), slots, sc.slot() - shape->numFixedSlots(), rval);
    } else {
        store = MStoreFixedSlot::NewBarriered(alloc(), obj, sc.slot(), rval);
    }

    current->add(store);
    return resumeAfter(store);
}

bool
IonBuilder::jsop_in()
{
    MDefinition* obj = convertUnboxedObjects(current->pop());
    MDefinition* id = current->pop();

    do {
        if (shouldAbortOnPreliminaryGroups(obj))
            break;

        JSValueType unboxedType = UnboxedArrayElementType(constraints(), obj, id);
        if (unboxedType == JSVAL_TYPE_MAGIC) {
            if (!ElementAccessIsDenseNative(constraints(), obj, id))
                break;
        }

        if (ElementAccessHasExtraIndexedProperty(this, obj))
            break;

        return jsop_in_dense(obj, id, unboxedType);
    } while (false);

    MIn* ins = MIn::New(alloc(), id, obj);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_in_dense(MDefinition* obj, MDefinition* id, JSValueType unboxedType)
{
    bool needsHoleCheck = !ElementAccessIsPacked(constraints(), obj);

    // Ensure id is an integer.
    MInstruction* idInt32 = MToInt32::New(alloc(), id);
    current->add(idInt32);
    id = idInt32;

    // Get the elements vector.
    MElements* elements = MElements::New(alloc(), obj, unboxedType != JSVAL_TYPE_MAGIC);
    current->add(elements);

    MInstruction* initLength = initializedLength(obj, elements, unboxedType);

    // If there are no holes, speculate the InArray check will not fail.
    if (!needsHoleCheck && !failedBoundsCheck_) {
        addBoundsCheck(idInt32, initLength);
        return pushConstant(BooleanValue(true));
    }

    // Check if id < initLength and elem[id] not a hole.
    MInArray* ins = MInArray::New(alloc(), elements, id, initLength, obj, needsHoleCheck,
                                  unboxedType);

    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::hasOnProtoChain(TypeSet::ObjectKey* key, JSObject* protoObject, bool* hasOnProto)
{
    MOZ_ASSERT(protoObject);

    while (true) {
        if (!key->hasStableClassAndProto(constraints()) || !key->clasp()->isNative())
            return false;

        JSObject* proto = checkNurseryObject(key->proto().toObjectOrNull());
        if (!proto) {
            *hasOnProto = false;
            return true;
        }

        if (proto == protoObject) {
            *hasOnProto = true;
            return true;
        }

        key = TypeSet::ObjectKey::get(proto);
    }

    MOZ_CRASH("Unreachable");
}

bool
IonBuilder::tryFoldInstanceOf(MDefinition* lhs, JSObject* protoObject)
{
    // Try to fold the js::IsDelegate part of the instanceof operation.

    if (!lhs->mightBeType(MIRType_Object)) {
        // If the lhs is a primitive, the result is false.
        lhs->setImplicitlyUsedUnchecked();
        pushConstant(BooleanValue(false));
        return true;
    }

    TemporaryTypeSet* lhsTypes = lhs->resultTypeSet();
    if (!lhsTypes || lhsTypes->unknownObject())
        return false;

    // We can fold if either all objects have protoObject on their proto chain
    // or none have.
    bool isFirst = true;
    bool knownIsInstance = false;

    for (unsigned i = 0; i < lhsTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = lhsTypes->getObject(i);
        if (!key)
            continue;

        bool isInstance;
        if (!hasOnProtoChain(key, protoObject, &isInstance))
            return false;

        if (isFirst) {
            knownIsInstance = isInstance;
            isFirst = false;
        } else if (knownIsInstance != isInstance) {
            // Some of the objects have protoObject on their proto chain and
            // others don't, so we can't optimize this.
            return false;
        }
    }

    if (knownIsInstance && lhsTypes->getKnownMIRType() != MIRType_Object) {
        // The result is true for all objects, but the lhs might be a primitive.
        // We can't fold this completely but we can use a much faster IsObject
        // test.
        MIsObject* isObject = MIsObject::New(alloc(), lhs);
        current->add(isObject);
        current->push(isObject);
        return true;
    }

    lhs->setImplicitlyUsedUnchecked();
    pushConstant(BooleanValue(knownIsInstance));
    return true;
}

bool
IonBuilder::jsop_instanceof()
{
    MDefinition* rhs = current->pop();
    MDefinition* obj = current->pop();

    // If this is an 'x instanceof function' operation and we can determine the
    // exact function and prototype object being tested for, use a typed path.
    do {
        TemporaryTypeSet* rhsTypes = rhs->resultTypeSet();
        JSObject* rhsObject = rhsTypes ? rhsTypes->maybeSingleton() : nullptr;
        if (!rhsObject || !rhsObject->is<JSFunction>() || rhsObject->isBoundFunction())
            break;

        TypeSet::ObjectKey* rhsKey = TypeSet::ObjectKey::get(rhsObject);
        if (rhsKey->unknownProperties())
            break;

        HeapTypeSetKey protoProperty =
            rhsKey->property(NameToId(names().prototype));
        JSObject* protoObject = protoProperty.singleton(constraints());
        if (!protoObject)
            break;

        rhs->setImplicitlyUsedUnchecked();

        if (tryFoldInstanceOf(obj, protoObject))
            return true;

        MInstanceOf* ins = MInstanceOf::New(alloc(), obj, protoObject);

        current->add(ins);
        current->push(ins);

        return resumeAfter(ins);
    } while (false);

    // Try to inline a fast path based on Baseline ICs.
    do {
        Shape* shape;
        uint32_t slot;
        JSObject* protoObject;
        if (!inspector->instanceOfData(pc, &shape, &slot, &protoObject))
            break;

        // Shape guard.
        rhs = addShapeGuard(rhs, shape, Bailout_ShapeGuard);

        // Guard .prototype == protoObject.
        MOZ_ASSERT(shape->numFixedSlots() == 0, "Must be a dynamic slot");
        MSlots* slots = MSlots::New(alloc(), rhs);
        current->add(slots);
        MLoadSlot* prototype = MLoadSlot::New(alloc(), slots, slot);
        current->add(prototype);
        MConstant* protoConst = MConstant::NewConstraintlessObject(alloc(), protoObject);
        current->add(protoConst);
        MGuardObjectIdentity* guard = MGuardObjectIdentity::New(alloc(), prototype, protoConst,
                                                                /* bailOnEquality = */ false);
        current->add(guard);

        if (tryFoldInstanceOf(obj, protoObject))
            return true;

        MInstanceOf* ins = MInstanceOf::New(alloc(), obj, protoObject);
        current->add(ins);
        current->push(ins);
        return resumeAfter(ins);
    } while (false);

    MCallInstanceOf* ins = MCallInstanceOf::New(alloc(), obj, rhs);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_debugger()
{
    MDebugger* debugger = MDebugger::New(alloc());
    current->add(debugger);

    // The |debugger;| statement will always bail out to baseline if
    // cx->compartment()->isDebuggee(). Resume in-place and have baseline
    // handle the details.
    return resumeAt(debugger, pc);
}

MInstruction*
IonBuilder::addConvertElementsToDoubles(MDefinition* elements)
{
    MInstruction* convert = MConvertElementsToDoubles::New(alloc(), elements);
    current->add(convert);
    return convert;
}

MDefinition*
IonBuilder::addMaybeCopyElementsForWrite(MDefinition* object, bool checkNative)
{
    if (!ElementAccessMightBeCopyOnWrite(constraints(), object))
        return object;
    MInstruction* copy = MMaybeCopyElementsForWrite::New(alloc(), object, checkNative);
    current->add(copy);
    return copy;
}

MInstruction*
IonBuilder::addBoundsCheck(MDefinition* index, MDefinition* length)
{
    MInstruction* check = MBoundsCheck::New(alloc(), index, length);
    current->add(check);

    // If a bounds check failed in the past, don't optimize bounds checks.
    if (failedBoundsCheck_)
        check->setNotMovable();

    return check;
}

MInstruction*
IonBuilder::addShapeGuard(MDefinition* obj, Shape* const shape, BailoutKind bailoutKind)
{
    MGuardShape* guard = MGuardShape::New(alloc(), obj, shape, bailoutKind);
    current->add(guard);

    // If a shape guard failed in the past, don't optimize shape guard.
    if (failedShapeGuard_)
        guard->setNotMovable();

    return guard;
}

MInstruction*
IonBuilder::addGroupGuard(MDefinition* obj, ObjectGroup* group, BailoutKind bailoutKind)
{
    MGuardObjectGroup* guard = MGuardObjectGroup::New(alloc(), obj, group,
                                                      /* bailOnEquality = */ false, bailoutKind);
    current->add(guard);

    // If a shape guard failed in the past, don't optimize group guards.
    if (failedShapeGuard_)
        guard->setNotMovable();

    LifoAlloc* lifoAlloc = alloc().lifoAlloc();
    guard->setResultTypeSet(lifoAlloc->new_<TemporaryTypeSet>(lifoAlloc,
                                                            TypeSet::ObjectType(group)));

    return guard;
}

MInstruction*
IonBuilder::addUnboxedExpandoGuard(MDefinition* obj, bool hasExpando, BailoutKind bailoutKind)
{
    MGuardUnboxedExpando* guard = MGuardUnboxedExpando::New(alloc(), obj, hasExpando, bailoutKind);
    current->add(guard);

    // If a shape guard failed in the past, don't optimize group guards.
    if (failedShapeGuard_)
        guard->setNotMovable();

    return guard;
}

MInstruction*
IonBuilder::addGuardReceiverPolymorphic(MDefinition* obj,
                                        const BaselineInspector::ReceiverVector& receivers)
{
    if (receivers.length() == 1) {
        if (!receivers[0].group) {
            // Monomorphic guard on a native object.
            return addShapeGuard(obj, receivers[0].shape, Bailout_ShapeGuard);
        }

        if (!receivers[0].shape) {
            // Guard on an unboxed object that does not have an expando.
            obj = addGroupGuard(obj, receivers[0].group, Bailout_ShapeGuard);
            return addUnboxedExpandoGuard(obj, /* hasExpando = */ false, Bailout_ShapeGuard);
        }

        // Monomorphic receiver guards are not yet supported when the receiver
        // is an unboxed object with an expando.
    }

    MGuardReceiverPolymorphic* guard = MGuardReceiverPolymorphic::New(alloc(), obj);
    current->add(guard);

    if (failedShapeGuard_)
        guard->setNotMovable();

    for (size_t i = 0; i < receivers.length(); i++) {
        if (!guard->addReceiver(receivers[i]))
            return nullptr;
    }

    return guard;
}

MInstruction*
IonBuilder::addSharedTypedArrayGuard(MDefinition* obj)
{
    MGuardSharedTypedArray* guard = MGuardSharedTypedArray::New(alloc(), obj);
    current->add(guard);
    return guard;
}

TemporaryTypeSet*
IonBuilder::bytecodeTypes(jsbytecode* pc)
{
    return TypeScript::BytecodeTypes(script(), pc, bytecodeTypeMap, &typeArrayHint, typeArray);
}

TypedObjectPrediction
IonBuilder::typedObjectPrediction(MDefinition* typedObj)
{
    // Extract TypedObjectPrediction directly if we can
    if (typedObj->isNewDerivedTypedObject()) {
        return typedObj->toNewDerivedTypedObject()->prediction();
    }

    TemporaryTypeSet* types = typedObj->resultTypeSet();
    return typedObjectPrediction(types);
}

TypedObjectPrediction
IonBuilder::typedObjectPrediction(TemporaryTypeSet* types)
{
    // Type set must be known to be an object.
    if (!types || types->getKnownMIRType() != MIRType_Object)
        return TypedObjectPrediction();

    // And only known objects.
    if (types->unknownObject())
        return TypedObjectPrediction();

    TypedObjectPrediction out;
    for (uint32_t i = 0; i < types->getObjectCount(); i++) {
        ObjectGroup* group = types->getGroup(i);
        if (!group || !TypeSet::ObjectKey::get(group)->hasStableClassAndProto(constraints()))
            return TypedObjectPrediction();

        if (!IsTypedObjectClass(group->clasp()))
            return TypedObjectPrediction();

        out.addDescr(group->typeDescr());
    }

    return out;
}

MDefinition*
IonBuilder::loadTypedObjectType(MDefinition* typedObj)
{
    // Shortcircuit derived type objects, meaning the intermediate
    // objects created to represent `a.b` in an expression like
    // `a.b.c`. In that case, the type object can be simply pulled
    // from the operands of that instruction.
    if (typedObj->isNewDerivedTypedObject())
        return typedObj->toNewDerivedTypedObject()->type();

    MInstruction* descr = MTypedObjectDescr::New(alloc(), typedObj);
    current->add(descr);

    return descr;
}

// Given a typed object `typedObj` and an offset `offset` into that
// object's data, returns another typed object and adusted offset
// where the data can be found. Often, these returned values are the
// same as the inputs, but in cases where intermediate derived type
// objects have been created, the return values will remove
// intermediate layers (often rendering those derived type objects
// into dead code).
void
IonBuilder::loadTypedObjectData(MDefinition* typedObj,
                                MDefinition** owner,
                                LinearSum* ownerOffset)
{
    MOZ_ASSERT(typedObj->type() == MIRType_Object);

    // Shortcircuit derived type objects, meaning the intermediate
    // objects created to represent `a.b` in an expression like
    // `a.b.c`. In that case, the owned and a base offset can be
    // pulled from the operands of the instruction and combined with
    // `offset`.
    if (typedObj->isNewDerivedTypedObject()) {
        MNewDerivedTypedObject* ins = typedObj->toNewDerivedTypedObject();

        SimpleLinearSum base = ExtractLinearSum(ins->offset());
        if (!ownerOffset->add(base))
            setForceAbort();

        *owner = ins->owner();
        return;
    }

    *owner = typedObj;
}

// Takes as input a typed object, an offset into that typed object's
// memory, and the type repr of the data found at that offset. Returns
// the elements pointer and a scaled offset. The scaled offset is
// expressed in units of `unit`; when working with typed array MIR,
// this is typically the alignment.
void
IonBuilder::loadTypedObjectElements(MDefinition* typedObj,
                                    const LinearSum& baseByteOffset,
                                    int32_t scale,
                                    MDefinition** ownerElements,
                                    MDefinition** ownerScaledOffset,
                                    int32_t* ownerByteAdjustment)
{
    MDefinition* owner;
    LinearSum ownerByteOffset(alloc());
    loadTypedObjectData(typedObj, &owner, &ownerByteOffset);

    if (!ownerByteOffset.add(baseByteOffset))
        setForceAbort();

    TemporaryTypeSet* ownerTypes = owner->resultTypeSet();
    const Class* clasp = ownerTypes ? ownerTypes->getKnownClass(constraints()) : nullptr;
    if (clasp && IsInlineTypedObjectClass(clasp)) {
        // Perform the load directly from the owner pointer.
        if (!ownerByteOffset.add(InlineTypedObject::offsetOfDataStart()))
            setForceAbort();
        *ownerElements = owner;
    } else {
        bool definitelyOutline = clasp && IsOutlineTypedObjectClass(clasp);
        *ownerElements = MTypedObjectElements::New(alloc(), owner, definitelyOutline);
        current->add((*ownerElements)->toInstruction());
    }

    // Extract the constant adjustment from the byte offset.
    *ownerByteAdjustment = ownerByteOffset.constant();
    int32_t negativeAdjustment;
    if (!SafeSub(0, *ownerByteAdjustment, &negativeAdjustment))
        setForceAbort();
    if (!ownerByteOffset.add(negativeAdjustment))
        setForceAbort();

    // Scale the byte offset if required by the MIR node which will access the
    // typed object. In principle we should always be able to cleanly divide
    // the terms in this lienar sum due to alignment restrictions, but due to
    // limitations of ExtractLinearSum when applied to the terms in derived
    // typed objects this isn't always be possible. In these cases, fall back
    // on an explicit division operation.
    if (ownerByteOffset.divide(scale)) {
        *ownerScaledOffset = ConvertLinearSum(alloc(), current, ownerByteOffset);
    } else {
        MDefinition* unscaledOffset = ConvertLinearSum(alloc(), current, ownerByteOffset);
        *ownerScaledOffset = MDiv::NewAsmJS(alloc(), unscaledOffset, constantInt(scale),
                                            MIRType_Int32, /* unsigned = */ false);
        current->add((*ownerScaledOffset)->toInstruction());
    }
}

// Looks up the offset/type-repr-set of the field `id`, given the type
// set `objTypes` of the field owner. If a field is found, returns true
// and sets *fieldOffset, *fieldPrediction, and *fieldIndex. Returns false
// otherwise. Infallible.
bool
IonBuilder::typedObjectHasField(MDefinition* typedObj,
                                PropertyName* name,
                                size_t* fieldOffset,
                                TypedObjectPrediction* fieldPrediction,
                                size_t* fieldIndex)
{
    TypedObjectPrediction objPrediction = typedObjectPrediction(typedObj);
    if (objPrediction.isUseless()) {
        trackOptimizationOutcome(TrackedOutcome::AccessNotTypedObject);
        return false;
    }

    // Must be accessing a struct.
    if (objPrediction.kind() != type::Struct) {
        trackOptimizationOutcome(TrackedOutcome::NotStruct);
        return false;
    }

    // Determine the type/offset of the field `name`, if any.
    if (!objPrediction.hasFieldNamed(NameToId(name), fieldOffset,
                                     fieldPrediction, fieldIndex))
    {
        trackOptimizationOutcome(TrackedOutcome::StructNoField);
        return false;
    }

    return true;
}

MDefinition*
IonBuilder::typeObjectForElementFromArrayStructType(MDefinition* typeObj)
{
    MInstruction* elemType = MLoadFixedSlot::New(alloc(), typeObj, JS_DESCR_SLOT_ARRAY_ELEM_TYPE);
    current->add(elemType);

    MInstruction* unboxElemType = MUnbox::New(alloc(), elemType, MIRType_Object, MUnbox::Infallible);
    current->add(unboxElemType);

    return unboxElemType;
}

MDefinition*
IonBuilder::typeObjectForFieldFromStructType(MDefinition* typeObj,
                                             size_t fieldIndex)
{
    // Load list of field type objects.

    MInstruction* fieldTypes = MLoadFixedSlot::New(alloc(), typeObj, JS_DESCR_SLOT_STRUCT_FIELD_TYPES);
    current->add(fieldTypes);

    MInstruction* unboxFieldTypes = MUnbox::New(alloc(), fieldTypes, MIRType_Object, MUnbox::Infallible);
    current->add(unboxFieldTypes);

    // Index into list with index of field.

    MInstruction* fieldTypesElements = MElements::New(alloc(), unboxFieldTypes);
    current->add(fieldTypesElements);

    MConstant* fieldIndexDef = constantInt(fieldIndex);

    MInstruction* fieldType = MLoadElement::New(alloc(), fieldTypesElements, fieldIndexDef, false, false);
    current->add(fieldType);

    MInstruction* unboxFieldType = MUnbox::New(alloc(), fieldType, MIRType_Object, MUnbox::Infallible);
    current->add(unboxFieldType);

    return unboxFieldType;
}

bool
IonBuilder::storeScalarTypedObjectValue(MDefinition* typedObj,
                                        const LinearSum& byteOffset,
                                        ScalarTypeDescr::Type type,
                                        MDefinition* value)
{
    // Find location within the owner object.
    MDefinition* elements;
    MDefinition* scaledOffset;
    int32_t adjustment;
    size_t alignment = ScalarTypeDescr::alignment(type);
    loadTypedObjectElements(typedObj, byteOffset, alignment, &elements, &scaledOffset, &adjustment);

    // Clamp value to [0, 255] when type is Uint8Clamped
    MDefinition* toWrite = value;
    if (type == Scalar::Uint8Clamped) {
        toWrite = MClampToUint8::New(alloc(), value);
        current->add(toWrite->toInstruction());
    }

    MStoreUnboxedScalar* store =
        MStoreUnboxedScalar::New(alloc(), elements, scaledOffset, toWrite,
                                 type, MStoreUnboxedScalar::TruncateInput,
                                 DoesNotRequireMemoryBarrier, adjustment);
    current->add(store);

    return true;
}

bool
IonBuilder::storeReferenceTypedObjectValue(MDefinition* typedObj,
                                           const LinearSum& byteOffset,
                                           ReferenceTypeDescr::Type type,
                                           MDefinition* value,
                                           PropertyName* name)
{
    // Make sure we aren't adding new type information for writes of object and value
    // references.
    if (type != ReferenceTypeDescr::TYPE_STRING) {
        MOZ_ASSERT(type == ReferenceTypeDescr::TYPE_ANY ||
                   type == ReferenceTypeDescr::TYPE_OBJECT);
        MIRType implicitType =
            (type == ReferenceTypeDescr::TYPE_ANY) ? MIRType_Undefined : MIRType_Null;

        if (PropertyWriteNeedsTypeBarrier(alloc(), constraints(), current, &typedObj, name, &value,
                                          /* canModify = */ true, implicitType))
        {
            trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
            return false;
        }
    }

    // Find location within the owner object.
    MDefinition* elements;
    MDefinition* scaledOffset;
    int32_t adjustment;
    size_t alignment = ReferenceTypeDescr::alignment(type);
    loadTypedObjectElements(typedObj, byteOffset, alignment, &elements, &scaledOffset, &adjustment);

    MInstruction* store = nullptr;  // initialize to silence GCC warning
    switch (type) {
      case ReferenceTypeDescr::TYPE_ANY:
        if (NeedsPostBarrier(value))
            current->add(MPostWriteBarrier::New(alloc(), typedObj, value));
        store = MStoreElement::New(alloc(), elements, scaledOffset, value, false, adjustment);
        store->toStoreElement()->setNeedsBarrier();
        break;
      case ReferenceTypeDescr::TYPE_OBJECT:
        // Note: We cannot necessarily tell at this point whether a post
        // barrier is needed, because the type policy may insert ToObjectOrNull
        // instructions later, and those may require a post barrier. Therefore,
        // defer the insertion of post barriers to the type policy.
        store = MStoreUnboxedObjectOrNull::New(alloc(), elements, scaledOffset, value, typedObj, adjustment);
        break;
      case ReferenceTypeDescr::TYPE_STRING:
        // Strings are not nursery allocated, so these writes do not need post
        // barriers.
        store = MStoreUnboxedString::New(alloc(), elements, scaledOffset, value, adjustment);
        break;
    }

    current->add(store);
    return true;
}

JSObject*
IonBuilder::checkNurseryObject(JSObject* obj)
{
    // If we try to use any nursery pointers during compilation, make sure that
    // the main thread will cancel this compilation before performing a minor
    // GC. All constants used during compilation should either go through this
    // function or should come from a type set (which has a similar barrier).
    if (obj && IsInsideNursery(obj)) {
        compartment->runtime()->setMinorGCShouldCancelIonCompilations();
        IonBuilder* builder = this;
        while (builder) {
            builder->setNotSafeForMinorGC();
            builder = builder->callerBuilder_;
        }
    }

    return obj;
}

MConstant*
IonBuilder::constant(const Value& v)
{
    MOZ_ASSERT(!v.isString() || v.toString()->isAtom(),
               "Handle non-atomized strings outside IonBuilder.");

    if (v.isObject())
        checkNurseryObject(&v.toObject());

    MConstant* c = MConstant::New(alloc(), v, constraints());
    current->add(c);
    return c;
}

MConstant*
IonBuilder::constantInt(int32_t i)
{
    return constant(Int32Value(i));
}

MInstruction*
IonBuilder::initializedLength(MDefinition* obj, MDefinition* elements, JSValueType unboxedType)
{
    MInstruction* res;
    if (unboxedType != JSVAL_TYPE_MAGIC)
        res = MUnboxedArrayInitializedLength::New(alloc(), obj);
    else
        res = MInitializedLength::New(alloc(), elements);
    current->add(res);
    return res;
}

MInstruction*
IonBuilder::setInitializedLength(MDefinition* obj, JSValueType unboxedType, size_t count)
{
    MOZ_ASSERT(count);

    MInstruction* res;
    if (unboxedType != JSVAL_TYPE_MAGIC) {
        res = MSetUnboxedArrayInitializedLength::New(alloc(), obj, constant(Int32Value(count)));
    } else {
        // MSetInitializedLength takes the index of the last element, rather
        // than the count itself.
        MInstruction* elements = MElements::New(alloc(), obj, /* unboxed = */ false);
        current->add(elements);
        res = MSetInitializedLength::New(alloc(), elements, constant(Int32Value(count - 1)));
    }
    current->add(res);
    return res;
}

MDefinition*
IonBuilder::getCallee()
{
    if (inliningDepth_ == 0) {
        MInstruction* callee = MCallee::New(alloc());
        current->add(callee);
        return callee;
    }

    return inlineCallInfo_->fun();
}

MDefinition*
IonBuilder::addLexicalCheck(MDefinition* input)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_CHECKLEXICAL ||
               JSOp(*pc) == JSOP_CHECKALIASEDLEXICAL ||
               JSOp(*pc) == JSOP_GETIMPORT);

    MInstruction* lexicalCheck;

    // If we're guaranteed to not be JS_UNINITIALIZED_LEXICAL, no need to check.
    if (input->type() == MIRType_MagicUninitializedLexical) {
        // Mark the input as implicitly used so the JS_UNINITIALIZED_LEXICAL
        // magic value will be preserved on bailout.
        input->setImplicitlyUsedUnchecked();
        lexicalCheck = MThrowRuntimeLexicalError::New(alloc(), JSMSG_UNINITIALIZED_LEXICAL);
        current->add(lexicalCheck);
        if (!resumeAfter(lexicalCheck))
            return nullptr;
        return constant(UndefinedValue());
    }

    if (input->type() == MIRType_Value) {
        lexicalCheck = MLexicalCheck::New(alloc(), input);
        current->add(lexicalCheck);
        if (failedLexicalCheck_)
            lexicalCheck->setNotMovableUnchecked();
        return lexicalCheck;
    }

    return input;
}
