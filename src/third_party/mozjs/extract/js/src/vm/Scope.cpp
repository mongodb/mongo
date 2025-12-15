/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Scope.h"

#include <new>

#include "jsnum.h"

#include "frontend/CompilationStencil.h"  // ScopeStencilRef, CompilationStencil, CompilationState, CompilationAtomCache
#include "frontend/ParserAtom.h"  // frontend::ParserAtomsTable, frontend::ParserAtom
#include "frontend/ScriptIndex.h"  // ScriptIndex
#include "frontend/Stencil.h"
#include "util/StringBuilder.h"
#include "vm/EnvironmentObject.h"
#include "vm/ErrorReporting.h"  // MaybePrintAndClearPendingException
#include "vm/JSScript.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmInstance.h"

#include "gc/GCContext-inl.h"
#include "gc/ObjectKind-inl.h"
#include "gc/TraceMethods-inl.h"
#include "vm/JSContext-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::frontend;

const char* js::BindingKindString(BindingKind kind) {
  switch (kind) {
    case BindingKind::Import:
      return "import";
    case BindingKind::FormalParameter:
      return "formal parameter";
    case BindingKind::Var:
      return "var";
    case BindingKind::Let:
      return "let";
    case BindingKind::Const:
      return "const";
    case BindingKind::NamedLambdaCallee:
      return "named lambda callee";
    case BindingKind::Synthetic:
      return "synthetic";
    case BindingKind::PrivateMethod:
      return "private method";
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case BindingKind::Using:
      return "using";
#endif
  }
  MOZ_CRASH("Bad BindingKind");
}

const char* js::ScopeKindString(ScopeKind kind) {
  switch (kind) {
    case ScopeKind::Function:
      return "function";
    case ScopeKind::FunctionBodyVar:
      return "function body var";
    case ScopeKind::Lexical:
      return "lexical";
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
      return "catch";
    case ScopeKind::NamedLambda:
      return "named lambda";
    case ScopeKind::StrictNamedLambda:
      return "strict named lambda";
    case ScopeKind::FunctionLexical:
      return "function lexical";
    case ScopeKind::ClassBody:
      return "class body";
    case ScopeKind::With:
      return "with";
    case ScopeKind::Eval:
      return "eval";
    case ScopeKind::StrictEval:
      return "strict eval";
    case ScopeKind::Global:
      return "global";
    case ScopeKind::NonSyntactic:
      return "non-syntactic";
    case ScopeKind::Module:
      return "module";
    case ScopeKind::WasmInstance:
      return "wasm instance";
    case ScopeKind::WasmFunction:
      return "wasm function";
  }
  MOZ_CRASH("Bad ScopeKind");
}

SharedShape* js::EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                                       uint32_t numSlots,
                                       ObjectFlags objectFlags) {
  // Put as many slots into the object header as possible.
  uint32_t numFixed = gc::GetGCKindSlots(gc::GetGCObjectKind(numSlots));
  return SharedShape::getInitialShape(
      cx, cls, cx->realm(), TaggedProto(nullptr), numFixed, objectFlags);
}

static bool AddToEnvironmentMap(JSContext* cx, const JSClass* clasp,
                                HandleId id, BindingKind bindKind,
                                uint32_t slot,
                                MutableHandle<SharedPropMap*> map,
                                uint32_t* mapLength, ObjectFlags* objectFlags) {
  PropertyFlags propFlags = {PropertyFlag::Enumerable};
  switch (bindKind) {
    case BindingKind::Const:
    case BindingKind::NamedLambdaCallee:
      // Non-writable.
      break;
    default:
      propFlags.setFlag(PropertyFlag::Writable);
      break;
  }

  return SharedPropMap::addPropertyWithKnownSlot(cx, clasp, map, mapLength, id,
                                                 propFlags, slot, objectFlags);
}

SharedShape* js::CreateEnvironmentShape(JSContext* cx, BindingIter& bi,
                                        const JSClass* cls, uint32_t numSlots,
                                        ObjectFlags objectFlags) {
  Rooted<SharedPropMap*> map(cx);
  uint32_t mapLength = 0;

  RootedId id(cx);
  for (; bi; bi++) {
    BindingLocation loc = bi.location();
    if (loc.kind() == BindingLocation::Kind::Environment) {
      JSAtom* name = bi.name();
      MOZ_ASSERT(AtomIsMarked(cx->zone(), name));
      id = NameToId(name->asPropertyName());
      if (!AddToEnvironmentMap(cx, cls, id, bi.kind(), loc.slot(), &map,
                               &mapLength, &objectFlags)) {
        return nullptr;
      }
    }
  }

  uint32_t numFixed = gc::GetGCKindSlots(gc::GetGCObjectKind(numSlots));
  return SharedShape::getInitialOrPropMapShape(cx, cls, cx->realm(),
                                               TaggedProto(nullptr), numFixed,
                                               map, mapLength, objectFlags);
}

SharedShape* js::CreateEnvironmentShapeForSyntheticModule(
    JSContext* cx, const JSClass* cls, uint32_t numSlots,
    Handle<ModuleObject*> module) {
  Rooted<SharedPropMap*> map(cx);
  uint32_t mapLength = 0;

  PropertyFlags propFlags = {PropertyFlag::Enumerable};
  ObjectFlags objectFlags = ModuleEnvironmentObject::OBJECT_FLAGS;

  RootedId id(cx);
  uint32_t slotIndex = numSlots;
  for (JSAtom* exportName : module->syntheticExportNames()) {
    id = NameToId(exportName->asPropertyName());
    if (!SharedPropMap::addPropertyWithKnownSlot(cx, cls, &map, &mapLength, id,
                                                 propFlags, slotIndex,
                                                 &objectFlags)) {
      return nullptr;
    }
    slotIndex++;
  }

  uint32_t numFixed = gc::GetGCKindSlots(gc::GetGCObjectKind(numSlots));
  return SharedShape::getInitialOrPropMapShape(cx, cls, cx->realm(),
                                               TaggedProto(nullptr), numFixed,
                                               map, mapLength, objectFlags);
}

template <class DataT>
inline size_t SizeOfAllocatedData(DataT* data) {
  return SizeOfScopeData<DataT>(data->length);
}

template <typename ConcreteScope>
static void MarkParserScopeData(typename ConcreteScope::ParserData* data,
                                frontend::CompilationState& compilationState) {
  auto names = GetScopeDataTrailingNames(data);
  for (auto& binding : names) {
    auto index = binding.name();
    if (!index) {
      continue;
    }
    compilationState.parserAtoms.markUsedByStencil(
        index, frontend::ParserAtom::Atomize::Yes);
  }
}

template <typename ConcreteScope, typename EnvironmentT>
static void PrepareScopeData(ParserBindingIter& bi,
                             typename ConcreteScope::ParserData* data,
                             uint32_t firstFrameSlot,
                             mozilla::Maybe<uint32_t>* envShape) {
  const JSClass* cls = &EnvironmentT::class_;

  // Iterate through all bindings. This counts the number of environment
  // slots needed and computes the maximum frame slot.
  while (bi) {
    bi++;
  }
  data->slotInfo.nextFrameSlot =
      bi.canHaveFrameSlots() ? bi.nextFrameSlot() : LOCALNO_LIMIT;

  // Make a new environment shape if any environment slots were used.
  if (bi.nextEnvironmentSlot() != JSSLOT_FREE(cls)) {
    envShape->emplace(bi.nextEnvironmentSlot());
  }
}

template <typename ConcreteScope>
static typename ConcreteScope::ParserData* NewEmptyParserScopeData(
    FrontendContext* fc, LifoAlloc& alloc, uint32_t length = 0) {
  using Data = typename ConcreteScope::ParserData;

  size_t dataSize = SizeOfScopeData<Data>(length);
  void* raw = alloc.alloc(dataSize);
  if (!raw) {
    js::ReportOutOfMemory(fc);
    return nullptr;
  }

  return new (raw) Data(length);
}

template <typename ConcreteScope, typename AtomT>
static UniquePtr<AbstractScopeData<ConcreteScope, AtomT>> NewEmptyScopeData(
    JSContext* cx, uint32_t length = 0) {
  using Data = AbstractScopeData<ConcreteScope, AtomT>;

  size_t dataSize = SizeOfScopeData<Data>(length);
  uint8_t* bytes = cx->pod_malloc<uint8_t>(dataSize);
  auto data = reinterpret_cast<Data*>(bytes);
  if (data) {
    new (data) Data(length);
  }
  return UniquePtr<Data>(data);
}

template <typename ConcreteScope>
static UniquePtr<typename ConcreteScope::RuntimeData> LiftParserScopeData(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    BaseParserScopeData* baseData) {
  using ConcreteData = typename ConcreteScope::RuntimeData;

  auto* data = static_cast<typename ConcreteScope::ParserData*>(baseData);

  // Convert all scope ParserAtoms to rooted JSAtoms.
  // Rooting is necessary as conversion can gc.
  JS::RootedVector<JSAtom*> jsatoms(cx);
  if (!jsatoms.reserve(data->length)) {
    return nullptr;
  }
  auto names = GetScopeDataTrailingNames(data);
  for (size_t i = 0; i < names.size(); i++) {
    JSAtom* jsatom = nullptr;
    if (names[i].name()) {
      jsatom = atomCache.getExistingAtomAt(cx, names[i].name());
      MOZ_ASSERT(jsatom);
    }
    jsatoms.infallibleAppend(jsatom);
  }

  // Allocate a new scope-data of the right kind.
  UniquePtr<ConcreteData> scopeData(
      NewEmptyScopeData<ConcreteScope, JSAtom>(cx, data->length));
  if (!scopeData) {
    return nullptr;
  }

  // NOTE: There shouldn't be any fallible operation or GC between setting
  //       `length` and filling `trailingNames`.
  scopeData.get()->length = data->length;

  memcpy(&scopeData.get()->slotInfo, &data->slotInfo,
         sizeof(typename ConcreteScope::SlotInfo));

  // Initialize new scoped names.
  auto namesOut = GetScopeDataTrailingNames(scopeData.get());
  MOZ_ASSERT(data->length == namesOut.size());
  for (size_t i = 0; i < namesOut.size(); i++) {
    namesOut[i] = names[i].copyWithNewAtom(jsatoms[i].get());
  }

  return scopeData;
}

/* static */
Scope* Scope::create(JSContext* cx, ScopeKind kind, Handle<Scope*> enclosing,
                     Handle<SharedShape*> envShape) {
  return cx->newCell<Scope>(kind, enclosing, envShape);
}

template <typename ConcreteScope>
/* static */
ConcreteScope* Scope::create(
    JSContext* cx, ScopeKind kind, Handle<Scope*> enclosing,
    Handle<SharedShape*> envShape,
    MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data) {
  Scope* scope = create(cx, kind, enclosing, envShape);
  if (!scope) {
    return nullptr;
  }

  // It is an invariant that all Scopes that have data (currently, all
  // ScopeKinds except With) must have non-null data.
  MOZ_ASSERT(data);
  scope->initData<ConcreteScope>(data);

  return &scope->as<ConcreteScope>();
}

template <typename ConcreteScope>
inline void Scope::initData(
    MutableHandle<UniquePtr<typename ConcreteScope::RuntimeData>> data) {
  MOZ_ASSERT(!rawData());

  AddCellMemory(this, SizeOfAllocatedData(data.get().get()),
                MemoryUse::ScopeData);

  setHeaderPtr(data.get().release());
}

void Scope::updateEnvShapeIfRequired(mozilla::Maybe<uint32_t>* envShape,
                                     bool needsEnvironment) {
  if (envShape->isNothing() && needsEnvironment) {
    uint32_t numSlots = 0;
    envShape->emplace(numSlots);
  }
}

uint32_t Scope::firstFrameSlot() const {
  switch (kind()) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::FunctionLexical:
      // For intra-frame scopes, find the enclosing scope's next frame slot.
      MOZ_ASSERT(is<LexicalScope>());
      return LexicalScope::nextFrameSlot(enclosing());

    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
      // Named lambda scopes cannot have frame slots.
      return LOCALNO_LIMIT;

    case ScopeKind::ClassBody:
      MOZ_ASSERT(is<ClassBodyScope>());
      return ClassBodyScope::nextFrameSlot(enclosing());

    case ScopeKind::FunctionBodyVar:
      if (enclosing()->is<FunctionScope>()) {
        return enclosing()->as<FunctionScope>().nextFrameSlot();
      }
      break;

    default:
      break;
  }
  return 0;
}

uint32_t Scope::chainLength() const {
  uint32_t length = 0;
  for (ScopeIter si(const_cast<Scope*>(this)); si; si++) {
    length++;
  }
  return length;
}

uint32_t Scope::environmentChainLength() const {
  uint32_t length = 0;
  for (ScopeIter si(const_cast<Scope*>(this)); si; si++) {
    if (si.hasSyntacticEnvironment()) {
      length++;
    }
  }
  return length;
}

void Scope::finalize(JS::GCContext* gcx) {
  MOZ_ASSERT(CurrentThreadIsGCFinalizing());
  applyScopeDataTyped([this, gcx](auto data) {
    gcx->delete_(this, data, SizeOfAllocatedData(data), MemoryUse::ScopeData);
  });
  setHeaderPtr(nullptr);
}

size_t Scope::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  if (rawData()) {
    return mallocSizeOf(rawData());
  }
  return 0;
}

void Scope::dump() {
  JSContext* cx = TlsContext.get();
  if (!cx) {
    fprintf(stderr, "*** can't get JSContext for current thread\n");
    return;
  }
  for (Rooted<ScopeIter> si(cx, ScopeIter(this)); si; si++) {
    fprintf(stderr, "- %s [%p]\n", ScopeKindString(si.kind()), si.scope());
    DumpBindings(cx, si.scope());
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n");
}

#if defined(DEBUG) || defined(JS_JITSPEW)

/* static */
bool Scope::dumpForDisassemble(JSContext* cx, JS::Handle<Scope*> scope,
                               GenericPrinter& out, const char* indent) {
  out.put(ScopeKindString(scope->kind()));
  out.put(" {");

  size_t i = 0;
  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++, i++) {
    if (i == 0) {
      out.put("\n");
    }
    UniqueChars bytes = AtomToPrintableString(cx, bi.name());
    if (!bytes) {
      return false;
    }
    out.put(indent);
    out.printf("  %2zu: %s %s ", i, BindingKindString(bi.kind()), bytes.get());
    switch (bi.location().kind()) {
      case BindingLocation::Kind::Global:
        if (bi.isTopLevelFunction()) {
          out.put("(global function)\n");
        } else {
          out.put("(global)\n");
        }
        break;
      case BindingLocation::Kind::Argument:
        out.printf("(arg slot %u)\n", bi.location().argumentSlot());
        break;
      case BindingLocation::Kind::Frame:
        out.printf("(frame slot %u)\n", bi.location().slot());
        break;
      case BindingLocation::Kind::Environment:
        out.printf("(env slot %u)\n", bi.location().slot());
        break;
      case BindingLocation::Kind::NamedLambdaCallee:
        out.put("(named lambda callee)\n");
        break;
      case BindingLocation::Kind::Import:
        out.put("(import)\n");
        break;
    }
  }
  if (i > 0) {
    out.put(indent);
  }
  out.put("}");

  ScopeIter si(scope);
  si++;
  for (; si; si++) {
    out.put(" -> ");
    out.put(ScopeKindString(si.kind()));
  }
  return true;
}

#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

static uint32_t NextFrameSlot(Scope* scope) {
  for (ScopeIter si(scope); si; si++) {
    switch (si.kind()) {
      case ScopeKind::With:
        continue;

      case ScopeKind::Function:
        return si.scope()->as<FunctionScope>().nextFrameSlot();

      case ScopeKind::FunctionBodyVar:
        return si.scope()->as<VarScope>().nextFrameSlot();

      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::FunctionLexical:
        return si.scope()->as<LexicalScope>().nextFrameSlot();

      case ScopeKind::ClassBody:
        return si.scope()->as<ClassBodyScope>().nextFrameSlot();

      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
        // Named lambda scopes cannot have frame slots.
        return 0;

      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
        return si.scope()->as<EvalScope>().nextFrameSlot();

      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        return 0;

      case ScopeKind::Module:
        return si.scope()->as<ModuleScope>().nextFrameSlot();

      case ScopeKind::WasmInstance:
      case ScopeKind::WasmFunction:
        // Invalid; MOZ_CRASH below.
        break;
    }
  }
  MOZ_CRASH("Not an enclosing intra-frame Scope");
}

/* static */
uint32_t LexicalScope::nextFrameSlot(Scope* scope) {
  return NextFrameSlot(scope);
}

/* static */
uint32_t ClassBodyScope::nextFrameSlot(Scope* scope) {
  return NextFrameSlot(scope);
}

/* static */
void LexicalScope::prepareForScopeCreation(ScopeKind kind,
                                           uint32_t firstFrameSlot,
                                           LexicalScope::ParserData* data,
                                           mozilla::Maybe<uint32_t>* envShape) {
  bool isNamedLambda =
      kind == ScopeKind::NamedLambda || kind == ScopeKind::StrictNamedLambda;

  MOZ_ASSERT_IF(isNamedLambda, firstFrameSlot == LOCALNO_LIMIT);

  ParserBindingIter bi(*data, firstFrameSlot, isNamedLambda);
  PrepareScopeData<LexicalScope, BlockLexicalEnvironmentObject>(
      bi, data, firstFrameSlot, envShape);
}

/* static */
SharedShape* LexicalScope::getEmptyExtensibleEnvironmentShape(JSContext* cx) {
  const JSClass* cls = &LexicalEnvironmentObject::class_;
  return EmptyEnvironmentShape(cx, cls, JSSLOT_FREE(cls), ObjectFlags());
}

/* static */
void ClassBodyScope::prepareForScopeCreation(
    ScopeKind kind, uint32_t firstFrameSlot, ClassBodyScope::ParserData* data,
    mozilla::Maybe<uint32_t>* envShape) {
  MOZ_ASSERT(kind == ScopeKind::ClassBody);

  ParserBindingIter bi(*data, firstFrameSlot);
  PrepareScopeData<ClassBodyScope, BlockLexicalEnvironmentObject>(
      bi, data, firstFrameSlot, envShape);
}

/* static */
void FunctionScope::prepareForScopeCreation(
    FunctionScope::ParserData* data, bool hasParameterExprs,
    bool needsEnvironment, mozilla::Maybe<uint32_t>* envShape) {
  uint32_t firstFrameSlot = 0;
  ParserBindingIter bi(*data, hasParameterExprs);
  PrepareScopeData<FunctionScope, CallObject>(bi, data, firstFrameSlot,
                                              envShape);

  if (hasParameterExprs) {
    data->slotInfo.setHasParameterExprs();
  }

  // An environment may be needed regardless of existence of any closed over
  // bindings:
  //   - Extensible scopes (i.e., due to direct eval)
  //   - Needing a home object
  //   - Being a derived class constructor
  //   - Being a generator or async function
  // Also see |FunctionBox::needsExtraBodyVarEnvironmentRegardlessOfBindings()|.
  updateEnvShapeIfRequired(envShape, needsEnvironment);
}

JSScript* FunctionScope::script() const {
  return canonicalFunction()->nonLazyScript();
}

/* static */
bool FunctionScope::isSpecialName(frontend::TaggedParserAtomIndex name) {
  return name == frontend::TaggedParserAtomIndex::WellKnown::arguments() ||
         name == frontend::TaggedParserAtomIndex::WellKnown::dot_this_() ||
         name == frontend::TaggedParserAtomIndex::WellKnown::dot_newTarget_() ||
         name == frontend::TaggedParserAtomIndex::WellKnown::dot_generator_();
}

/* static */
void VarScope::prepareForScopeCreation(ScopeKind kind,
                                       VarScope::ParserData* data,
                                       uint32_t firstFrameSlot,
                                       bool needsEnvironment,
                                       mozilla::Maybe<uint32_t>* envShape) {
  ParserBindingIter bi(*data, firstFrameSlot);
  PrepareScopeData<VarScope, VarEnvironmentObject>(bi, data, firstFrameSlot,
                                                   envShape);

  // An environment may be needed regardless of existence of any closed over
  // bindings:
  //   - Extensible scopes (i.e., due to direct eval)
  //   - Being a generator
  updateEnvShapeIfRequired(envShape, needsEnvironment);
}

GlobalScope* GlobalScope::createEmpty(JSContext* cx, ScopeKind kind) {
  Rooted<UniquePtr<RuntimeData>> data(
      cx, NewEmptyScopeData<GlobalScope, JSAtom>(cx));
  if (!data) {
    return nullptr;
  }

  return createWithData(cx, kind, &data);
}

/* static */
GlobalScope* GlobalScope::createWithData(
    JSContext* cx, ScopeKind kind, MutableHandle<UniquePtr<RuntimeData>> data) {
  MOZ_ASSERT(data);

  // The global scope has no environment shape. Its environment is the
  // global lexical scope and the global object or non-syntactic objects
  // created by embedding, all of which are not only extensible but may
  // have names on them deleted.
  return Scope::create<GlobalScope>(cx, kind, nullptr, nullptr, data);
}

/* static */
WithScope* WithScope::create(JSContext* cx, Handle<Scope*> enclosing) {
  Scope* scope = Scope::create(cx, ScopeKind::With, enclosing, nullptr);
  return static_cast<WithScope*>(scope);
}

/* static */
void EvalScope::prepareForScopeCreation(ScopeKind scopeKind,
                                        EvalScope::ParserData* data,
                                        mozilla::Maybe<uint32_t>* envShape) {
  if (scopeKind == ScopeKind::StrictEval) {
    uint32_t firstFrameSlot = 0;
    ParserBindingIter bi(*data, true);
    PrepareScopeData<EvalScope, VarEnvironmentObject>(bi, data, firstFrameSlot,
                                                      envShape);
  }
}

/* static */
Scope* EvalScope::nearestVarScopeForDirectEval(Scope* scope) {
  for (ScopeIter si(scope); si; si++) {
    switch (si.kind()) {
      case ScopeKind::Function:
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
        return scope;
      default:
        break;
    }
  }
  return nullptr;
}

ModuleScope::RuntimeData::RuntimeData(size_t length) {
  PoisonNames(this, length);
}

/* static */
void ModuleScope::prepareForScopeCreation(ModuleScope::ParserData* data,
                                          mozilla::Maybe<uint32_t>* envShape) {
  uint32_t firstFrameSlot = 0;
  ParserBindingIter bi(*data);
  PrepareScopeData<ModuleScope, ModuleEnvironmentObject>(
      bi, data, firstFrameSlot, envShape);

  // Modules always need an environment object for now.
  bool needsEnvironment = true;
  updateEnvShapeIfRequired(envShape, needsEnvironment);
}

template <size_t ArrayLength>
static JSAtom* GenerateWasmName(JSContext* cx,
                                const char (&prefix)[ArrayLength],
                                uint32_t index) {
  StringBuilder sb(cx);
  if (!sb.append(prefix)) {
    return nullptr;
  }
  if (!NumberValueToStringBuilder(NumberValue(index), sb)) {
    return nullptr;
  }

  return sb.finishAtom();
}

static void InitializeTrailingName(AbstractBindingName<JSAtom>* trailingNames,
                                   size_t i, JSAtom* name) {
  void* trailingName = &trailingNames[i];
  new (trailingName) BindingName(name, false);
}

template <class DataT>
static void InitializeNextTrailingName(const Rooted<UniquePtr<DataT>>& data,
                                       JSAtom* name) {
  InitializeTrailingName(GetScopeDataTrailingNamesPointer(data.get().get()),
                         data->length, name);
  data->length++;
}

WasmInstanceScope::RuntimeData::RuntimeData(size_t length) {
  PoisonNames(this, length);
}

/* static */
WasmInstanceScope* WasmInstanceScope::create(JSContext* cx,
                                             WasmInstanceObject* instance) {
  size_t namesCount = 0;

  size_t memoriesStart = namesCount;
  size_t memoriesCount = instance->instance().codeMeta().memories.length();
  namesCount += memoriesCount;

  size_t globalsStart = namesCount;
  size_t globalsCount = instance->instance().codeMeta().globals.length();
  namesCount += globalsCount;

  Rooted<UniquePtr<RuntimeData>> data(
      cx, NewEmptyScopeData<WasmInstanceScope, JSAtom>(cx, namesCount));
  if (!data) {
    return nullptr;
  }

  Rooted<WasmInstanceObject*> rootedInstance(cx, instance);
  for (size_t i = 0; i < memoriesCount; i++) {
    JSAtom* wasmName = GenerateWasmName(cx, "memory", i);
    if (!wasmName) {
      return nullptr;
    }

    InitializeNextTrailingName(data, wasmName);
  }

  for (size_t i = 0; i < globalsCount; i++) {
    JSAtom* wasmName = GenerateWasmName(cx, "global", i);
    if (!wasmName) {
      return nullptr;
    }

    InitializeNextTrailingName(data, wasmName);
  }

  MOZ_ASSERT(data->length == namesCount);

  data->instance.init(rootedInstance);
  data->slotInfo.memoriesStart = memoriesStart;
  data->slotInfo.globalsStart = globalsStart;

  Rooted<Scope*> enclosing(cx, &cx->global()->emptyGlobalScope());
  return Scope::create<WasmInstanceScope>(cx, ScopeKind::WasmInstance,
                                          enclosing,
                                          /* envShape = */ nullptr, &data);
}

/* static */
WasmFunctionScope* WasmFunctionScope::create(JSContext* cx,
                                             Handle<Scope*> enclosing,
                                             uint32_t funcIndex) {
  MOZ_ASSERT(enclosing->is<WasmInstanceScope>());

  Rooted<WasmFunctionScope*> wasmFunctionScope(cx);

  Rooted<WasmInstanceObject*> instance(
      cx, enclosing->as<WasmInstanceScope>().instance());

  // TODO pull the local variable names from the wasm function definition.
  wasm::ValTypeVector locals;
  size_t argsLength;
  wasm::StackResults unusedStackResults;
  if (!instance->instance().debug().debugGetLocalTypes(
          funcIndex, &locals, &argsLength, &unusedStackResults)) {
    return nullptr;
  }
  uint32_t namesCount = locals.length();

  Rooted<UniquePtr<RuntimeData>> data(
      cx, NewEmptyScopeData<WasmFunctionScope, JSAtom>(cx, namesCount));
  if (!data) {
    return nullptr;
  }

  for (size_t i = 0; i < namesCount; i++) {
    JSAtom* wasmName = GenerateWasmName(cx, "var", i);
    if (!wasmName) {
      return nullptr;
    }

    InitializeNextTrailingName(data, wasmName);
  }
  MOZ_ASSERT(data->length == namesCount);

  return Scope::create<WasmFunctionScope>(cx, ScopeKind::WasmFunction,
                                          enclosing,
                                          /* envShape = */ nullptr, &data);
}

ScopeIter::ScopeIter(JSScript* script) : scope_(script->bodyScope()) {}

bool ScopeIter::hasSyntacticEnvironment() const {
  return scope()->hasEnvironment() &&
         scope()->kind() != ScopeKind::NonSyntactic;
}

AbstractBindingIter<JSAtom>::AbstractBindingIter(ScopeKind kind,
                                                 BaseScopeData* data,
                                                 uint32_t firstFrameSlot)
    : BaseAbstractBindingIter<JSAtom>() {
  switch (kind) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::FunctionLexical:
      init(*static_cast<LexicalScope::RuntimeData*>(data), firstFrameSlot, 0);
      break;
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
      init(*static_cast<LexicalScope::RuntimeData*>(data), LOCALNO_LIMIT,
           IsNamedLambda);
      break;
    case ScopeKind::ClassBody:
      init(*static_cast<ClassBodyScope::RuntimeData*>(data), firstFrameSlot);
      break;
    case ScopeKind::With:
      // With scopes do not have bindings.
      index_ = length_ = 0;
      MOZ_ASSERT(done());
      break;
    case ScopeKind::Function: {
      uint8_t flags = IgnoreDestructuredFormalParameters;
      if (static_cast<FunctionScope::RuntimeData*>(data)
              ->slotInfo.hasParameterExprs()) {
        flags |= HasFormalParameterExprs;
      }
      init(*static_cast<FunctionScope::RuntimeData*>(data), flags);
      break;
    }
    case ScopeKind::FunctionBodyVar:
      init(*static_cast<VarScope::RuntimeData*>(data), firstFrameSlot);
      break;
    case ScopeKind::Eval:
    case ScopeKind::StrictEval:
      init(*static_cast<EvalScope::RuntimeData*>(data),
           kind == ScopeKind::StrictEval);
      break;
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      init(*static_cast<GlobalScope::RuntimeData*>(data));
      break;
    case ScopeKind::Module:
      init(*static_cast<ModuleScope::RuntimeData*>(data));
      break;
    case ScopeKind::WasmInstance:
      init(*static_cast<WasmInstanceScope::RuntimeData*>(data));
      break;
    case ScopeKind::WasmFunction:
      init(*static_cast<WasmFunctionScope::RuntimeData*>(data));
      break;
  }
}

AbstractBindingIter<JSAtom>::AbstractBindingIter(Scope* scope)
    : AbstractBindingIter<JSAtom>(scope->kind(), scope->rawData(),
                                  scope->firstFrameSlot()) {}

AbstractBindingIter<JSAtom>::AbstractBindingIter(JSScript* script)
    : AbstractBindingIter<JSAtom>(script->bodyScope()) {}

AbstractBindingIter<frontend::TaggedParserAtomIndex>::AbstractBindingIter(
    const frontend::ScopeStencilRef& ref)
    : Base() {
  const ScopeStencil& scope = ref.scope();
  BaseParserScopeData* data = ref.context_.scopeNames[ref.scopeIndex_];
  switch (scope.kind()) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::FunctionLexical:
      init(*static_cast<LexicalScope::ParserData*>(data),
           scope.firstFrameSlot(), 0);
      break;
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
      init(*static_cast<LexicalScope::ParserData*>(data), LOCALNO_LIMIT,
           IsNamedLambda);
      break;
    case ScopeKind::ClassBody:
      init(*static_cast<ClassBodyScope::ParserData*>(data),
           scope.firstFrameSlot());
      break;
    case ScopeKind::With:
      // With scopes do not have bindings.
      index_ = length_ = 0;
      MOZ_ASSERT(done());
      break;
    case ScopeKind::Function: {
      uint8_t flags = IgnoreDestructuredFormalParameters;
      if (static_cast<FunctionScope::ParserData*>(data)
              ->slotInfo.hasParameterExprs()) {
        flags |= HasFormalParameterExprs;
      }
      init(*static_cast<FunctionScope::ParserData*>(data), flags);
      break;
    }
    case ScopeKind::FunctionBodyVar:
      init(*static_cast<VarScope::ParserData*>(data), scope.firstFrameSlot());
      break;
    case ScopeKind::Eval:
    case ScopeKind::StrictEval:
      init(*static_cast<EvalScope::ParserData*>(data),
           scope.kind() == ScopeKind::StrictEval);
      break;
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      init(*static_cast<GlobalScope::ParserData*>(data));
      break;
    case ScopeKind::Module:
      init(*static_cast<ModuleScope::ParserData*>(data));
      break;
    case ScopeKind::WasmInstance:
      init(*static_cast<WasmInstanceScope::ParserData*>(data));
      break;
    case ScopeKind::WasmFunction:
      init(*static_cast<WasmFunctionScope::ParserData*>(data));
      break;
  }
}

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    LexicalScope::AbstractData<NameT>& data, uint32_t firstFrameSlot,
    uint8_t flags) {
  auto& slotInfo = data.slotInfo;

  // Named lambda scopes can only have environment slots. If the callee
  // isn't closed over, it is accessed via JSOp::Callee.
  if (flags & IsNamedLambda) {
    // Named lambda binding is weird. Normal BindingKind ordering rules
    // don't apply.
    init(/* positionalFormalStart= */ 0,
         /* nonPositionalFormalStart= */ 0,
         /* varStart= */ 0,
         /* letStart= */ 0,
         /* constStart= */ 0,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
         /* usingStart= */ data.length,
#endif
         /* syntheticStart= */ data.length,
         /* privageMethodStart= */ data.length,
         /* flags= */ CanHaveEnvironmentSlots | flags,
         /* firstFrameSlot= */ firstFrameSlot,
         /* firstEnvironmentSlot= */
         JSSLOT_FREE(&LexicalEnvironmentObject::class_),
         /* names= */ GetScopeDataTrailingNames(&data));
  } else {
    //            imports - [0, 0)
    // positional formals - [0, 0)
    //      other formals - [0, 0)
    //               vars - [0, 0)
    //               lets - [0, slotInfo.constStart)
    //             consts - [slotInfo.constStart, data.length)
    //          synthetic - [data.length, data.length)
    //    private methods - [data.length, data.length)
    //
    // If ENABLE_EXPLICIT_RESOURCE_MANAGEMENT is set, the consts range is split
    // into the following:
    //             consts - [slotInfo.constStart, slotInfo.usingStart)
    //             usings - [slotInfo.usingStart, data.length)
    init(/* positionalFormalStart= */ 0,
         /* nonPositionalFormalStart= */ 0,
         /* varStart= */ 0,
         /* letStart= */ 0,
         /* constStart= */ slotInfo.constStart,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
         /* usingStart= */ slotInfo.usingStart,
#endif
         /* syntheticStart= */ data.length,
         /* privateMethodStart= */ data.length,
         /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots | flags,
         /* firstFrameSlot= */ firstFrameSlot,
         /* firstEnvironmentSlot= */
         JSSLOT_FREE(&LexicalEnvironmentObject::class_),
         /* names= */ GetScopeDataTrailingNames(&data));
  }
}

template void BaseAbstractBindingIter<JSAtom>::init(
    LexicalScope::AbstractData<JSAtom>&, uint32_t, uint8_t);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    LexicalScope::AbstractData<frontend::TaggedParserAtomIndex>&, uint32_t,
    uint8_t);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    ClassBodyScope::AbstractData<NameT>& data, uint32_t firstFrameSlot) {
  auto& slotInfo = data.slotInfo;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, 0)
  //               lets - [0, 0)
  //             consts - [0, 0)
  //          synthetic - [0, slotInfo.privateMethodStart)
  //    private methods - [slotInfo.privateMethodStart, data.length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ 0,
       /* varStart= */ 0,
       /* letStart= */ 0,
       /* constStart= */ 0,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ 0,
#endif
       /* syntheticStart= */ 0,
       /* privateMethodStart= */ slotInfo.privateMethodStart,
       /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots,
       /* firstFrameSlot= */ firstFrameSlot,
       /* firstEnvironmentSlot= */
       JSSLOT_FREE(&ClassBodyLexicalEnvironmentObject::class_),
       /* names= */ GetScopeDataTrailingNames(&data));
}

template void BaseAbstractBindingIter<JSAtom>::init(
    ClassBodyScope::AbstractData<JSAtom>&, uint32_t);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    ClassBodyScope::AbstractData<frontend::TaggedParserAtomIndex>&, uint32_t);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    FunctionScope::AbstractData<NameT>& data, uint8_t flags) {
  flags = CanHaveFrameSlots | CanHaveEnvironmentSlots | flags;
  if (!(flags & HasFormalParameterExprs)) {
    flags |= CanHaveArgumentSlots;
  }

  auto length = data.length;
  auto& slotInfo = data.slotInfo;

  //            imports - [0, 0)
  // positional formals - [0, slotInfo.nonPositionalFormalStart)
  //      other formals - [slotInfo.nonPositionalParamStart, slotInfo.varStart)
  //               vars - [slotInfo.varStart, length)
  //               lets - [length, length)
  //             consts - [length, length)
  //          synthetic - [length, length)
  //    private methods - [length, length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ slotInfo.nonPositionalFormalStart,
       /* varStart= */ slotInfo.varStart,
       /* letStart= */ length,
       /* constStart= */ length,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ length,
#endif
       /* syntheticStart= */ length,
       /* privateMethodStart= */ length,
       /* flags= */ flags,
       /* firstFrameSlot= */ 0,
       /* firstEnvironmentSlot= */ JSSLOT_FREE(&CallObject::class_),
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    FunctionScope::AbstractData<JSAtom>&, uint8_t);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    FunctionScope::AbstractData<frontend::TaggedParserAtomIndex>&, uint8_t);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(VarScope::AbstractData<NameT>& data,
                                          uint32_t firstFrameSlot) {
  auto length = data.length;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, length)
  //               lets - [length, length)
  //             consts - [length, length)
  //          synthetic - [length, length)
  //    private methods - [length, length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ 0,
       /* varStart= */ 0,
       /* letStart= */ length,
       /* constStart= */ length,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ length,
#endif
       /* syntheticStart= */ length,
       /* privateMethodStart= */ length,
       /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots,
       /* firstFrameSlot= */ firstFrameSlot,
       /* firstEnvironmentSlot= */ JSSLOT_FREE(&VarEnvironmentObject::class_),
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    VarScope::AbstractData<JSAtom>&, uint32_t);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    VarScope::AbstractData<frontend::TaggedParserAtomIndex>&, uint32_t);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    GlobalScope::AbstractData<NameT>& data) {
  auto& slotInfo = data.slotInfo;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, slotInfo.letStart)
  //               lets - [slotInfo.letStart, slotInfo.constStart)
  //             consts - [slotInfo.constStart, data.length)
  //          synthetic - [data.length, data.length)
  //    private methods - [data.length, data.length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ 0,
       /* varStart= */ 0,
       /* letStart= */ slotInfo.letStart,
       /* constStart= */ slotInfo.constStart,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ data.length,
#endif
       /* syntheticStart= */ data.length,
       /* privateMethoodStart= */ data.length,
       /* flags= */ CannotHaveSlots,
       /* firstFrameSlot= */ UINT32_MAX,
       /* firstEnvironmentSlot= */ UINT32_MAX,
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    GlobalScope::AbstractData<JSAtom>&);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    GlobalScope::AbstractData<frontend::TaggedParserAtomIndex>&);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(EvalScope::AbstractData<NameT>& data,
                                          bool strict) {
  uint32_t flags;
  uint32_t firstFrameSlot;
  uint32_t firstEnvironmentSlot;
  if (strict) {
    flags = CanHaveFrameSlots | CanHaveEnvironmentSlots;
    firstFrameSlot = 0;
    firstEnvironmentSlot = JSSLOT_FREE(&VarEnvironmentObject::class_);
  } else {
    flags = CannotHaveSlots;
    firstFrameSlot = UINT32_MAX;
    firstEnvironmentSlot = UINT32_MAX;
  }

  auto length = data.length;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, length)
  //               lets - [length, length)
  //             consts - [length, length)
  //          synthetic - [length, length)
  //    private methods - [length, length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ 0,
       /* varStart= */ 0,
       /* letStart= */ length,
       /* constStart= */ length,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ length,
#endif
       /* syntheticStart= */ length,
       /* privateMethodStart= */ length,
       /* flags= */ flags,
       /* firstFrameSlot= */ firstFrameSlot,
       /* firstEnvironmentSlot= */ firstEnvironmentSlot,
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    EvalScope::AbstractData<JSAtom>&, bool);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    EvalScope::AbstractData<frontend::TaggedParserAtomIndex>&, bool);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    ModuleScope::AbstractData<NameT>& data) {
  auto& slotInfo = data.slotInfo;

  //            imports - [0, slotInfo.varStart)
  // positional formals - [slotInfo.varStart, slotInfo.varStart)
  //      other formals - [slotInfo.varStart, slotInfo.varStart)
  //               vars - [slotInfo.varStart, slotInfo.letStart)
  //               lets - [slotInfo.letStart, slotInfo.constStart)
  //             consts - [slotInfo.constStart, data.length)
  //          synthetic - [data.length, data.length)
  //    private methods - [data.length, data.length)
  //
  // If ENABLE_EXPLICIT_RESOURCE_MANAGEMENT is set, the consts range is split
  // into the following:
  //             consts - [slotInfo.constStart, slotInfo.usingStart)
  //             usings - [slotInfo.usingStart, data.length)
  init(
      /* positionalFormalStart= */ slotInfo.varStart,
      /* nonPositionalFormalStart= */ slotInfo.varStart,
      /* varStart= */ slotInfo.varStart,
      /* letStart= */ slotInfo.letStart,
      /* constStart= */ slotInfo.constStart,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      /* usingStart= */ slotInfo.usingStart,
#endif
      /* syntheticStart= */ data.length,
      /* privateMethodStart= */ data.length,
      /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots,
      /* firstFrameSlot= */ 0,
      /* firstEnvironmentSlot= */ JSSLOT_FREE(&ModuleEnvironmentObject::class_),
      /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    ModuleScope::AbstractData<JSAtom>&);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    ModuleScope::AbstractData<frontend::TaggedParserAtomIndex>&);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    WasmInstanceScope::AbstractData<NameT>& data) {
  auto length = data.length;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, length)
  //               lets - [length, length)
  //             consts - [length, length)
  //          synthetic - [length, length)
  //    private methods - [length, length)
  init(/* positionalFormalStart= */ 0,
       /* nonPositionalFormalStart= */ 0,
       /* varStart= */ 0,
       /* letStart= */ length,
       /* constStart= */ length,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ length,
#endif
       /* syntheticStart= */ length,
       /* privateMethodStart= */ length,
       /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots,
       /* firstFrameSlot= */ UINT32_MAX,
       /* firstEnvironmentSlot= */ UINT32_MAX,
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    WasmInstanceScope::AbstractData<JSAtom>&);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    WasmInstanceScope::AbstractData<frontend::TaggedParserAtomIndex>&);

template <typename NameT>
void BaseAbstractBindingIter<NameT>::init(
    WasmFunctionScope::AbstractData<NameT>& data) {
  auto length = data.length;

  //            imports - [0, 0)
  // positional formals - [0, 0)
  //      other formals - [0, 0)
  //               vars - [0, length)
  //               lets - [length, length)
  //             consts - [length, length)
  //          synthetic - [length, length)
  //    private methods - [length, length)
  init(/* positionalFormalStart = */ 0,
       /* nonPositionalFormalStart = */ 0,
       /* varStart= */ 0,
       /* letStart= */ length,
       /* constStart= */ length,
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
       /* usingStart= */ length,
#endif
       /* syntheticStart= */ length,
       /* privateMethodStart= */ length,
       /* flags= */ CanHaveFrameSlots | CanHaveEnvironmentSlots,
       /* firstFrameSlot= */ UINT32_MAX,
       /* firstEnvironmentSlot= */ UINT32_MAX,
       /* names= */ GetScopeDataTrailingNames(&data));
}
template void BaseAbstractBindingIter<JSAtom>::init(
    WasmFunctionScope::AbstractData<JSAtom>&);
template void BaseAbstractBindingIter<frontend::TaggedParserAtomIndex>::init(
    WasmFunctionScope::AbstractData<frontend::TaggedParserAtomIndex>&);

AbstractPositionalFormalParameterIter<
    JSAtom>::AbstractPositionalFormalParameterIter(Scope* scope)
    : Base(scope) {
  // Reinit with flags = 0, i.e., iterate over all positional parameters.
  if (scope->is<FunctionScope>()) {
    init(scope->as<FunctionScope>().data(), /* flags = */ 0);
  }
  settle();
}

AbstractPositionalFormalParameterIter<
    JSAtom>::AbstractPositionalFormalParameterIter(JSScript* script)
    : AbstractPositionalFormalParameterIter(script->bodyScope()) {}

void js::DumpBindings(JSContext* cx, Scope* scopeArg) {
  Rooted<Scope*> scope(cx, scopeArg);
  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
    UniqueChars bytes = AtomToPrintableString(cx, bi.name());
    if (!bytes) {
      MaybePrintAndClearPendingException(cx);
      return;
    }
    fprintf(stderr, "    %s %s ", BindingKindString(bi.kind()), bytes.get());
    switch (bi.location().kind()) {
      case BindingLocation::Kind::Global:
        if (bi.isTopLevelFunction()) {
          fprintf(stderr, "global function\n");
        } else {
          fprintf(stderr, "global\n");
        }
        break;
      case BindingLocation::Kind::Argument:
        fprintf(stderr, "arg slot %u\n", bi.location().argumentSlot());
        break;
      case BindingLocation::Kind::Frame:
        fprintf(stderr, "frame slot %u\n", bi.location().slot());
        break;
      case BindingLocation::Kind::Environment:
        fprintf(stderr, "env slot %u\n", bi.location().slot());
        break;
      case BindingLocation::Kind::NamedLambdaCallee:
        fprintf(stderr, "named lambda callee\n");
        break;
      case BindingLocation::Kind::Import:
        fprintf(stderr, "import\n");
        break;
    }
  }
}

static JSAtom* GetFrameSlotNameInScope(Scope* scope, uint32_t slot) {
  for (BindingIter bi(scope); bi; bi++) {
    BindingLocation loc = bi.location();
    if (loc.kind() == BindingLocation::Kind::Frame && loc.slot() == slot) {
      return bi.name();
    }
  }
  return nullptr;
}

JSAtom* js::FrameSlotName(JSScript* script, jsbytecode* pc) {
  MOZ_ASSERT(IsLocalOp(JSOp(*pc)));
  uint32_t slot = GET_LOCALNO(pc);
  MOZ_ASSERT(slot < script->nfixed());

  // Look for it in the body scope first.
  if (JSAtom* name = GetFrameSlotNameInScope(script->bodyScope(), slot)) {
    return name;
  }

  // If this is a function script and there is an extra var scope, look for
  // it there.
  if (script->functionHasExtraBodyVarScope()) {
    if (JSAtom* name = GetFrameSlotNameInScope(
            script->functionExtraBodyVarScope(), slot)) {
      return name;
    }
  }
  // If not found, look for it in a lexical scope.
  for (ScopeIter si(script->innermostScope(pc)); si; si++) {
    if (!si.scope()->is<LexicalScope>() && !si.scope()->is<ClassBodyScope>()) {
      continue;
    }

    // Is the slot within bounds of the current lexical scope?
    if (slot < si.scope()->firstFrameSlot()) {
      continue;
    }
    if (slot >= LexicalScope::nextFrameSlot(si.scope())) {
      break;
    }

    // If so, get the name.
    if (JSAtom* name = GetFrameSlotNameInScope(si.scope(), slot)) {
      return name;
    }
  }

  MOZ_CRASH("Frame slot not found");
}

JS::ubi::Node::Size JS::ubi::Concrete<Scope>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return js::gc::Arena::thingSize(get().asTenured().getAllocKind()) +
         get().sizeOfExcludingThis(mallocSizeOf);
}

template <typename... Args>
/* static */ bool ScopeStencil::appendScopeStencilAndData(
    FrontendContext* fc, CompilationState& compilationState,
    BaseParserScopeData* data, ScopeIndex* indexOut, Args&&... args) {
  *indexOut = ScopeIndex(compilationState.scopeData.length());
  if (uint32_t(*indexOut) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(fc);
    return false;
  }

  if (!compilationState.scopeData.emplaceBack(std::forward<Args>(args)...)) {
    js::ReportOutOfMemory(fc);
    return false;
  }
  if (!compilationState.scopeNames.append(data)) {
    compilationState.scopeData.popBack();
    MOZ_ASSERT(compilationState.scopeData.length() ==
               compilationState.scopeNames.length());

    js::ReportOutOfMemory(fc);
    return false;
  }

  return true;
}

/* static */
bool ScopeStencil::createForFunctionScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    FunctionScope::ParserData* data, bool hasParameterExprs,
    bool needsEnvironment, ScriptIndex functionIndex, bool isArrow,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  auto kind = ScopeKind::Function;
  using ScopeType = FunctionScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  FunctionScope::prepareForScopeCreation(data, hasParameterExprs,
                                         needsEnvironment, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape,
                                   mozilla::Some(functionIndex), isArrow);
}

/* static */
bool ScopeStencil::createForLexicalScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ScopeKind kind, LexicalScope::ParserData* data, uint32_t firstFrameSlot,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = LexicalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  ScopeType::prepareForScopeCreation(kind, firstFrameSlot, data, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForClassBodyScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ScopeKind kind, ClassBodyScope::ParserData* data, uint32_t firstFrameSlot,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = ClassBodyScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  ScopeType::prepareForScopeCreation(kind, firstFrameSlot, data, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

bool ScopeStencil::createForVarScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ScopeKind kind, VarScope::ParserData* data, uint32_t firstFrameSlot,
    bool needsEnvironment, mozilla::Maybe<ScopeIndex> enclosing,
    ScopeIndex* index) {
  using ScopeType = VarScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  VarScope::prepareForScopeCreation(kind, data, firstFrameSlot,
                                    needsEnvironment, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForGlobalScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ScopeKind kind, GlobalScope::ParserData* data, ScopeIndex* index) {
  using ScopeType = GlobalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  // The global scope has no environment shape. Its environment is the
  // global lexical scope and the global object or non-syntactic objects
  // created by embedding, all of which are not only extensible but may
  // have names on them deleted.
  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;

  mozilla::Maybe<ScopeIndex> enclosing;

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForEvalScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ScopeKind kind, EvalScope::ParserData* data,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = EvalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  EvalScope::prepareForScopeCreation(kind, data, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForModuleScope(
    FrontendContext* fc, frontend::CompilationState& compilationState,
    ModuleScope::ParserData* data, mozilla::Maybe<ScopeIndex> enclosing,
    ScopeIndex* index) {
  auto kind = ScopeKind::Module;
  using ScopeType = ModuleScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(fc, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  MOZ_ASSERT(enclosing.isNothing());

  // The data that's passed in is from the frontend and is LifoAlloc'd.
  // Copy it now that we're creating a permanent VM scope.
  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  ModuleScope::prepareForScopeCreation(data, &envShape);

  return appendScopeStencilAndData(fc, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

template <typename SpecificEnvironmentT>
bool ScopeStencil::createSpecificShape(
    JSContext* cx, ScopeKind kind, BaseScopeData* scopeData,
    MutableHandle<SharedShape*> shape) const {
  const JSClass* cls = &SpecificEnvironmentT::class_;
  constexpr ObjectFlags objectFlags = SpecificEnvironmentT::OBJECT_FLAGS;

  if (hasEnvironmentShape()) {
    if (numEnvironmentSlots() > 0) {
      BindingIter bi(kind, scopeData, firstFrameSlot_);
      shape.set(CreateEnvironmentShape(cx, bi, cls, numEnvironmentSlots(),
                                       objectFlags));
      return shape;
    }

    shape.set(EmptyEnvironmentShape(cx, cls, JSSLOT_FREE(cls), objectFlags));
    return shape;
  }

  return true;
}

/* static */
bool ScopeStencil::createForWithScope(FrontendContext* fc,
                                      CompilationState& compilationState,
                                      mozilla::Maybe<ScopeIndex> enclosing,
                                      ScopeIndex* index) {
  auto kind = ScopeKind::With;
  MOZ_ASSERT(matchScopeKind<WithScope>(kind));

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;

  return appendScopeStencilAndData(fc, compilationState, nullptr, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

template <typename SpecificScopeT>
UniquePtr<typename SpecificScopeT::RuntimeData>
ScopeStencil::createSpecificScopeData(JSContext* cx,
                                      CompilationAtomCache& atomCache,
                                      BaseParserScopeData* baseData) const {
  return LiftParserScopeData<SpecificScopeT>(cx, atomCache, baseData);
}

template <>
UniquePtr<FunctionScope::RuntimeData>
ScopeStencil::createSpecificScopeData<FunctionScope>(
    JSContext* cx, CompilationAtomCache& atomCache,
    BaseParserScopeData* baseData) const {
  // Allocate a new vm function-scope.
  UniquePtr<FunctionScope::RuntimeData> data =
      LiftParserScopeData<FunctionScope>(cx, atomCache, baseData);
  if (!data) {
    return nullptr;
  }

  return data;
}

template <>
UniquePtr<ModuleScope::RuntimeData>
ScopeStencil::createSpecificScopeData<ModuleScope>(
    JSContext* cx, CompilationAtomCache& atomCache,
    BaseParserScopeData* baseData) const {
  // Allocate a new vm module-scope.
  UniquePtr<ModuleScope::RuntimeData> data =
      LiftParserScopeData<ModuleScope>(cx, atomCache, baseData);
  if (!data) {
    return nullptr;
  }

  return data;
}

// WithScope does not use binding data.
template <>
Scope* ScopeStencil::createSpecificScope<WithScope, std::nullptr_t>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const {
  return Scope::create(cx, ScopeKind::With, enclosingScope, nullptr);
}

// GlobalScope has bindings but no environment shape.
template <>
Scope* ScopeStencil::createSpecificScope<GlobalScope, std::nullptr_t>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const {
  Rooted<UniquePtr<GlobalScope::RuntimeData>> rootedData(
      cx, createSpecificScopeData<GlobalScope>(cx, atomCache, baseData));
  if (!rootedData) {
    return nullptr;
  }

  MOZ_ASSERT(!hasEnclosing());
  MOZ_ASSERT(!enclosingScope);

  // Because we already baked the data here, we needn't do it again.
  return Scope::create<GlobalScope>(cx, kind(), nullptr, nullptr, &rootedData);
}

template <typename SpecificScopeT, typename SpecificEnvironmentT>
Scope* ScopeStencil::createSpecificScope(JSContext* cx,
                                         CompilationAtomCache& atomCache,
                                         Handle<Scope*> enclosingScope,
                                         BaseParserScopeData* baseData) const {
  Rooted<UniquePtr<typename SpecificScopeT::RuntimeData>> rootedData(
      cx, createSpecificScopeData<SpecificScopeT>(cx, atomCache, baseData));
  if (!rootedData) {
    return nullptr;
  }

  Rooted<SharedShape*> shape(cx);
  if (!createSpecificShape<SpecificEnvironmentT>(
          cx, kind(), rootedData.get().get(), &shape)) {
    return nullptr;
  }

  // Because we already baked the data here, we needn't do it again.
  return Scope::create<SpecificScopeT>(cx, kind(), enclosingScope, shape,
                                       &rootedData);
}

template Scope* ScopeStencil::createSpecificScope<FunctionScope, CallObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<LexicalScope, BlockLexicalEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
template Scope* ScopeStencil::createSpecificScope<
    ClassBodyScope, BlockLexicalEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<EvalScope, VarEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<VarScope, VarEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<ModuleScope, ModuleEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<Scope*> enclosingScope, BaseParserScopeData* baseData) const;
