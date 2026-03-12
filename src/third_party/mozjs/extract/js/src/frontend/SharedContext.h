/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SharedContext_h
#define frontend_SharedContext_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ParserAtom.h"          // TaggedParserAtomIndex
#include "frontend/ScopeIndex.h"          // ScopeIndex
#include "frontend/ScriptIndex.h"         // ScriptIndex
#include "js/ColumnNumber.h"              // JS::LimitedColumnNumberOneOrigin
#include "vm/FunctionFlags.h"             // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/Scope.h"
#include "vm/ScopeKind.h"
#include "vm/SharedStencil.h"
#include "vm/StencilEnums.h"

namespace JS {
class JS_PUBLIC_API ReadOnlyCompileOptions;
struct WasmModule;
}  // namespace JS

namespace js {

class FrontendContext;

namespace frontend {

struct CompilationState;
class FunctionBox;
class FunctionNode;
class ParseContext;
class ScriptStencil;
class ScriptStencilExtra;
struct ScopeContext;

enum class StatementKind : uint8_t {
  Label,
  Block,
  If,
  Switch,
  With,
  Catch,
  Try,
  Finally,
  ForLoopLexicalHead,
  ForLoop,
  ForInLoop,
  ForOfLoop,
  DoLoop,
  WhileLoop,
  Class,

  // Used only by BytecodeEmitter.
  Spread,
  YieldStar,
};

static inline bool StatementKindIsLoop(StatementKind kind) {
  return kind == StatementKind::ForLoop || kind == StatementKind::ForInLoop ||
         kind == StatementKind::ForOfLoop || kind == StatementKind::DoLoop ||
         kind == StatementKind::WhileLoop || kind == StatementKind::Spread ||
         kind == StatementKind::YieldStar;
}

static inline bool StatementKindIsUnlabeledBreakTarget(StatementKind kind) {
  return StatementKindIsLoop(kind) || kind == StatementKind::Switch;
}

// List of directives that may be encountered in a Directive Prologue
// (ES5 15.1).
class Directives {
  bool strict_;
  bool asmJS_;

 public:
  explicit Directives(bool strict) : strict_(strict), asmJS_(false) {}
  explicit Directives(ParseContext* parent);

  void setStrict() { strict_ = true; }
  bool strict() const { return strict_; }

  void setAsmJS() { asmJS_ = true; }
  bool asmJS() const { return asmJS_; }

  Directives& operator=(Directives rhs) {
    strict_ = rhs.strict_;
    asmJS_ = rhs.asmJS_;
    return *this;
  }
  bool operator==(const Directives& rhs) const {
    return strict_ == rhs.strict_ && asmJS_ == rhs.asmJS_;
  }
  bool operator!=(const Directives& rhs) const { return !(*this == rhs); }
};

// The kind of this-binding for the current scope. Note that arrow functions
// have a lexical this-binding so their ThisBinding is the same as the
// ThisBinding of their enclosing scope and can be any value. Derived
// constructors require TDZ checks when accessing the binding.
enum class ThisBinding : uint8_t {
  Global,
  Module,
  Function,
  DerivedConstructor
};

// If Yes, the script inherits it's "this" environment and binding from the
// enclosing script. This is true for arrow-functions and eval scripts.
enum class InheritThis { No, Yes };

class GlobalSharedContext;
class EvalSharedContext;
class ModuleSharedContext;
class SuspendableContext;

#define IMMUTABLE_FLAG_GETTER_SETTER(lowerName, name) \
  GENERIC_FLAG_GETTER_SETTER(ImmutableFlags, lowerName, name)

#define IMMUTABLE_FLAG_GETTER(lowerName, name) \
  GENERIC_FLAG_GETTER(ImmutableFlags, lowerName, name)

/*
 * The struct SharedContext is part of the current parser context (see
 * ParseContext). It stores information that is reused between the parser and
 * the bytecode emitter.
 */
class SharedContext {
 public:
  FrontendContext* const fc_;

 protected:
  // See: BaseScript::immutableFlags_
  ImmutableScriptFlags immutableFlags_ = {};

  // The location of this script in the source. Note that the value here differs
  // from the final BaseScript for the case of standalone functions.
  // This field is copied to ScriptStencil, and shouldn't be modified after the
  // copy.
  SourceExtent extent_ = {};

 protected:
  // See: ThisBinding
  ThisBinding thisBinding_ = ThisBinding::Global;

  // These flags do not have corresponding script flags and may be inherited
  // from the scope chain in the case of eval and arrows.
  bool allowNewTarget_ : 1;
  bool allowSuperProperty_ : 1;
  bool allowSuperCall_ : 1;
  bool allowArguments_ : 1;
  bool inWith_ : 1;
  bool inClass_ : 1;

  // See `strict()` below.
  bool localStrict : 1;

  // True if "use strict"; appears in the body instead of being inherited.
  bool hasExplicitUseStrict_ : 1;

  // Tracks if script-related fields are already copied to ScriptStencilExtra.
  //
  // If this field is true, those fileds shouldn't be modified.
  //
  // For FunctionBox, some fields are allowed to be modified, but the
  // modification should be synced with ScriptStencilExtra by
  // FunctionBox::copyUpdated* methods.
  bool isScriptExtraFieldCopiedToStencil : 1;

  // Indicates this shared context is eligible to use JSOp::ArgumentsLength
  // when emitting the ArgumentsLength parse node.
  bool eligibleForArgumentsLength : 1;

  // End of fields.

  enum class Kind : uint8_t { FunctionBox, Global, Eval, Module };

  // Alias enum into SharedContext
  using ImmutableFlags = ImmutableScriptFlagsEnum;

  [[nodiscard]] bool hasFlag(ImmutableFlags flag) const {
    return immutableFlags_.hasFlag(flag);
  }
  void setFlag(ImmutableFlags flag, bool b = true) {
    MOZ_ASSERT(!isScriptExtraFieldCopiedToStencil);
    immutableFlags_.setFlag(flag, b);
  }
  void clearFlag(ImmutableFlags flag) {
    MOZ_ASSERT(!isScriptExtraFieldCopiedToStencil);
    immutableFlags_.clearFlag(flag);
  }

 public:
  SharedContext(FrontendContext* fc, Kind kind,
                const JS::ReadOnlyCompileOptions& options,
                Directives directives, SourceExtent extent);

  IMMUTABLE_FLAG_GETTER_SETTER(isForEval, IsForEval)
  IMMUTABLE_FLAG_GETTER_SETTER(isModule, IsModule)
  IMMUTABLE_FLAG_GETTER_SETTER(isFunction, IsFunction)
  IMMUTABLE_FLAG_GETTER_SETTER(selfHosted, SelfHosted)
  IMMUTABLE_FLAG_GETTER_SETTER(forceStrict, ForceStrict)
  IMMUTABLE_FLAG_GETTER_SETTER(hasNonSyntacticScope, HasNonSyntacticScope)
  IMMUTABLE_FLAG_GETTER_SETTER(noScriptRval, NoScriptRval)
  IMMUTABLE_FLAG_GETTER(treatAsRunOnce, TreatAsRunOnce)
  // Strict: custom logic below
  IMMUTABLE_FLAG_GETTER_SETTER(hasModuleGoal, HasModuleGoal)
  IMMUTABLE_FLAG_GETTER_SETTER(hasInnerFunctions, HasInnerFunctions)
  IMMUTABLE_FLAG_GETTER_SETTER(hasDirectEval, HasDirectEval)
  IMMUTABLE_FLAG_GETTER_SETTER(bindingsAccessedDynamically,
                               BindingsAccessedDynamically)
  IMMUTABLE_FLAG_GETTER_SETTER(hasCallSiteObj, HasCallSiteObj)

  const SourceExtent& extent() const { return extent_; }

  bool isFunctionBox() const { return isFunction(); }
  inline FunctionBox* asFunctionBox();
  bool isModuleContext() const { return isModule(); }
  inline ModuleSharedContext* asModuleContext();
  bool isSuspendableContext() const { return isFunction() || isModule(); }
  inline SuspendableContext* asSuspendableContext();
  bool isGlobalContext() const {
    return !(isFunction() || isModule() || isForEval());
  }
  inline GlobalSharedContext* asGlobalContext();
  bool isEvalContext() const { return isForEval(); }
  inline EvalSharedContext* asEvalContext();

  bool isTopLevelContext() const { return !isFunction(); }

  ThisBinding thisBinding() const { return thisBinding_; }
  bool hasFunctionThisBinding() const {
    return thisBinding() == ThisBinding::Function ||
           thisBinding() == ThisBinding::DerivedConstructor;
  }
  bool needsThisTDZChecks() const {
    return thisBinding() == ThisBinding::DerivedConstructor;
  }

  bool isSelfHosted() const { return selfHosted(); }
  bool allowNewTarget() const { return allowNewTarget_; }
  bool allowSuperProperty() const { return allowSuperProperty_; }
  bool allowSuperCall() const { return allowSuperCall_; }
  bool allowArguments() const { return allowArguments_; }
  bool inWith() const { return inWith_; }
  bool inClass() const { return inClass_; }

  bool hasExplicitUseStrict() const { return hasExplicitUseStrict_; }
  void setExplicitUseStrict() { hasExplicitUseStrict_ = true; }

  ImmutableScriptFlags immutableFlags() const { return immutableFlags_; }

  bool allBindingsClosedOver() const { return bindingsAccessedDynamically(); }

  // The ImmutableFlag tracks if the entire script is strict, while the
  // localStrict flag indicates the current region (such as class body) should
  // be treated as strict. The localStrict flag will always be reset to false
  // before the end of the script.
  bool strict() const { return hasFlag(ImmutableFlags::Strict) || localStrict; }
  void setStrictScript() { setFlag(ImmutableFlags::Strict); }
  bool setLocalStrictMode(bool strict) {
    bool retVal = localStrict;
    localStrict = strict;
    return retVal;
  }

  bool isEligibleForArgumentsLength() const {
    return eligibleForArgumentsLength && !bindingsAccessedDynamically();
  }
  void setIneligibleForArgumentsLength() { eligibleForArgumentsLength = false; }

  void copyScriptExtraFields(ScriptStencilExtra& scriptExtra);
};

class MOZ_STACK_CLASS GlobalSharedContext : public SharedContext {
  ScopeKind scopeKind_;

 public:
  GlobalScope::ParserData* bindings;

  GlobalSharedContext(FrontendContext* fc, ScopeKind scopeKind,
                      const JS::ReadOnlyCompileOptions& options,
                      Directives directives, SourceExtent extent);

  ScopeKind scopeKind() const { return scopeKind_; }
};

inline GlobalSharedContext* SharedContext::asGlobalContext() {
  MOZ_ASSERT(isGlobalContext());
  return static_cast<GlobalSharedContext*>(this);
}

class MOZ_STACK_CLASS EvalSharedContext : public SharedContext {
 public:
  EvalScope::ParserData* bindings;

  EvalSharedContext(FrontendContext* fc, CompilationState& compilationState,
                    SourceExtent extent);
};

inline EvalSharedContext* SharedContext::asEvalContext() {
  MOZ_ASSERT(isEvalContext());
  return static_cast<EvalSharedContext*>(this);
}

enum class HasHeritage { No, Yes };

class SuspendableContext : public SharedContext {
 public:
  SuspendableContext(FrontendContext* fc, Kind kind,
                     const JS::ReadOnlyCompileOptions& options,
                     Directives directives, SourceExtent extent,
                     bool isGenerator, bool isAsync);

  IMMUTABLE_FLAG_GETTER_SETTER(isAsync, IsAsync)
  IMMUTABLE_FLAG_GETTER_SETTER(isGenerator, IsGenerator)

  bool needsFinalYield() const { return isGenerator() || isAsync(); }
  bool needsDotGeneratorName() const { return isGenerator() || isAsync(); }
  bool needsClearSlotsOnExit() const { return isGenerator() || isAsync(); }
  bool needsIteratorResult() const { return isGenerator() && !isAsync(); }
  bool needsPromiseResult() const { return isAsync() && !isGenerator(); }
};

class FunctionBox : public SuspendableContext {
  friend struct GCThingList;

  CompilationState& compilationState_;

  // If this FunctionBox refers to a lazy child of the function being
  // compiled, this field holds the child's immediately enclosing scope's index.
  // Once compilation succeeds, we will store the scope pointed by this in the
  // child's BaseScript.  (Debugger may become confused if lazy scripts refer to
  // partially initialized enclosing scopes, so we must avoid storing the
  // scope in the BaseScript until compilation has completed
  // successfully.)
  // This is copied to ScriptStencil.
  // Any update after the copy should be synced to the ScriptStencil.
  mozilla::Maybe<ScopeIndex> enclosingScopeIndex_;

  // Names from the named lambda scope, if a named lambda.
  LexicalScope::ParserData* namedLambdaBindings_ = nullptr;

  // Names from the function scope.
  FunctionScope::ParserData* functionScopeBindings_ = nullptr;

  // Names from the extra 'var' scope of the function, if the parameter list
  // has expressions.
  VarScope::ParserData* extraVarScopeBindings_ = nullptr;

  // The explicit or implicit name of the function. The FunctionFlags indicate
  // the kind of name.
  // This is copied to ScriptStencil.
  // Any update after the copy should be synced to the ScriptStencil.
  TaggedParserAtomIndex atom_;

  // Index into CompilationStencil::scriptData.
  ScriptIndex funcDataIndex_ = ScriptIndex(-1);

  // See: FunctionFlags
  // This is copied to ScriptStencil.
  // Any update after the copy should be synced to the ScriptStencil.
  FunctionFlags flags_ = {};

  // See: ImmutableScriptData::funLength
  uint16_t length_ = 0;

  // JSFunction::nargs_
  // This field is copied to ScriptStencil, and shouldn't be modified after the
  // copy.
  uint16_t nargs_ = 0;

  // See: PrivateScriptData::memberInitializers_
  // This field is copied to ScriptStencil, and shouldn't be modified after the
  // copy.
  MemberInitializers memberInitializers_ = MemberInitializers::Invalid();

 public:
  // Back pointer used by asm.js for error messages.
  FunctionNode* functionNode = nullptr;

  // True if bytecode will be emitted for this function in the current
  // compilation.
  bool emitBytecode : 1;

  // This is set by the BytecodeEmitter of the enclosing script when a reference
  // to this function is generated. This is also used to determine a hoisted
  // function already is referenced by the bytecode.
  bool wasEmittedByEnclosingScript_ : 1;

  // Need to emit a synthesized Annex B assignment
  bool isAnnexB : 1;

  // Track if we saw "use asm".
  // If we successfully validated it, `flags_` is seto to `AsmJS` kind.
  bool useAsm : 1;

  // Analysis of parameter list
  bool hasParameterExprs : 1;
  bool hasDestructuringArgs : 1;
  bool hasDuplicateParameters : 1;

  // Arrow function with expression body like: `() => 1`.
  bool hasExprBody_ : 1;

  // Used to issue an early error in static class blocks.
  bool allowReturn_ : 1;

  // Tracks if function-related fields are already copied to ScriptStencil.
  // If this field is true, modification to those fields should be synced with
  // ScriptStencil by copyUpdated* methods.
  bool isFunctionFieldCopiedToStencil : 1;

  // True if this is part of initial compilation.
  // False if this is part of delazification.
  bool isInitialCompilation : 1;

  // True if this is standalone function
  // (new Function() including generator/async, or event handler).
  bool isStandalone : 1;

  // End of fields.

  FunctionBox(FrontendContext* fc, SourceExtent extent,
              CompilationState& compilationState, Directives directives,
              GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
              bool isInitialCompilation, TaggedParserAtomIndex atom,
              FunctionFlags flags, ScriptIndex index);

  ScriptStencil& functionStencil() const;
  ScriptStencilExtra& functionExtraStencil() const;

  LexicalScope::ParserData* namedLambdaBindings() {
    return namedLambdaBindings_;
  }
  void setNamedLambdaBindings(LexicalScope::ParserData* bindings) {
    namedLambdaBindings_ = bindings;
  }

  FunctionScope::ParserData* functionScopeBindings() {
    return functionScopeBindings_;
  }
  void setFunctionScopeBindings(FunctionScope::ParserData* bindings) {
    functionScopeBindings_ = bindings;
  }

  VarScope::ParserData* extraVarScopeBindings() {
    return extraVarScopeBindings_;
  }
  void setExtraVarScopeBindings(VarScope::ParserData* bindings) {
    extraVarScopeBindings_ = bindings;
  }

  void initFromLazyFunction(const ScriptStencilExtra& extra,
                            ScopeContext& scopeContext,
                            FunctionSyntaxKind kind);
  void initFromScriptStencilExtra(const ScriptStencilExtra& extra);
  void initStandalone(ScopeContext& scopeContext, FunctionSyntaxKind kind);

 private:
  void initStandaloneOrLazy(ScopeContext& scopeContext,
                            FunctionSyntaxKind kind);

 public:
  void initWithEnclosingParseContext(ParseContext* enclosing,
                                     FunctionSyntaxKind kind);

  void setEnclosingScopeForInnerLazyFunction(ScopeIndex scopeIndex);

  bool wasEmittedByEnclosingScript() const {
    return wasEmittedByEnclosingScript_;
  }
  void setWasEmittedByEnclosingScript(bool wasEmitted) {
    wasEmittedByEnclosingScript_ = wasEmitted;
    if (isFunctionFieldCopiedToStencil) {
      copyUpdatedWasEmitted();
    }
  }

  [[nodiscard]] bool setAsmJSModule(const JS::WasmModule* module);
  bool isAsmJSModule() const { return flags_.isAsmJSNative(); }

  bool hasEnclosingScopeIndex() const { return enclosingScopeIndex_.isSome(); }
  ScopeIndex getEnclosingScopeIndex() const { return *enclosingScopeIndex_; }

  IMMUTABLE_FLAG_GETTER_SETTER(isAsync, IsAsync)
  IMMUTABLE_FLAG_GETTER_SETTER(isGenerator, IsGenerator)
  IMMUTABLE_FLAG_GETTER_SETTER(funHasExtensibleScope, FunHasExtensibleScope)
  IMMUTABLE_FLAG_GETTER_SETTER(functionHasThisBinding, FunctionHasThisBinding)
  IMMUTABLE_FLAG_GETTER_SETTER(functionHasNewTargetBinding,
                               FunctionHasNewTargetBinding)
  // NeedsHomeObject: custom logic below.
  // IsDerivedClassConstructor: custom logic below.
  // IsFieldInitializer: custom logic below.
  IMMUTABLE_FLAG_GETTER(useMemberInitializers, UseMemberInitializers)
  IMMUTABLE_FLAG_GETTER_SETTER(hasRest, HasRest)
  IMMUTABLE_FLAG_GETTER_SETTER(needsFunctionEnvironmentObjects,
                               NeedsFunctionEnvironmentObjects)
  IMMUTABLE_FLAG_GETTER_SETTER(functionHasExtraBodyVarScope,
                               FunctionHasExtraBodyVarScope)
  IMMUTABLE_FLAG_GETTER_SETTER(shouldDeclareArguments, ShouldDeclareArguments)
  IMMUTABLE_FLAG_GETTER_SETTER(needsArgsObj, NeedsArgsObj)
  // HasMappedArgsObj: custom logic below.

  bool needsCallObjectRegardlessOfBindings() const {
    // Always create a CallObject if:
    // - The scope is extensible at runtime due to sloppy eval.
    // - The function is a generator or async function. (The debugger reads the
    //   generator object directly from the frame.)

    return funHasExtensibleScope() || isGenerator() || isAsync();
  }

  bool needsExtraBodyVarEnvironmentRegardlessOfBindings() const {
    MOZ_ASSERT(hasParameterExprs);
    return funHasExtensibleScope();
  }

  GeneratorKind generatorKind() const {
    return isGenerator() ? GeneratorKind::Generator
                         : GeneratorKind::NotGenerator;
  }

  FunctionAsyncKind asyncKind() const {
    return isAsync() ? FunctionAsyncKind::AsyncFunction
                     : FunctionAsyncKind::SyncFunction;
  }

  bool needsFinalYield() const { return isGenerator() || isAsync(); }
  bool needsDotGeneratorName() const { return isGenerator() || isAsync(); }
  bool needsClearSlotsOnExit() const { return isGenerator() || isAsync(); }
  bool needsIteratorResult() const { return isGenerator() && !isAsync(); }
  bool needsPromiseResult() const { return isAsync() && !isGenerator(); }

  bool isArrow() const { return flags_.isArrow(); }
  bool isLambda() const { return flags_.isLambda(); }

  bool hasExprBody() const { return hasExprBody_; }
  void setHasExprBody() {
    MOZ_ASSERT(isArrow());
    hasExprBody_ = true;
  }

  bool allowReturn() const { return allowReturn_; }

  bool isNamedLambda() const { return flags_.isNamedLambda(!!explicitName()); }
  bool isGetter() const { return flags_.isGetter(); }
  bool isSetter() const { return flags_.isSetter(); }
  bool isMethod() const { return flags_.isMethod(); }
  bool isClassConstructor() const { return flags_.isClassConstructor(); }

  bool isInterpreted() const { return flags_.hasBaseScript(); }

  FunctionFlags::FunctionKind kind() const { return flags_.kind(); }

  bool hasInferredName() const { return flags_.hasInferredName(); }
  bool hasGuessedAtom() const { return flags_.hasGuessedAtom(); }

  TaggedParserAtomIndex displayAtom() const { return atom_; }
  TaggedParserAtomIndex explicitName() const {
    return (hasInferredName() || hasGuessedAtom())
               ? TaggedParserAtomIndex::null()
               : atom_;
  }

  // NOTE: We propagate to any existing functions for now. This handles both the
  // delazification case where functions already exist, and also handles
  // code-coverage which is not yet deferred.
  void setInferredName(TaggedParserAtomIndex atom) {
    atom_ = atom;
    flags_.setInferredName();
    if (isFunctionFieldCopiedToStencil) {
      copyUpdatedAtomAndFlags();
    }
  }
  void setGuessedAtom(TaggedParserAtomIndex atom) {
    atom_ = atom;
    flags_.setGuessedAtom();
    if (isFunctionFieldCopiedToStencil) {
      copyUpdatedAtomAndFlags();
    }
  }

  bool needsHomeObject() const {
    return hasFlag(ImmutableFlags::NeedsHomeObject);
  }
  void setNeedsHomeObject() {
    MOZ_ASSERT(flags_.allowSuperProperty());
    setFlag(ImmutableFlags::NeedsHomeObject);
    flags_.setIsExtended();
  }

  bool isDerivedClassConstructor() const {
    return hasFlag(ImmutableFlags::IsDerivedClassConstructor);
  }
  void setDerivedClassConstructor() {
    MOZ_ASSERT(flags_.isClassConstructor());
    setFlag(ImmutableFlags::IsDerivedClassConstructor);
  }

  bool isSyntheticFunction() const {
    return hasFlag(ImmutableFlags::IsSyntheticFunction);
  }
  void setSyntheticFunction() {
    // Field initializer, class constructor or getter or setter
    // synthesized from accessor keyword.
    MOZ_ASSERT(flags_.isMethod() || flags_.isGetter() || flags_.isSetter());
    setFlag(ImmutableFlags::IsSyntheticFunction);
  }

  bool hasSimpleParameterList() const {
    return !hasRest() && !hasParameterExprs && !hasDestructuringArgs;
  }

  bool hasMappedArgsObj() const {
    return !strict() && hasSimpleParameterList();
  }

  // Return whether this or an enclosing function is being parsed and
  // validated as asm.js. Note: if asm.js validation fails, this will be false
  // while the function is being reparsed. This flag can be used to disable
  // certain parsing features that are necessary in general, but unnecessary
  // for validated asm.js.
  bool useAsmOrInsideUseAsm() const { return useAsm; }

  void setStart(uint32_t offset, uint32_t line,
                JS::LimitedColumnNumberOneOrigin column) {
    MOZ_ASSERT(!isScriptExtraFieldCopiedToStencil);
    extent_.sourceStart = offset;
    extent_.lineno = line;
    extent_.column = column;
  }

  void setEnd(uint32_t end) {
    MOZ_ASSERT(!isScriptExtraFieldCopiedToStencil);
    // For all functions except class constructors, the buffer and
    // toString ending positions are the same. Class constructors override
    // the toString ending position with the end of the class definition.
    extent_.sourceEnd = end;
    extent_.toStringEnd = end;
  }

  void setCtorToStringEnd(uint32_t end) {
    extent_.toStringEnd = end;
    if (isScriptExtraFieldCopiedToStencil) {
      copyUpdatedExtent();
    }
  }

  void setCtorFunctionHasThisBinding() {
    immutableFlags_.setFlag(ImmutableFlags::FunctionHasThisBinding, true);
    if (isScriptExtraFieldCopiedToStencil) {
      copyUpdatedImmutableFlags();
    }
  }

  void setIsInlinableLargeFunction() {
    immutableFlags_.setFlag(ImmutableFlags::IsInlinableLargeFunction, true);
    if (isScriptExtraFieldCopiedToStencil) {
      copyUpdatedImmutableFlags();
    }
  }

  void setUsesArgumentsIntrinsics() {
    immutableFlags_.setFlag(ImmutableFlags::UsesArgumentsIntrinsics, true);
    if (isScriptExtraFieldCopiedToStencil) {
      copyUpdatedImmutableFlags();
    }
  }

  uint16_t length() const { return length_; }
  void setLength(uint16_t length) { length_ = length; }

  void setArgCount(uint16_t args) {
    MOZ_ASSERT(!isFunctionFieldCopiedToStencil);
    nargs_ = args;
  }

  size_t nargs() const { return nargs_; }

  const MemberInitializers& memberInitializers() const {
    MOZ_ASSERT(useMemberInitializers());
    return memberInitializers_;
  }
  void setMemberInitializers(MemberInitializers memberInitializers) {
    immutableFlags_.setFlag(ImmutableFlags::UseMemberInitializers, true);
    memberInitializers_ = memberInitializers;
    if (isScriptExtraFieldCopiedToStencil) {
      copyUpdatedImmutableFlags();
      copyUpdatedMemberInitializers();
    }
  }

  ScriptIndex index() const { return funcDataIndex_; }

  void finishScriptFlags();
  void copyFunctionFields(ScriptStencil& script);
  void copyFunctionExtraFields(ScriptStencilExtra& scriptExtra);

  // * setCtorFunctionHasThisBinding can be called to a class constructor
  //   with a lazy function, while parsing enclosing class
  // * setIsInlinableLargeFunction can be called by BCE to update flags of the
  //   previous top-level function, but only in self-hosted mode.
  void copyUpdatedImmutableFlags();

  // * setCtorToStringEnd bcan be called to a class constructor with a lazy
  //   function, while parsing enclosing class
  void copyUpdatedExtent();

  // * setMemberInitializers can be called to a class constructor with a lazy
  //   function, while emitting enclosing script
  void copyUpdatedMemberInitializers();

  // * setEnclosingScopeForInnerLazyFunction can be called to a lazy function,
  //   while emitting enclosing script
  void copyUpdatedEnclosingScopeIndex();

  // * setInferredName can be called to a lazy function, while emitting
  //   enclosing script
  // * setGuessedAtom can be called to both lazy/non-lazy functions,
  //   while running NameFunctions
  void copyUpdatedAtomAndFlags();

  // * setWasEmitted can be called to a lazy function, while emitting
  //   enclosing script
  void copyUpdatedWasEmitted();
};

#undef FLAG_GETTER_SETTER
#undef IMMUTABLE_FLAG_GETTER_SETTER

inline FunctionBox* SharedContext::asFunctionBox() {
  MOZ_ASSERT(isFunctionBox());
  return static_cast<FunctionBox*>(this);
}

inline SuspendableContext* SharedContext::asSuspendableContext() {
  MOZ_ASSERT(isSuspendableContext());
  return static_cast<SuspendableContext*>(this);
}

}  // namespace frontend
}  // namespace js

#endif /* frontend_SharedContext_h */
