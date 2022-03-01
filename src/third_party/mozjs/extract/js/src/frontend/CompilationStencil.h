/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_CompilationStencil_h
#define frontend_CompilationStencil_h

#include "mozilla/AlreadyAddRefed.h"  // already_AddRefed
#include "mozilla/Assertions.h"       // MOZ_ASSERT
#include "mozilla/Atomics.h"          // mozilla::Atomic
#include "mozilla/Attributes.h"       // MOZ_RAII
#include "mozilla/HashTable.h"        // mozilla::HashMap
#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf
#include "mozilla/RefPtr.h"           // RefPtr
#include "mozilla/Span.h"

#include "builtin/ModuleObject.h"
#include "ds/LifoAlloc.h"
#include "frontend/NameAnalysisTypes.h"  // EnvironmentCoordinate
#include "frontend/ParserAtom.h"   // ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/ScriptIndex.h"  // ScriptIndex
#include "frontend/SharedContext.h"
#include "frontend/Stencil.h"
#include "frontend/TaggedParserAtomIndexHasher.h"  // TaggedParserAtomIndexHasher
#include "frontend/UsedNameTracker.h"
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/RealmOptions.h"
#include "js/RefCounted.h"  // AtomicRefCounted
#include "js/SourceText.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"  // js::UniquePtr
#include "js/Vector.h"
#include "js/WasmModule.h"
#include "vm/GlobalObject.h"  // GlobalObject
#include "vm/JSContext.h"
#include "vm/JSFunction.h"  // JSFunction
#include "vm/JSScript.h"    // SourceExtent
#include "vm/Realm.h"
#include "vm/ScopeKind.h"      // ScopeKind
#include "vm/SharedStencil.h"  // SharedImmutableScriptData

namespace js {

class JSONPrinter;

namespace frontend {

struct CompilationInput;
struct CompilationStencil;
struct CompilationGCOutput;
class ScriptStencilIterable;

// ScopeContext holds information derived from the scope and environment chains
// to try to avoid the parser needing to traverse VM structures directly.
struct ScopeContext {
  // Class field initializer info if we are nested within a class constructor.
  // We may be an combination of arrow and eval context within the constructor.
  mozilla::Maybe<MemberInitializers> memberInitializers = {};

  enum class EnclosingLexicalBindingKind {
    Let,
    Const,
    CatchParameter,
    Synthetic,
    PrivateMethod,
  };

  using EnclosingLexicalBindingCache =
      mozilla::HashMap<TaggedParserAtomIndex, EnclosingLexicalBindingKind,
                       TaggedParserAtomIndexHasher>;

  // Cache of enclosing lexical bindings.
  // Used only for eval.
  mozilla::Maybe<EnclosingLexicalBindingCache> enclosingLexicalBindingCache_;

  // A map of private names to NameLocations used to allow evals to
  // provide correct private name semantics (particularly around early
  // errors and private brand lookup).
  using EffectiveScopePrivateFieldCache =
      mozilla::HashMap<TaggedParserAtomIndex, NameLocation,
                       TaggedParserAtomIndexHasher>;

  // Cache of enclosing class's private fields.
  // Used only for eval.
  mozilla::Maybe<EffectiveScopePrivateFieldCache>
      effectiveScopePrivateFieldCache_;

#ifdef DEBUG
  bool enclosingEnvironmentIsDebugProxy_ = false;
#endif

  // How many hops required to navigate from 'enclosingScope' to effective
  // scope.
  uint32_t effectiveScopeHops = 0;

  uint32_t enclosingScopeEnvironmentChainLength = 0;

  // Eval and arrow scripts also inherit the "this" environment -- used by
  // `super` expressions -- from their enclosing script. We count the number of
  // environment hops needed to get from enclosing scope to the nearest
  // appropriate environment. This value is undefined if the script we are
  // compiling is not an eval or arrow-function.
  uint32_t enclosingThisEnvironmentHops = 0;

  // The kind of enclosing scope.
  ScopeKind enclosingScopeKind = ScopeKind::Global;

  // The type of binding required for `this` of the top level context, as
  // indicated by the enclosing scopes of this parse.
  //
  // NOTE: This is computed based on the effective scope (defined above).
  ThisBinding thisBinding = ThisBinding::Global;

  // Eval and arrow scripts inherit certain syntax allowances from their
  // enclosing scripts.
  bool allowNewTarget = false;
  bool allowSuperProperty = false;
  bool allowSuperCall = false;
  bool allowArguments = true;

  // Indicates there is a 'class' or 'with' scope on enclosing scope chain.
  bool inClass = false;
  bool inWith = false;

  // True if the enclosing scope is for FunctionScope of arrow function.
  bool enclosingScopeIsArrow = false;

  // True if the enclosing scope has environment.
  bool enclosingScopeHasEnvironment = false;

#ifdef DEBUG
  // True if the enclosing scope has non-syntactic scope on chain.
  bool hasNonSyntacticScopeOnChain = false;

  // True if the enclosing scope has function scope where the function needs
  // home object.
  bool hasFunctionNeedsHomeObjectOnChain = false;
#endif

  bool init(JSContext* cx, CompilationInput& input,
            ParserAtomsTable& parserAtoms, InheritThis inheritThis,
            JSObject* enclosingEnv);

  mozilla::Maybe<EnclosingLexicalBindingKind>
  lookupLexicalBindingInEnclosingScope(TaggedParserAtomIndex name);

  NameLocation searchInEnclosingScope(JSContext* cx, CompilationInput& input,
                                      ParserAtomsTable& parserAtoms,
                                      TaggedParserAtomIndex name, uint8_t hops);

  bool effectiveScopePrivateFieldCacheHas(TaggedParserAtomIndex name);
  mozilla::Maybe<NameLocation> getPrivateFieldLocation(
      TaggedParserAtomIndex name);

 private:
  void computeThisBinding(Scope* scope);
  void computeThisEnvironment(Scope* enclosingScope);
  void computeInScope(Scope* enclosingScope);
  void cacheEnclosingScope(Scope* enclosingScope);

  Scope* determineEffectiveScope(Scope* scope, JSObject* environment);

  bool cacheEnclosingScopeBindingForEval(JSContext* cx, CompilationInput& input,
                                         ParserAtomsTable& parserAtoms);

  bool cachePrivateFieldsForEval(JSContext* cx, CompilationInput& input,
                                 JSObject* enclosingEnvironment,
                                 Scope* effectiveScope,
                                 ParserAtomsTable& parserAtoms);

  bool addToEnclosingLexicalBindingCache(JSContext* cx, CompilationInput& input,
                                         ParserAtomsTable& parserAtoms,
                                         JSAtom* name,
                                         EnclosingLexicalBindingKind kind);
};

struct CompilationAtomCache {
 public:
  using AtomCacheVector = JS::GCVector<JSAtom*, 0, js::SystemAllocPolicy>;

 private:
  // Atoms lowered into or converted from CompilationStencil.parserAtomData.
  //
  // This field is here instead of in CompilationGCOutput because atoms lowered
  // from JSAtom is part of input (enclosing scope bindings, lazy function name,
  // etc), and having 2 vectors in both input/output is error prone.
  AtomCacheVector atoms_;

 public:
  JSAtom* getExistingAtomAt(ParserAtomIndex index) const;
  JSAtom* getExistingAtomAt(JSContext* cx,
                            TaggedParserAtomIndex taggedIndex) const;
  JSAtom* getAtomAt(ParserAtomIndex index) const;
  bool hasAtomAt(ParserAtomIndex index) const;
  bool setAtomAt(JSContext* cx, ParserAtomIndex index, JSAtom* atom);
  bool allocate(JSContext* cx, size_t length);

  void stealBuffer(AtomCacheVector& atoms);
  void releaseBuffer(AtomCacheVector& atoms);

  void trace(JSTracer* trc);

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return atoms_.sizeOfExcludingThis(mallocSizeOf);
  }
};

// Input of the compilation, including source and enclosing context.
struct CompilationInput {
  enum class CompilationTarget {
    Global,
    SelfHosting,
    StandaloneFunction,
    StandaloneFunctionInNonSyntacticScope,
    Eval,
    Module,
    Delazification,
  };
  CompilationTarget target = CompilationTarget::Global;

  const JS::ReadOnlyCompileOptions& options;

  CompilationAtomCache atomCache;

 private:
  BaseScript* lazy_ = nullptr;

 public:
  RefPtr<ScriptSource> source;

  //  * If the target is Global, null.
  //  * If the target is SelfHosting, an empty global scope.
  //    This scope is also used for EmptyGlobalScopeType in
  //    CompilationStencil.gcThings.
  //    See the comment in initForSelfHostingGlobal.
  //  * If the target is StandaloneFunction, an empty global scope.
  //  * If the target is StandaloneFunctionInNonSyntacticScope, the non-null
  //    enclosing scope of the function
  //  * If the target is Eval, the non-null enclosing scope of the `eval`.
  //  * If the target is Module, null that means empty global scope
  //    (See EmitterScope::checkEnvironmentChainLength)
  //  * If the target is Delazification, the non-null enclosing scope of
  //    the function
  Scope* enclosingScope = nullptr;

  explicit CompilationInput(const JS::ReadOnlyCompileOptions& options)
      : options(options) {}

 private:
  bool initScriptSource(JSContext* cx);

 public:
  bool initForGlobal(JSContext* cx) {
    target = CompilationTarget::Global;
    return initScriptSource(cx);
  }

  bool initForSelfHostingGlobal(JSContext* cx) {
    target = CompilationTarget::SelfHosting;
    if (!initScriptSource(cx)) {
      return false;
    }

    // This enclosing scope is also recorded as EmptyGlobalScopeType in
    // CompilationStencil.gcThings even though corresponding ScopeStencil
    // isn't generated.
    //
    // Store the enclosing scope here in order to access it from
    // inner scopes' ScopeStencil::enclosing.
    enclosingScope = &cx->global()->emptyGlobalScope();
    return true;
  }

  bool initForStandaloneFunction(JSContext* cx) {
    target = CompilationTarget::StandaloneFunction;
    if (!initScriptSource(cx)) {
      return false;
    }
    enclosingScope = &cx->global()->emptyGlobalScope();
    return true;
  }

  bool initForStandaloneFunctionInNonSyntacticScope(
      JSContext* cx, HandleScope functionEnclosingScope);

  bool initForEval(JSContext* cx, HandleScope evalEnclosingScope) {
    target = CompilationTarget::Eval;
    if (!initScriptSource(cx)) {
      return false;
    }
    enclosingScope = evalEnclosingScope;
    return true;
  }

  bool initForModule(JSContext* cx) {
    target = CompilationTarget::Module;
    if (!initScriptSource(cx)) {
      return false;
    }
    // The `enclosingScope` is the emptyGlobalScope.
    return true;
  }

  void initFromLazy(JSContext* cx, BaseScript* lazyScript, ScriptSource* ss) {
    MOZ_ASSERT(cx->compartment() == lazyScript->compartment());

    // We can only compile functions whose parents have previously been
    // compiled, because compilation requires full information about the
    // function's immediately enclosing scope.
    MOZ_ASSERT(lazyScript->isReadyForDelazification());
    target = CompilationTarget::Delazification;
    lazy_ = lazyScript;
    source = ss;
    enclosingScope = lazy_->function()->enclosingScope();
  }

  // Returns true if enclosingScope field is provided to init* function,
  // instead of setting to empty global internally.
  bool hasNonDefaultEnclosingScope() const {
    return target == CompilationTarget::StandaloneFunctionInNonSyntacticScope ||
           target == CompilationTarget::Eval ||
           target == CompilationTarget::Delazification;
  }

  // Returns the enclosing scope provided to init* function.
  // nullptr otherwise.
  Scope* maybeNonDefaultEnclosingScope() const {
    if (hasNonDefaultEnclosingScope()) {
      return enclosingScope;
    }
    return nullptr;
  }

  // FullParseHandler needs a BaseScript to find the closed-over-binding index,
  // as well as to walk over the inner functions references with
  // skipLazyInnerFunctions.
  BaseScript* lazyOuterScript() {
    MOZ_ASSERT(isInitialStencil() == !lazy_);
    return lazy_;
  }

  // When compiling a lazy function, this is needed to initialize the
  // FunctionBox as well as the CompilationState.
  JSFunction* function() { return lazy_->function(); }

  // When compiling an inner function, we want to know the unique identifier
  // which identify a function. This is computed from the source extend.
  SourceExtent extent() const { return lazy_->extent(); }

  // See `BaseScript::immutableFlags_`.
  ImmutableScriptFlags immutableFlags() const {
    return lazy_->immutableFlags();
  }

  RO_IMMUTABLE_SCRIPT_FLAGS(immutableFlags())

  bool hasPrivateScriptData() const {
    // This is equivalent to: ngcthings != 0 || useMemberInitializers()
    // See BaseScript::CreateRawLazy.
    return lazy_->hasPrivateScriptData();
  }

  // Whether this CompilationInput is parsing the top-level of a script, or
  // false if we are parsing an inner function.
  bool isInitialStencil() { return !lazy_; }

  void trace(JSTracer* trc);

  // Size of dynamic data. Note that GC data is counted by GC and not here. We
  // also ignore ScriptSource which is a shared RefPtr.
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return atomCache.sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }
};

// AsmJS scripts are very rare on-average, so we use a HashMap to associate
// data with a ScriptStencil. The ScriptStencil has a flag to indicate if we
// need to even do this lookup.
using StencilAsmJSMap =
    HashMap<ScriptIndex, RefPtr<const JS::WasmModule>,
            mozilla::DefaultHasher<ScriptIndex>, js::SystemAllocPolicy>;

struct StencilAsmJSContainer
    : public js::AtomicRefCounted<StencilAsmJSContainer> {
  StencilAsmJSMap moduleMap;

  StencilAsmJSContainer() = default;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return moduleMap.shallowSizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }
};

// Store shared data for non-lazy script.
struct SharedDataContainer {
  // NOTE: While stored, we must hold a ref-count and care must be taken when
  //       updating or clearing the pointer.
  using SingleSharedDataPtr = SharedImmutableScriptData*;

  using SharedDataVector =
      Vector<RefPtr<js::SharedImmutableScriptData>, 0, js::SystemAllocPolicy>;
  using SharedDataVectorPtr = SharedDataVector*;

  using SharedDataMap =
      HashMap<ScriptIndex, RefPtr<js::SharedImmutableScriptData>,
              mozilla::DefaultHasher<ScriptIndex>, js::SystemAllocPolicy>;
  using SharedDataMapPtr = SharedDataMap*;

 private:
  enum {
    SingleTag = 0,
    VectorTag = 1,
    MapTag = 2,
    BorrowTag = 3,

    TagMask = 3,
  };

  uintptr_t data_ = 0;

 public:
  // Defaults to SingleSharedData.
  SharedDataContainer() = default;

  SharedDataContainer(const SharedDataContainer&) = delete;
  SharedDataContainer(SharedDataContainer&& other) noexcept {
    std::swap(data_, other.data_);
    MOZ_ASSERT(other.isEmpty());
  }

  SharedDataContainer& operator=(const SharedDataContainer&) = delete;
  SharedDataContainer& operator=(SharedDataContainer&& other) noexcept {
    std::swap(data_, other.data_);
    MOZ_ASSERT(other.isEmpty());
    return *this;
  }

  ~SharedDataContainer();

  [[nodiscard]] bool initVector(JSContext* cx);
  [[nodiscard]] bool initMap(JSContext* cx);

 private:
  [[nodiscard]] bool convertFromSingleToMap(JSContext* cx);

 public:
  bool isEmpty() const { return (data_) == SingleTag; }
  bool isSingle() const { return (data_ & TagMask) == SingleTag; }
  bool isVector() const { return (data_ & TagMask) == VectorTag; }
  bool isMap() const { return (data_ & TagMask) == MapTag; }
  bool isBorrow() const { return (data_ & TagMask) == BorrowTag; }

  void setSingle(already_AddRefed<SharedImmutableScriptData>&& data) {
    MOZ_ASSERT(isEmpty());
    data_ = reinterpret_cast<uintptr_t>(data.take());
    MOZ_ASSERT(isSingle());
    MOZ_ASSERT(!isEmpty());
  }

  void setBorrow(SharedDataContainer* sharedData) {
    MOZ_ASSERT(isEmpty());
    data_ = reinterpret_cast<uintptr_t>(sharedData) | BorrowTag;
    MOZ_ASSERT(isBorrow());
  }

  SingleSharedDataPtr asSingle() const {
    MOZ_ASSERT(isSingle());
    MOZ_ASSERT(!isEmpty());
    static_assert(SingleTag == 0);
    return reinterpret_cast<SingleSharedDataPtr>(data_);
  }
  SharedDataVectorPtr asVector() const {
    MOZ_ASSERT(isVector());
    return reinterpret_cast<SharedDataVectorPtr>(data_ & ~TagMask);
  }
  SharedDataMapPtr asMap() const {
    MOZ_ASSERT(isMap());
    return reinterpret_cast<SharedDataMapPtr>(data_ & ~TagMask);
  }
  SharedDataContainer* asBorrow() const {
    MOZ_ASSERT(isBorrow());
    return reinterpret_cast<SharedDataContainer*>(data_ & ~TagMask);
  }

  [[nodiscard]] bool prepareStorageFor(JSContext* cx, size_t nonLazyScriptCount,
                                       size_t allScriptCount);

  // Returns index-th script's shared data, or nullptr if it doesn't have.
  js::SharedImmutableScriptData* get(ScriptIndex index) const;

  // Add data for index-th script and share it with VM.
  [[nodiscard]] bool addAndShare(JSContext* cx, ScriptIndex index,
                                 js::SharedImmutableScriptData* data);

  // Add data for index-th script without sharing it with VM.
  // The data should already be shared with VM.
  //
  // The data is supposed to be added from delazification.
  [[nodiscard]] bool addExtraWithoutShare(JSContext* cx, ScriptIndex index,
                                          js::SharedImmutableScriptData* data);

  // Dynamic memory associated with this container. Does not include the
  // SharedImmutableScriptData since we are not the unique owner of it.
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    if (isVector()) {
      return asVector()->sizeOfIncludingThis(mallocSizeOf);
    }
    if (isMap()) {
      return asMap()->shallowSizeOfIncludingThis(mallocSizeOf);
    }
    MOZ_ASSERT(isSingle() || isBorrow());
    return 0;
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(js::JSONPrinter& json) const;
  void dumpFields(js::JSONPrinter& json) const;
#endif
};

struct ExtensibleCompilationStencil;

// The top level struct of stencil specialized for non-extensible case.
// Used as the compilation output, and also XDR decode output.
//
// In XDR decode output case, the span and not-owning pointer fields point
// the internal LifoAlloc and the external XDR buffer.
//
// In BorrowingCompilationStencil usage, span and not-owning pointer fields
// point the ExtensibleCompilationStencil and its LifoAlloc.
//
// The dependent XDR buffer or ExtensibleCompilationStencil must be kept
// alive manually.
struct CompilationStencil {
  static constexpr ScriptIndex TopLevelIndex = ScriptIndex(0);

  static constexpr size_t LifoAllocChunkSize = 512;

  // The lifetime of this CompilationStencil may be managed by stack allocation,
  // UniquePtr<T>, or RefPtr<T>. If a RefPtr is used, this ref-count will track
  // the lifetime, otherwise it is ignored.
  //
  // NOTE: Internal code and public APIs use a mix of these different allocation
  //       modes.
  //
  // See: JS::StencilAddRef/Release
  mutable mozilla::Atomic<uintptr_t> refCount{0};

  // Set to true if any pointer/span contains external data instead of
  // LifoAlloc or owned memory.
  bool hasExternalDependency = false;

  // Value of CanLazilyParse(CompilationInput) on compilation.
  // Used during instantiation.
  bool canLazilyParse = false;

  // FunctionKey is an encoded position of a function within the source text
  // that is reproducible.
  using FunctionKey = uint32_t;
  static constexpr FunctionKey NullFunctionKey = 0;

  // If this stencil is a delazification, this identifies location of the
  // function in the source text.
  FunctionKey functionKey = NullFunctionKey;

  // This holds allocations that do not require destructors to be run but are
  // live until the stencil is released.
  LifoAlloc alloc;

  // The source text holder for the script. This may be an empty placeholder if
  // the code will fully parsed and options indicate the source will never be
  // needed again.
  RefPtr<ScriptSource> source;

  // Stencil for all function and non-function scripts. The TopLevelIndex is
  // reserved for the top-level script. This top-level may or may not be a
  // function.
  mozilla::Span<ScriptStencil> scriptData;

  // Immutable data computed during initial compilation and never updated during
  // delazification.
  mozilla::Span<ScriptStencilExtra> scriptExtra;

  mozilla::Span<TaggedScriptThingIndex> gcThingData;

  // scopeData and scopeNames have the same size, and i-th scopeNames contains
  // the names for the bindings contained in the slot defined by i-th scopeData.
  mozilla::Span<ScopeStencil> scopeData;
  mozilla::Span<BaseParserScopeData*> scopeNames;

  // Hold onto the RegExpStencil, BigIntStencil, and ObjLiteralStencil that are
  // allocated during parse to ensure correct destruction.
  mozilla::Span<RegExpStencil> regExpData;
  mozilla::Span<BigIntStencil> bigIntData;
  mozilla::Span<ObjLiteralStencil> objLiteralData;

  // List of parser atoms for this compilation. This may contain nullptr entries
  // when round-tripping with XDR if the atom was generated in original parse
  // but not used by stencil.
  ParserAtomSpan parserAtomData;

  // Variable sized container for bytecode and other immutable data. A valid
  // stencil always contains at least an entry for `TopLevelIndex` script.
  SharedDataContainer sharedData;

  // Module metadata if this is a module compile.
  RefPtr<StencilModuleMetadata> moduleMetadata;

  // AsmJS modules generated by parsing. These scripts are never lazy and
  // therefore only generated during initial parse.
  RefPtr<StencilAsmJSContainer> asmJS;

  // End of fields.

  // Construct a CompilationStencil
  explicit CompilationStencil(ScriptSource* source)
      : alloc(LifoAllocChunkSize), source(source) {}

  static FunctionKey toFunctionKey(const SourceExtent& extent) {
    // In eval("x=>1"), the arrow function will have a sourceStart of 0 which
    // conflicts with the NullFunctionKey, so shift all keys by 1 instead.
    auto result = extent.sourceStart + 1;
    MOZ_ASSERT(result != NullFunctionKey);
    return result;
  }

  bool isInitialStencil() const { return functionKey == NullFunctionKey; }

  [[nodiscard]] static bool instantiateStencilAfterPreparation(
      JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
      CompilationGCOutput& gcOutput);

  [[nodiscard]] static bool prepareForInstantiate(
      JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
      CompilationGCOutput& gcOutput);

  [[nodiscard]] static bool instantiateStencils(
      JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
      CompilationGCOutput& gcOutput);

  [[nodiscard]] bool serializeStencils(JSContext* cx, CompilationInput& input,
                                       JS::TranscodeBuffer& buf,
                                       bool* succeededOut = nullptr) const;
  [[nodiscard]] bool deserializeStencils(JSContext* cx, CompilationInput& input,
                                         const JS::TranscodeRange& range,
                                         bool* succeededOut = nullptr);

  // To avoid any misuses, make sure this is neither copyable or assignable.
  CompilationStencil(const CompilationStencil&) = delete;
  CompilationStencil(CompilationStencil&&) = delete;
  CompilationStencil& operator=(const CompilationStencil&) = delete;
  CompilationStencil& operator=(CompilationStencil&&) = delete;

  static inline ScriptStencilIterable functionScriptStencils(
      const CompilationStencil& stencil, CompilationGCOutput& gcOutput);

  void setFunctionKey(BaseScript* lazy) {
    functionKey = toFunctionKey(lazy->extent());
  }

  inline size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  // Steal ExtensibleCompilationStencil content.
  [[nodiscard]] bool steal(JSContext* cx, ExtensibleCompilationStencil&& other);

#ifdef DEBUG
  void assertNoExternalDependency() const;
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(js::JSONPrinter& json) const;
  void dumpFields(js::JSONPrinter& json) const;

  void dumpAtom(TaggedParserAtomIndex index) const;
#endif
};

// The top level struct of stencil specialized for extensible case.
// Used as the temporary storage during compilation, an the compilation output.
//
// All not-owning pointer fields point the internal LifoAlloc.
//
// See CompilationStencil for each field's description.
struct ExtensibleCompilationStencil {
  bool canLazilyParse = false;

  using FunctionKey = CompilationStencil::FunctionKey;

  FunctionKey functionKey = CompilationStencil::NullFunctionKey;

  // Data pointed by other fields are allocated in this LifoAlloc,
  // and moved to `CompilationStencil.alloc`.
  LifoAlloc alloc;

  RefPtr<ScriptSource> source;

  // NOTE: We reserve a modest amount of inline storage in order to reduce
  //       allocations in the most common delazification cases. These common
  //       cases have one script and scope, as well as a handful of gcthings.
  //       For complex pages this covers about 75% of delazifications.

  Vector<ScriptStencil, 1, js::SystemAllocPolicy> scriptData;
  Vector<ScriptStencilExtra, 0, js::SystemAllocPolicy> scriptExtra;

  Vector<TaggedScriptThingIndex, 8, js::SystemAllocPolicy> gcThingData;

  Vector<ScopeStencil, 1, js::SystemAllocPolicy> scopeData;
  Vector<BaseParserScopeData*, 1, js::SystemAllocPolicy> scopeNames;

  Vector<RegExpStencil, 0, js::SystemAllocPolicy> regExpData;
  Vector<BigIntStencil, 0, js::SystemAllocPolicy> bigIntData;
  Vector<ObjLiteralStencil, 0, js::SystemAllocPolicy> objLiteralData;

  // Table of parser atoms for this compilation.
  ParserAtomsTable parserAtoms;

  SharedDataContainer sharedData;

  RefPtr<StencilModuleMetadata> moduleMetadata;

  RefPtr<StencilAsmJSContainer> asmJS;

  ExtensibleCompilationStencil(JSContext* cx, CompilationInput& input);

  ExtensibleCompilationStencil(ExtensibleCompilationStencil&& other) noexcept
      : canLazilyParse(other.canLazilyParse),
        functionKey(other.functionKey),
        alloc(CompilationStencil::LifoAllocChunkSize),
        source(std::move(other.source)),
        scriptData(std::move(other.scriptData)),
        scriptExtra(std::move(other.scriptExtra)),
        gcThingData(std::move(other.gcThingData)),
        scopeData(std::move(other.scopeData)),
        scopeNames(std::move(other.scopeNames)),
        regExpData(std::move(other.regExpData)),
        bigIntData(std::move(other.bigIntData)),
        objLiteralData(std::move(other.objLiteralData)),
        parserAtoms(std::move(other.parserAtoms)),
        sharedData(std::move(other.sharedData)),
        moduleMetadata(std::move(other.moduleMetadata)),
        asmJS(std::move(other.asmJS)) {
    alloc.steal(&other.alloc);
    parserAtoms.fixupAlloc(alloc);
  }

  ExtensibleCompilationStencil& operator=(
      ExtensibleCompilationStencil&& other) noexcept {
    MOZ_ASSERT(alloc.isEmpty());

    canLazilyParse = other.canLazilyParse;
    functionKey = other.functionKey;
    source = std::move(other.source);
    scriptData = std::move(other.scriptData);
    scriptExtra = std::move(other.scriptExtra);
    gcThingData = std::move(other.gcThingData);
    scopeData = std::move(other.scopeData);
    scopeNames = std::move(other.scopeNames);
    regExpData = std::move(other.regExpData);
    bigIntData = std::move(other.bigIntData);
    objLiteralData = std::move(other.objLiteralData);
    parserAtoms = std::move(other.parserAtoms);
    sharedData = std::move(other.sharedData);
    moduleMetadata = std::move(other.moduleMetadata);
    asmJS = std::move(other.asmJS);

    alloc.steal(&other.alloc);
    parserAtoms.fixupAlloc(alloc);

    return *this;
  }

  void setFunctionKey(const SourceExtent& extent) {
    functionKey = CompilationStencil::toFunctionKey(extent);
  }

  bool isInitialStencil() const {
    return functionKey == CompilationStencil::NullFunctionKey;
  }

  // Steal CompilationStencil content.
  [[nodiscard]] bool steal(JSContext* cx, CompilationStencil&& other);

  inline size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

#ifdef DEBUG
  void assertNoExternalDependency() const;
#endif
};

// The internal state of the compilation.
struct MOZ_RAII CompilationState : public ExtensibleCompilationStencil {
  Directives directives;

  ScopeContext scopeContext;

  UsedNameTracker usedNames;
  LifoAllocScope& allocScope;

  CompilationInput& input;

  // The number of functions that *will* have bytecode.
  // This doesn't count top-level non-function script.
  //
  // This should be counted while parsing, and should be passed to
  // SharedDataContainer.prepareStorageFor *before* start emitting bytecode.
  size_t nonLazyFunctionCount = 0;

  // End of fields.

  CompilationState(JSContext* cx, LifoAllocScope& frontendAllocScope,
                   CompilationInput& input);

  bool init(JSContext* cx, InheritThis inheritThis = InheritThis::No,
            JSObject* enclosingEnv = nullptr) {
    return scopeContext.init(cx, input, parserAtoms, inheritThis, enclosingEnv);
  }

  // Track the state of key allocations and roll them back as parts of parsing
  // get retried. This ensures iteration during stencil instantiation does not
  // encounter discarded frontend state.
  struct CompilationStatePosition {
    // Temporarily share this token struct with CompilationState.
    size_t scriptDataLength = 0;

    size_t asmJSCount = 0;
  };

  bool prepareSharedDataStorage(JSContext* cx);

  CompilationStatePosition getPosition();
  void rewind(const CompilationStatePosition& pos);

  // When parsing arrow function, parameter is parsed twice, and if there are
  // functions inside parameter expression, stencils will be created for them.
  //
  // Those functions exist only for lazy parsing.
  // Mark them "ghost", so that they don't affect other parts.
  //
  // See GHOST_FUNCTION in FunctionFlags.h for more details.
  void markGhost(const CompilationStatePosition& pos);

  // Allocate space for `length` gcthings, and return the address of the
  // first element to `cursor` to initialize on the caller.
  bool allocateGCThingsUninitialized(JSContext* cx, ScriptIndex scriptIndex,
                                     size_t length,
                                     TaggedScriptThingIndex** cursor);

  bool appendScriptStencilAndData(JSContext* cx);

  bool appendGCThings(JSContext* cx, ScriptIndex scriptIndex,
                      mozilla::Span<const TaggedScriptThingIndex> things);
};

// A temporary CompilationStencil instance that borrows
// ExtensibleCompilationStencil data.
// Ensure that this instance does not outlive the ExtensibleCompilationStencil.
class MOZ_STACK_CLASS BorrowingCompilationStencil : public CompilationStencil {
 public:
  explicit BorrowingCompilationStencil(
      ExtensibleCompilationStencil& extensibleStencil);
};

// Size of dynamic data. Ignores Spans (unless their contents are in the
// LifoAlloc) and RefPtrs since we are not the unique owner.
inline size_t CompilationStencil::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t moduleMetadataSize =
      moduleMetadata ? moduleMetadata->sizeOfIncludingThis(mallocSizeOf) : 0;
  size_t asmJSSize = asmJS ? asmJS->sizeOfIncludingThis(mallocSizeOf) : 0;

  return alloc.sizeOfExcludingThis(mallocSizeOf) +
         sharedData.sizeOfExcludingThis(mallocSizeOf) + moduleMetadataSize +
         asmJSSize;
}

inline size_t ExtensibleCompilationStencil::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t moduleMetadataSize =
      moduleMetadata ? moduleMetadata->sizeOfIncludingThis(mallocSizeOf) : 0;
  size_t asmJSSize = asmJS ? asmJS->sizeOfIncludingThis(mallocSizeOf) : 0;

  return alloc.sizeOfExcludingThis(mallocSizeOf) +
         scriptData.sizeOfExcludingThis(mallocSizeOf) +
         scriptExtra.sizeOfExcludingThis(mallocSizeOf) +
         gcThingData.sizeOfExcludingThis(mallocSizeOf) +
         scopeData.sizeOfExcludingThis(mallocSizeOf) +
         scopeNames.sizeOfExcludingThis(mallocSizeOf) +
         regExpData.sizeOfExcludingThis(mallocSizeOf) +
         bigIntData.sizeOfExcludingThis(mallocSizeOf) +
         objLiteralData.sizeOfExcludingThis(mallocSizeOf) +
         parserAtoms.sizeOfExcludingThis(mallocSizeOf) +
         sharedData.sizeOfExcludingThis(mallocSizeOf) + moduleMetadataSize +
         asmJSSize;
}

// The output of GC allocation from stencil.
struct CompilationGCOutput {
  // The resulting outermost script for the compilation powered
  // by this CompilationStencil.
  JSScript* script = nullptr;

  // The resulting module object if there is one.
  ModuleObject* module = nullptr;

  // A Rooted vector to handle tracing of JSFunction* and Atoms within.
  //
  // If the top level script isn't a function, the item at TopLevelIndex is
  // nullptr.
  JS::GCVector<JSFunction*, 1, js::SystemAllocPolicy> functions;

  // References to scopes are controlled via AbstractScopePtr, which holds onto
  // an index (and CompilationStencil reference).
  JS::GCVector<js::Scope*, 1, js::SystemAllocPolicy> scopes;

  // The result ScriptSourceObject. This is unused in delazifying parses.
  ScriptSourceObject* sourceObject = nullptr;

  CompilationGCOutput() = default;

  // Reserve output vector capacity. This may be called before instantiate to do
  // allocations ahead of time (off thread). The stencil instantiation code will
  // also run this to ensure the vectors are ready.
  [[nodiscard]] bool ensureReserved(JSContext* cx, size_t scriptDataLength,
                                    size_t scopeDataLength) {
    if (!functions.reserve(scriptDataLength)) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (!scopes.reserve(scopeDataLength)) {
      ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  // Size of dynamic data. Note that GC data is counted by GC and not here.
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return functions.sizeOfExcludingThis(mallocSizeOf) +
           scopes.sizeOfExcludingThis(mallocSizeOf);
  }

  void trace(JSTracer* trc);
};

// Iterator over functions that make up a CompilationStencil. This abstracts
// over the parallel arrays in stencil and gc-output that use the same index
// system.
class ScriptStencilIterable {
 public:
  class ScriptAndFunction {
   public:
    const ScriptStencil& script;
    const ScriptStencilExtra* scriptExtra;
    JSFunction* function;
    ScriptIndex index;

    ScriptAndFunction() = delete;
    ScriptAndFunction(const ScriptStencil& script,
                      const ScriptStencilExtra* scriptExtra,
                      JSFunction* function, ScriptIndex index)
        : script(script),
          scriptExtra(scriptExtra),
          function(function),
          index(index) {}
  };

  class Iterator {
    size_t index_ = 0;
    const CompilationStencil& stencil_;
    CompilationGCOutput& gcOutput_;

    Iterator(const CompilationStencil& stencil, CompilationGCOutput& gcOutput,
             size_t index)
        : index_(index), stencil_(stencil), gcOutput_(gcOutput) {
      MOZ_ASSERT(index == stencil.scriptData.size());
    }

   public:
    explicit Iterator(const CompilationStencil& stencil,
                      CompilationGCOutput& gcOutput)
        : stencil_(stencil), gcOutput_(gcOutput) {
      skipTopLevelNonFunction();
    }

    Iterator operator++() {
      next();
      assertFunction();
      return *this;
    }

    void next() {
      MOZ_ASSERT(index_ < stencil_.scriptData.size());
      index_++;
    }

    void assertFunction() {
      if (index_ < stencil_.scriptData.size()) {
        MOZ_ASSERT(stencil_.scriptData[index_].isFunction());
      }
    }

    void skipTopLevelNonFunction() {
      MOZ_ASSERT(index_ == 0);
      if (stencil_.scriptData.size()) {
        if (!stencil_.scriptData[0].isFunction()) {
          next();
          assertFunction();
        }
      }
    }

    bool operator!=(const Iterator& other) const {
      return index_ != other.index_;
    }

    ScriptAndFunction operator*() {
      ScriptIndex index = ScriptIndex(index_);
      const ScriptStencil& script = stencil_.scriptData[index];
      const ScriptStencilExtra* scriptExtra = nullptr;
      if (stencil_.isInitialStencil()) {
        scriptExtra = &stencil_.scriptExtra[index];
      }
      return ScriptAndFunction(script, scriptExtra, gcOutput_.functions[index],
                               index);
    }

    static Iterator end(const CompilationStencil& stencil,
                        CompilationGCOutput& gcOutput) {
      return Iterator(stencil, gcOutput, stencil.scriptData.size());
    }
  };

  const CompilationStencil& stencil_;
  CompilationGCOutput& gcOutput_;

  explicit ScriptStencilIterable(const CompilationStencil& stencil,
                                 CompilationGCOutput& gcOutput)
      : stencil_(stencil), gcOutput_(gcOutput) {}

  Iterator begin() const { return Iterator(stencil_, gcOutput_); }

  Iterator end() const { return Iterator::end(stencil_, gcOutput_); }
};

inline ScriptStencilIterable CompilationStencil::functionScriptStencils(
    const CompilationStencil& stencil, CompilationGCOutput& gcOutput) {
  return ScriptStencilIterable(stencil, gcOutput);
}

// Merge CompilationStencil for delazification into initial
// ExtensibleCompilationStencil.
struct CompilationStencilMerger {
 private:
  using FunctionKey = ExtensibleCompilationStencil::FunctionKey;

  // The stencil for the initial compilation.
  // Delazifications are merged into this.
  //
  // If any failure happens during merge operation, this field is reset to
  // nullptr.
  UniquePtr<ExtensibleCompilationStencil> initial_;

  // A Map from function key to the ScriptIndex in the initial stencil.
  using FunctionKeyToScriptIndexMap =
      HashMap<FunctionKey, ScriptIndex, mozilla::DefaultHasher<FunctionKey>,
              js::SystemAllocPolicy>;
  FunctionKeyToScriptIndexMap functionKeyToInitialScriptIndex_;

  [[nodiscard]] bool buildFunctionKeyToIndex(JSContext* cx);

  ScriptIndex getInitialScriptIndexFor(
      const CompilationStencil& delazification) const;

  // A map from delazification's ParserAtomIndex to
  // initial's TaggedParserAtomIndex
  using AtomIndexMap = Vector<TaggedParserAtomIndex, 0, js::SystemAllocPolicy>;

  [[nodiscard]] bool buildAtomIndexMap(JSContext* cx,
                                       const CompilationStencil& delazification,
                                       AtomIndexMap& atomIndexMap);

 public:
  CompilationStencilMerger() = default;

  // Set the initial stencil and prepare for merging.
  [[nodiscard]] bool setInitial(
      JSContext* cx, UniquePtr<ExtensibleCompilationStencil>&& initial);

  // Merge the delazification stencil into the initial stencil.
  [[nodiscard]] bool addDelazification(
      JSContext* cx, const CompilationStencil& delazification);

  ExtensibleCompilationStencil& getResult() const { return *initial_; }
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_CompilationStencil_h
