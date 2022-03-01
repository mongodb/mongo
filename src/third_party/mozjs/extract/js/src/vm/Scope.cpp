/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Scope.h"

#include "mozilla/OperatorNewExtensions.h"  // mozilla::KnownNotNull
#include "mozilla/ScopeExit.h"

#include <memory>
#include <new>

#include "builtin/ModuleObject.h"
#include "frontend/CompilationStencil.h"  // CompilationState, CompilationAtomCache
#include "frontend/Parser.h"              // Copy*ScopeData
#include "frontend/ScriptIndex.h"         // ScriptIndex
#include "frontend/SharedContext.h"
#include "frontend/Stencil.h"
#include "gc/Allocator.h"
#include "gc/MaybeRooted.h"
#include "util/StringBuffer.h"
#include "vm/EnvironmentObject.h"
#include "vm/ErrorReporting.h"  // MaybePrintAndClearPendingException
#include "vm/JSScript.h"
#include "wasm/WasmInstance.h"

#include "gc/FreeOp-inl.h"
#include "gc/ObjectKind-inl.h"
#include "vm/Shape-inl.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

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

Shape* js::EmptyEnvironmentShape(JSContext* cx, const JSClass* cls,
                                 uint32_t numSlots, ObjectFlags objectFlags) {
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

Shape* js::CreateEnvironmentShape(JSContext* cx, BindingIter& bi,
                                  const JSClass* cls, uint32_t numSlots,
                                  ObjectFlags objectFlags) {
  Rooted<SharedPropMap*> map(cx);
  uint32_t mapLength = 0;

  RootedId id(cx);
  for (; bi; bi++) {
    BindingLocation loc = bi.location();
    if (loc.kind() == BindingLocation::Kind::Environment) {
      JSAtom* name = bi.name();
      cx->markAtom(name);
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

Shape* js::CreateEnvironmentShape(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    AbstractBindingIter<frontend::TaggedParserAtomIndex>& bi,
    const JSClass* cls, uint32_t numSlots, ObjectFlags objectFlags) {
  Rooted<SharedPropMap*> map(cx);
  uint32_t mapLength = 0;

  RootedId id(cx);
  for (; bi; bi++) {
    BindingLocation loc = bi.location();
    if (loc.kind() == BindingLocation::Kind::Environment) {
      JSAtom* name = atomCache.getExistingAtomAt(cx, bi.name());
      MOZ_ASSERT(name);
      cx->markAtom(name);
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

template <class DataT>
inline size_t SizeOfAllocatedData(DataT* data) {
  return SizeOfScopeData<DataT>(data->length);
}

template <typename ConcreteScope>
static UniquePtr<typename ConcreteScope::RuntimeData> CopyScopeData(
    JSContext* cx, typename ConcreteScope::RuntimeData* data) {
  using Data = typename ConcreteScope::RuntimeData;

  // Make sure the binding names are marked in the context's zone, if we are
  // copying data from another zone.
  auto names = GetScopeDataTrailingNames(data);
  for (auto binding : names) {
    if (JSAtom* name = binding.name()) {
      cx->markAtom(name);
    }
  }

  size_t size = SizeOfAllocatedData(data);
  void* bytes = cx->pod_malloc<char>(size);
  if (!bytes) {
    return nullptr;
  }

  auto* dataCopy = new (bytes) Data(*data);

  std::uninitialized_copy_n(GetScopeDataTrailingNamesPointer(data),
                            data->length,
                            GetScopeDataTrailingNamesPointer(dataCopy));

  return UniquePtr<Data>(dataCopy);
}

template <typename ConcreteScope>
static void MarkParserScopeData(JSContext* cx,
                                typename ConcreteScope::ParserData* data,
                                frontend::CompilationState& compilationState) {
  auto names = GetScopeDataTrailingNames(data);
  for (auto& binding : names) {
    auto index = binding.name();
    if (!index) {
      continue;
    }
    compilationState.parserAtoms.markUsedByStencil(index);
  }
}

static bool SetEnvironmentShape(JSContext* cx, BindingIter& freshBi,
                                BindingIter& bi, const JSClass* cls,
                                uint32_t firstFrameSlot,
                                ObjectFlags objectFlags,
                                MutableHandleShape envShape) {
  envShape.set(CreateEnvironmentShape(cx, freshBi, cls,
                                      bi.nextEnvironmentSlot(), objectFlags));
  return envShape;
}

static bool SetEnvironmentShape(JSContext* cx, ParserBindingIter& freshBi,
                                ParserBindingIter& bi, const JSClass* cls,
                                uint32_t firstFrameSlot,
                                ObjectFlags objectFlags,
                                mozilla::Maybe<uint32_t>* envShape) {
  envShape->emplace(bi.nextEnvironmentSlot());
  return true;
}

template <typename ConcreteScope, typename AtomT, typename EnvironmentT,
          typename ShapeT>
static bool PrepareScopeData(
    JSContext* cx, AbstractBindingIter<AtomT>& bi,
    typename MaybeRootedScopeData<ConcreteScope, AtomT>::HandleType data,
    uint32_t firstFrameSlot, ShapeT envShape) {
  const JSClass* cls = &EnvironmentT::class_;
  constexpr ObjectFlags objectFlags = EnvironmentT::OBJECT_FLAGS;

  // Copy a fresh BindingIter for use below.
  AbstractBindingIter<AtomT> freshBi(bi);

  // Iterate through all bindings. This counts the number of environment
  // slots needed and computes the maximum frame slot.
  while (bi) {
    bi++;
  }
  data->slotInfo.nextFrameSlot =
      bi.canHaveFrameSlots() ? bi.nextFrameSlot() : LOCALNO_LIMIT;

  // Data is not used after this point.  Before this point, gc cannot
  // occur, so `data` is fine as a raw pointer.

  // Make a new environment shape if any environment slots were used.
  if (bi.nextEnvironmentSlot() != JSSLOT_FREE(cls)) {
    if (!SetEnvironmentShape(cx, freshBi, bi, cls, firstFrameSlot, objectFlags,
                             envShape)) {
      return false;
    }
  }

  return true;
}

template <typename ConcreteScope>
static typename ConcreteScope::ParserData* NewEmptyParserScopeData(
    JSContext* cx, LifoAlloc& alloc, uint32_t length = 0) {
  using Data = typename ConcreteScope::ParserData;

  size_t dataSize = SizeOfScopeData<Data>(length);
  void* raw = alloc.alloc(dataSize);
  if (!raw) {
    js::ReportOutOfMemory(cx);
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

static constexpr size_t HasAtomMask = 1;
static constexpr size_t HasAtomShift = 1;

static XDRResult XDRTrailingName(XDRState<XDR_ENCODE>* xdr,
                                 BindingName* bindingName,
                                 const uint32_t* length) {
  JSContext* cx = xdr->cx();

  RootedAtom atom(cx, bindingName->name());
  bool hasAtom = !!atom;

  uint8_t flags = bindingName->flagsForXDR();
  MOZ_ASSERT(((flags << HasAtomShift) >> HasAtomShift) == flags);
  uint8_t u8 = (flags << HasAtomShift) | uint8_t(hasAtom);
  MOZ_TRY(xdr->codeUint8(&u8));

  if (hasAtom) {
    MOZ_TRY(XDRAtom(xdr, &atom));
  }

  return Ok();
}

static XDRResult XDRTrailingName(XDRState<XDR_DECODE>* xdr, void* bindingName,
                                 uint32_t* length) {
  JSContext* cx = xdr->cx();

  uint8_t u8;
  MOZ_TRY(xdr->codeUint8(&u8));

  bool hasAtom = u8 & HasAtomMask;
  RootedAtom atom(cx);
  if (hasAtom) {
    MOZ_TRY(XDRAtom(xdr, &atom));
  }

  uint8_t flags = u8 >> HasAtomShift;
  new (bindingName) BindingName(BindingName::fromXDR(atom, flags));
  ++*length;

  return Ok();
}

template <typename ConcreteScope, XDRMode mode>
/* static */
XDRResult Scope::XDRSizedBindingNames(
    XDRState<mode>* xdr, Handle<ConcreteScope*> scope,
    MutableHandle<typename ConcreteScope::RuntimeData*> data) {
  MOZ_ASSERT(!data);

  JSContext* cx = xdr->cx();

  uint32_t length;
  if (mode == XDR_ENCODE) {
    length = scope->data().length;
  }
  MOZ_TRY(xdr->codeUint32(&length));

  if (mode == XDR_ENCODE) {
    data.set(&scope->data());
  } else {
    data.set(NewEmptyScopeData<ConcreteScope, JSAtom>(cx, length).release());
    if (!data) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  auto dataGuard = mozilla::MakeScopeExit([&]() {
    if (mode == XDR_DECODE) {
      js_delete(data.get());
      data.set(nullptr);
    }
  });

  BindingName* names = GetScopeDataTrailingNamesPointer(data.get());
  for (uint32_t i = 0; i < length; i++) {
    if (mode == XDR_DECODE) {
      MOZ_ASSERT(i == data->length, "must be decoding at the end");
    }
    MOZ_TRY(XDRTrailingName(xdr, &names[i], &data->length));
  }

  dataGuard.release();
  return Ok();
}

/* static */
Scope* Scope::create(JSContext* cx, ScopeKind kind, HandleScope enclosing,
                     HandleShape envShape) {
  Scope* scope = Allocate<Scope>(cx);
  if (scope) {
    new (scope) Scope(kind, enclosing, envShape);
  }
  return scope;
}

template <typename ConcreteScope>
/* static */
ConcreteScope* Scope::create(
    JSContext* cx, ScopeKind kind, HandleScope enclosing, HandleShape envShape,
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

template <typename EnvironmentT>
bool Scope::updateEnvShapeIfRequired(JSContext* cx, MutableHandleShape envShape,
                                     bool needsEnvironment) {
  if (!envShape && needsEnvironment) {
    envShape.set(EmptyEnvironmentShape<EnvironmentT>(cx));
    if (!envShape) {
      return false;
    }
  }
  return true;
}

template <typename EnvironmentT>
bool Scope::updateEnvShapeIfRequired(JSContext* cx,
                                     mozilla::Maybe<uint32_t>* envShape,
                                     bool needsEnvironment) {
  if (envShape->isNothing() && needsEnvironment) {
    uint32_t numSlots = 0;
    envShape->emplace(numSlots);
  }
  return true;
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

Shape* Scope::maybeCloneEnvironmentShape(JSContext* cx) {
  // Clone the environment shape if cloning into a different realm.
  Shape* shape = environmentShape();
  if (shape && shape->realm() != cx->realm()) {
    BindingIter bi(this);
    return CreateEnvironmentShape(cx, bi, shape->getObjectClass(),
                                  shape->slotSpan(), shape->objectFlags());
  }
  return shape;
}

/* static */
Scope* Scope::clone(JSContext* cx, HandleScope scope, HandleScope enclosing) {
  RootedShape envShape(cx);
  if (scope->environmentShape()) {
    envShape = scope->maybeCloneEnvironmentShape(cx);
    if (!envShape) {
      return nullptr;
    }
  }

  switch (scope->kind_) {
    case ScopeKind::Function: {
      RootedScript script(cx, scope->as<FunctionScope>().script());
      const char* filename = script->filename();
      // If the script has an internal URL, include it in the crash reason. If
      // not, it may be a web URL, and therefore privacy-sensitive.
      if (!strncmp(filename, "chrome:", 7) ||
          !strncmp(filename, "resource:", 9)) {
        MOZ_CRASH_UNSAFE_PRINTF("Use FunctionScope::clone (script URL: %s)",
                                filename);
      }

      MOZ_CRASH("Use FunctionScope::clone.");
      break;
    }

    case ScopeKind::FunctionBodyVar: {
      Rooted<UniquePtr<VarScope::RuntimeData>> dataClone(cx);
      dataClone = CopyScopeData<VarScope>(cx, &scope->as<VarScope>().data());
      if (!dataClone) {
        return nullptr;
      }
      return create<VarScope>(cx, scope->kind_, enclosing, envShape,
                              &dataClone);
    }

    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical: {
      Rooted<UniquePtr<LexicalScope::RuntimeData>> dataClone(cx);
      dataClone =
          CopyScopeData<LexicalScope>(cx, &scope->as<LexicalScope>().data());
      if (!dataClone) {
        return nullptr;
      }
      return create<LexicalScope>(cx, scope->kind_, enclosing, envShape,
                                  &dataClone);
    }

    case ScopeKind::ClassBody: {
      Rooted<UniquePtr<ClassBodyScope::RuntimeData>> dataClone(cx);
      dataClone = CopyScopeData<ClassBodyScope>(
          cx, &scope->as<ClassBodyScope>().data());
      if (!dataClone) {
        return nullptr;
      }
      return create<ClassBodyScope>(cx, scope->kind_, enclosing, envShape,
                                    &dataClone);
    }

    case ScopeKind::With:
      return create(cx, scope->kind_, enclosing, envShape);

    case ScopeKind::Eval:
    case ScopeKind::StrictEval: {
      Rooted<UniquePtr<EvalScope::RuntimeData>> dataClone(cx);
      dataClone = CopyScopeData<EvalScope>(cx, &scope->as<EvalScope>().data());
      if (!dataClone) {
        return nullptr;
      }
      return create<EvalScope>(cx, scope->kind_, enclosing, envShape,
                               &dataClone);
    }

    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      MOZ_CRASH("Use GlobalScope::clone.");
      break;

    case ScopeKind::WasmFunction:
      MOZ_CRASH("wasm functions are not nested in JSScript");
      break;

    case ScopeKind::Module:
    case ScopeKind::WasmInstance:
      MOZ_CRASH("NYI");
      break;
  }

  return nullptr;
}

void Scope::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(CurrentThreadIsGCFinalizing());
  applyScopeDataTyped([this, fop](auto data) {
    fop->delete_(this, data, SizeOfAllocatedData(data), MemoryUse::ScopeData);
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
  if (!out.put(ScopeKindString(scope->kind()))) {
    return false;
  }
  if (!out.put(" {")) {
    return false;
  }

  size_t i = 0;
  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++, i++) {
    if (i == 0) {
      if (!out.put("\n")) {
        return false;
      }
    }
    UniqueChars bytes = AtomToPrintableString(cx, bi.name());
    if (!bytes) {
      return false;
    }
    if (!out.put(indent)) {
      return false;
    }
    if (!out.printf("  %2zu: %s %s ", i, BindingKindString(bi.kind()),
                    bytes.get())) {
      return false;
    }
    switch (bi.location().kind()) {
      case BindingLocation::Kind::Global:
        if (bi.isTopLevelFunction()) {
          if (!out.put("(global function)\n")) {
            return false;
          }
        } else {
          if (!out.put("(global)\n")) {
            return false;
          }
        }
        break;
      case BindingLocation::Kind::Argument:
        if (!out.printf("(arg slot %u)\n", bi.location().argumentSlot())) {
          return false;
        }
        break;
      case BindingLocation::Kind::Frame:
        if (!out.printf("(frame slot %u)\n", bi.location().slot())) {
          return false;
        }
        break;
      case BindingLocation::Kind::Environment:
        if (!out.printf("(env slot %u)\n", bi.location().slot())) {
          return false;
        }
        break;
      case BindingLocation::Kind::NamedLambdaCallee:
        if (!out.put("(named lambda callee)\n")) {
          return false;
        }
        break;
      case BindingLocation::Kind::Import:
        if (!out.put("(import)\n")) {
          return false;
        }
        break;
    }
  }
  if (i > 0) {
    if (!out.put(indent)) {
      return false;
    }
  }
  if (!out.put("}")) {
    return false;
  }

  ScopeIter si(scope);
  si++;
  for (; si; si++) {
    if (!out.put(" -> ")) {
      return false;
    }
    if (!out.put(ScopeKindString(si.kind()))) {
      return false;
    }
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

template <typename AtomT, typename ShapeT>
bool LexicalScope::prepareForScopeCreation(
    JSContext* cx, ScopeKind kind, uint32_t firstFrameSlot,
    typename MaybeRootedScopeData<LexicalScope, AtomT>::MutableHandleType data,
    ShapeT envShape) {
  bool isNamedLambda =
      kind == ScopeKind::NamedLambda || kind == ScopeKind::StrictNamedLambda;

  MOZ_ASSERT_IF(isNamedLambda, firstFrameSlot == LOCALNO_LIMIT);

  AbstractBindingIter<AtomT> bi(*data, firstFrameSlot, isNamedLambda);
  if (!PrepareScopeData<LexicalScope, AtomT, BlockLexicalEnvironmentObject>(
          cx, bi, data, firstFrameSlot, envShape)) {
    return false;
  }
  return true;
}

/* static */
LexicalScope* LexicalScope::createWithData(
    JSContext* cx, ScopeKind kind, MutableHandle<UniquePtr<RuntimeData>> data,
    uint32_t firstFrameSlot, HandleScope enclosing) {
  RootedShape envShape(cx);

  if (!prepareForScopeCreation<JSAtom>(cx, kind, firstFrameSlot, data,
                                       &envShape)) {
    return nullptr;
  }

  auto scope = Scope::create<LexicalScope>(cx, kind, enclosing, envShape, data);
  if (!scope) {
    return nullptr;
  }

  MOZ_ASSERT(scope->firstFrameSlot() == firstFrameSlot);
  return scope;
}

/* static */
Shape* LexicalScope::getEmptyExtensibleEnvironmentShape(JSContext* cx) {
  const JSClass* cls = &LexicalEnvironmentObject::class_;
  return EmptyEnvironmentShape(cx, cls, JSSLOT_FREE(cls), ObjectFlags());
}

template <XDRMode mode>
/* static */
XDRResult LexicalScope::XDR(XDRState<mode>* xdr, ScopeKind kind,
                            HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();

  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(
      XDRSizedBindingNames<LexicalScope>(xdr, scope.as<LexicalScope>(), &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    uint32_t firstFrameSlot;
    uint32_t nextFrameSlot;
    if (mode == XDR_ENCODE) {
      firstFrameSlot = scope->firstFrameSlot();
      nextFrameSlot = data->slotInfo.nextFrameSlot;
    }

    MOZ_TRY(xdr->codeUint32(&data->slotInfo.constStart));
    MOZ_TRY(xdr->codeUint32(&firstFrameSlot));
    MOZ_TRY(xdr->codeUint32(&nextFrameSlot));

    if (mode == XDR_DECODE) {
      scope.set(createWithData(cx, kind, &uniqueData.ref(), firstFrameSlot,
                               enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // nextFrameSlot is used only for this correctness check.
      MOZ_ASSERT(nextFrameSlot ==
                 scope->as<LexicalScope>().data().slotInfo.nextFrameSlot);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    LexicalScope::XDR(XDRState<XDR_ENCODE>* xdr, ScopeKind kind,
                      HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    LexicalScope::XDR(XDRState<XDR_DECODE>* xdr, ScopeKind kind,
                      HandleScope enclosing, MutableHandleScope scope);

template <typename AtomT, typename ShapeT>
bool ClassBodyScope::prepareForScopeCreation(
    JSContext* cx, ScopeKind kind, uint32_t firstFrameSlot,
    typename MaybeRootedScopeData<ClassBodyScope, AtomT>::MutableHandleType
        data,
    ShapeT envShape) {
  MOZ_ASSERT(kind == ScopeKind::ClassBody);

  AbstractBindingIter<AtomT> bi(*data, firstFrameSlot);
  return PrepareScopeData<ClassBodyScope, AtomT, BlockLexicalEnvironmentObject>(
      cx, bi, data, firstFrameSlot, envShape);
}

/* static */
ClassBodyScope* ClassBodyScope::createWithData(
    JSContext* cx, ScopeKind kind, MutableHandle<UniquePtr<RuntimeData>> data,
    uint32_t firstFrameSlot, HandleScope enclosing) {
  RootedShape envShape(cx);

  if (!prepareForScopeCreation<JSAtom>(cx, kind, firstFrameSlot, data,
                                       &envShape)) {
    return nullptr;
  }

  auto* scope =
      Scope::create<ClassBodyScope>(cx, kind, enclosing, envShape, data);
  if (!scope) {
    return nullptr;
  }

  MOZ_ASSERT(scope->firstFrameSlot() == firstFrameSlot);
  return scope;
}

template <XDRMode mode>
/* static */
XDRResult ClassBodyScope::XDR(XDRState<mode>* xdr, ScopeKind kind,
                              HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();

  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(XDRSizedBindingNames<ClassBodyScope>(xdr, scope.as<ClassBodyScope>(),
                                               &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    uint32_t firstFrameSlot;
    uint32_t nextFrameSlot;
    if (mode == XDR_ENCODE) {
      firstFrameSlot = scope->firstFrameSlot();
      nextFrameSlot = data->slotInfo.nextFrameSlot;
    }

    MOZ_TRY(xdr->codeUint32(&firstFrameSlot));
    MOZ_TRY(xdr->codeUint32(&nextFrameSlot));

    if (mode == XDR_DECODE) {
      scope.set(createWithData(cx, kind, &uniqueData.ref(), firstFrameSlot,
                               enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // nextFrameSlot is used only for this correctness check.
      MOZ_ASSERT(nextFrameSlot ==
                 scope->as<ClassBodyScope>().data().slotInfo.nextFrameSlot);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    ClassBodyScope::XDR(XDRState<XDR_ENCODE>* xdr, ScopeKind kind,
                        HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    ClassBodyScope::XDR(XDRState<XDR_DECODE>* xdr, ScopeKind kind,
                        HandleScope enclosing, MutableHandleScope scope);

static void SetCanonicalFunction(FunctionScope::RuntimeData& data,
                                 HandleFunction fun) {
  data.canonicalFunction.init(fun);
}

static void SetCanonicalFunction(FunctionScope::ParserData& data,
                                 HandleFunction fun) {}

template <typename AtomT, typename ShapeT>
bool FunctionScope::prepareForScopeCreation(
    JSContext* cx,
    typename MaybeRootedScopeData<FunctionScope, AtomT>::MutableHandleType data,
    bool hasParameterExprs, bool needsEnvironment, HandleFunction fun,
    ShapeT envShape) {
  uint32_t firstFrameSlot = 0;
  AbstractBindingIter<AtomT> bi(*data, hasParameterExprs);
  if (!PrepareScopeData<FunctionScope, AtomT, CallObject>(
          cx, bi, data, firstFrameSlot, envShape)) {
    return false;
  }

  if (hasParameterExprs) {
    data->slotInfo.setHasParameterExprs();
  }
  SetCanonicalFunction(*data, fun);

  // An environment may be needed regardless of existence of any closed over
  // bindings:
  //   - Extensible scopes (i.e., due to direct eval)
  //   - Needing a home object
  //   - Being a derived class constructor
  //   - Being a generator or async function
  // Also see |FunctionBox::needsExtraBodyVarEnvironmentRegardlessOfBindings()|.
  return updateEnvShapeIfRequired<CallObject>(cx, envShape, needsEnvironment);
}

/* static */
FunctionScope* FunctionScope::createWithData(
    JSContext* cx, MutableHandle<UniquePtr<RuntimeData>> data,
    bool hasParameterExprs, bool needsEnvironment, HandleFunction fun,
    HandleScope enclosing) {
  MOZ_ASSERT(data);
  MOZ_ASSERT(fun->isTenured());

  RootedShape envShape(cx);

  if (!prepareForScopeCreation<JSAtom>(cx, data, hasParameterExprs,
                                       needsEnvironment, fun, &envShape)) {
    return nullptr;
  }

  return Scope::create<FunctionScope>(cx, ScopeKind::Function, enclosing,
                                      envShape, data);
}

JSScript* FunctionScope::script() const {
  return canonicalFunction()->nonLazyScript();
}

/* static */
bool FunctionScope::isSpecialName(JSContext* cx, JSAtom* name) {
  return name == cx->names().arguments || name == cx->names().dotThis ||
         name == cx->names().dotGenerator;
}

/* static */
bool FunctionScope::isSpecialName(JSContext* cx,
                                  frontend::TaggedParserAtomIndex name) {
  return name == frontend::TaggedParserAtomIndex::WellKnown::arguments() ||
         name == frontend::TaggedParserAtomIndex::WellKnown::dotThis() ||
         name == frontend::TaggedParserAtomIndex::WellKnown::dotGenerator();
}

/* static */
FunctionScope* FunctionScope::clone(JSContext* cx, Handle<FunctionScope*> scope,
                                    HandleFunction fun, HandleScope enclosing) {
  MOZ_ASSERT(fun != scope->canonicalFunction());

  RootedShape envShape(cx);
  if (scope->environmentShape()) {
    envShape = scope->maybeCloneEnvironmentShape(cx);
    if (!envShape) {
      return nullptr;
    }
  }

  Rooted<RuntimeData*> dataOriginal(cx, &scope->as<FunctionScope>().data());
  Rooted<UniquePtr<RuntimeData>> dataClone(
      cx, CopyScopeData<FunctionScope>(cx, dataOriginal));
  if (!dataClone) {
    return nullptr;
  }

  dataClone->canonicalFunction = fun;

  return Scope::create<FunctionScope>(cx, scope->kind(), enclosing, envShape,
                                      &dataClone);
}

template <XDRMode mode>
/* static */
XDRResult FunctionScope::XDR(XDRState<mode>* xdr, HandleFunction fun,
                             HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();
  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(XDRSizedBindingNames<FunctionScope>(xdr, scope.as<FunctionScope>(),
                                              &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    uint8_t needsEnvironment;
    uint8_t hasParameterExprs;
    uint32_t nextFrameSlot;
    if (mode == XDR_ENCODE) {
      needsEnvironment = scope->hasEnvironment();
      hasParameterExprs = data->slotInfo.hasParameterExprs();
      nextFrameSlot = data->slotInfo.nextFrameSlot;
    }
    MOZ_TRY(xdr->codeUint8(&needsEnvironment));
    MOZ_TRY(xdr->codeUint8(&hasParameterExprs));
    MOZ_TRY(xdr->codeUint16(&data->slotInfo.nonPositionalFormalStart));
    MOZ_TRY(xdr->codeUint16(&data->slotInfo.varStart));
    MOZ_TRY(xdr->codeUint32(&nextFrameSlot));

    if (mode == XDR_DECODE) {
      if (!data->length) {
        MOZ_ASSERT(!data->slotInfo.nonPositionalFormalStart);
        MOZ_ASSERT(!data->slotInfo.varStart);
        MOZ_ASSERT(!data->slotInfo.nextFrameSlot);
      }

      scope.set(createWithData(cx, &uniqueData.ref(), hasParameterExprs,
                               needsEnvironment, fun, enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // nextFrameSlot is used only for this correctness check.
      MOZ_ASSERT(nextFrameSlot ==
                 scope->as<FunctionScope>().data().slotInfo.nextFrameSlot);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    FunctionScope::XDR(XDRState<XDR_ENCODE>* xdr, HandleFunction fun,
                       HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    FunctionScope::XDR(XDRState<XDR_DECODE>* xdr, HandleFunction fun,
                       HandleScope enclosing, MutableHandleScope scope);

template <typename AtomT, typename ShapeT>
bool VarScope::prepareForScopeCreation(
    JSContext* cx, ScopeKind kind,
    typename MaybeRootedScopeData<VarScope, AtomT>::MutableHandleType data,
    uint32_t firstFrameSlot, bool needsEnvironment, ShapeT envShape) {
  AbstractBindingIter<AtomT> bi(*data, firstFrameSlot);
  if (!PrepareScopeData<VarScope, AtomT, VarEnvironmentObject>(
          cx, bi, data, firstFrameSlot, envShape)) {
    return false;
  }

  // An environment may be needed regardless of existence of any closed over
  // bindings:
  //   - Extensible scopes (i.e., due to direct eval)
  //   - Being a generator
  return updateEnvShapeIfRequired<VarEnvironmentObject>(cx, envShape,
                                                        needsEnvironment);
}

/* static */
VarScope* VarScope::createWithData(JSContext* cx, ScopeKind kind,
                                   MutableHandle<UniquePtr<RuntimeData>> data,
                                   uint32_t firstFrameSlot,
                                   bool needsEnvironment,
                                   HandleScope enclosing) {
  MOZ_ASSERT(data);

  RootedShape envShape(cx);
  if (!prepareForScopeCreation<JSAtom>(cx, kind, data, firstFrameSlot,
                                       needsEnvironment, &envShape)) {
    return nullptr;
  }

  return Scope::create<VarScope>(cx, kind, enclosing, envShape, data);
}

template <XDRMode mode>
/* static */
XDRResult VarScope::XDR(XDRState<mode>* xdr, ScopeKind kind,
                        HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();
  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(XDRSizedBindingNames<VarScope>(xdr, scope.as<VarScope>(), &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    uint8_t needsEnvironment;
    uint32_t firstFrameSlot;
    uint32_t nextFrameSlot;
    if (mode == XDR_ENCODE) {
      needsEnvironment = scope->hasEnvironment();
      firstFrameSlot = scope->firstFrameSlot();
      nextFrameSlot = data->slotInfo.nextFrameSlot;
    }
    MOZ_TRY(xdr->codeUint8(&needsEnvironment));
    MOZ_TRY(xdr->codeUint32(&firstFrameSlot));
    MOZ_TRY(xdr->codeUint32(&nextFrameSlot));

    if (mode == XDR_DECODE) {
      if (!data->length) {
        MOZ_ASSERT(!data->slotInfo.nextFrameSlot);
      }

      scope.set(createWithData(cx, kind, &uniqueData.ref(), firstFrameSlot,
                               needsEnvironment, enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // nextFrameSlot is used only for this correctness check.
      MOZ_ASSERT(nextFrameSlot ==
                 scope->as<VarScope>().data().slotInfo.nextFrameSlot);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    VarScope::XDR(XDRState<XDR_ENCODE>* xdr, ScopeKind kind,
                  HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    VarScope::XDR(XDRState<XDR_DECODE>* xdr, ScopeKind kind,
                  HandleScope enclosing, MutableHandleScope scope);

/* static */
GlobalScope* GlobalScope::create(JSContext* cx, ScopeKind kind,
                                 Handle<RuntimeData*> dataArg) {
  // The data that's passed in is from the frontend and is LifoAlloc'd.
  // Copy it now that we're creating a permanent VM scope.
  Rooted<UniquePtr<RuntimeData>> data(
      cx, dataArg ? CopyScopeData<GlobalScope>(cx, dataArg)
                  : NewEmptyScopeData<GlobalScope, JSAtom>(cx));
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
GlobalScope* GlobalScope::clone(JSContext* cx, Handle<GlobalScope*> scope) {
  Rooted<RuntimeData*> dataOriginal(cx, &scope->as<GlobalScope>().data());
  Rooted<UniquePtr<RuntimeData>> dataClone(
      cx, CopyScopeData<GlobalScope>(cx, dataOriginal));
  if (!dataClone) {
    return nullptr;
  }
  return Scope::create<GlobalScope>(cx, scope->kind(), nullptr, nullptr,
                                    &dataClone);
}

template <XDRMode mode>
/* static */
XDRResult GlobalScope::XDR(XDRState<mode>* xdr, ScopeKind kind,
                           MutableHandleScope scope) {
  MOZ_ASSERT((mode == XDR_DECODE) == !scope);

  JSContext* cx = xdr->cx();
  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(
      XDRSizedBindingNames<GlobalScope>(xdr, scope.as<GlobalScope>(), &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    MOZ_TRY(xdr->codeUint32(&data->slotInfo.letStart));
    MOZ_TRY(xdr->codeUint32(&data->slotInfo.constStart));

    if (mode == XDR_DECODE) {
      if (!data->length) {
        MOZ_ASSERT(!data->slotInfo.letStart);
        MOZ_ASSERT(!data->slotInfo.constStart);
      }

      scope.set(createWithData(cx, kind, &uniqueData.ref()));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    GlobalScope::XDR(XDRState<XDR_ENCODE>* xdr, ScopeKind kind,
                     MutableHandleScope scope);

template
    /* static */
    XDRResult
    GlobalScope::XDR(XDRState<XDR_DECODE>* xdr, ScopeKind kind,
                     MutableHandleScope scope);

/* static */
WithScope* WithScope::create(JSContext* cx, HandleScope enclosing) {
  Scope* scope = Scope::create(cx, ScopeKind::With, enclosing, nullptr);
  return static_cast<WithScope*>(scope);
}

template <XDRMode mode>
/* static */
XDRResult WithScope::XDR(XDRState<mode>* xdr, HandleScope enclosing,
                         MutableHandleScope scope) {
  JSContext* cx = xdr->cx();
  if (mode == XDR_DECODE) {
    scope.set(create(cx, enclosing));
    if (!scope) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    WithScope::XDR(XDRState<XDR_ENCODE>* xdr, HandleScope enclosing,
                   MutableHandleScope scope);

template
    /* static */
    XDRResult
    WithScope::XDR(XDRState<XDR_DECODE>* xdr, HandleScope enclosing,
                   MutableHandleScope scope);

template <typename AtomT, typename ShapeT>
bool EvalScope::prepareForScopeCreation(
    JSContext* cx, ScopeKind scopeKind,
    typename MaybeRootedScopeData<EvalScope, AtomT>::MutableHandleType data,
    ShapeT envShape) {
  if (scopeKind == ScopeKind::StrictEval) {
    uint32_t firstFrameSlot = 0;
    AbstractBindingIter<AtomT> bi(*data, true);
    if (!PrepareScopeData<EvalScope, AtomT, VarEnvironmentObject>(
            cx, bi, data, firstFrameSlot, envShape)) {
      return false;
    }
  }

  return true;
}

/* static */
EvalScope* EvalScope::createWithData(JSContext* cx, ScopeKind scopeKind,
                                     MutableHandle<UniquePtr<RuntimeData>> data,
                                     HandleScope enclosing) {
  MOZ_ASSERT(data);

  RootedShape envShape(cx);
  if (!prepareForScopeCreation<JSAtom>(cx, scopeKind, data, &envShape)) {
    return nullptr;
  }

  return Scope::create<EvalScope>(cx, scopeKind, enclosing, envShape, data);
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

template <XDRMode mode>
/* static */
XDRResult EvalScope::XDR(XDRState<mode>* xdr, ScopeKind kind,
                         HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();
  Rooted<RuntimeData*> data(cx);

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    MOZ_TRY(XDRSizedBindingNames<EvalScope>(xdr, scope.as<EvalScope>(), &data));

    if (mode == XDR_DECODE) {
      if (!data->length) {
        MOZ_ASSERT(!data->slotInfo.nextFrameSlot);
      }
      scope.set(createWithData(cx, kind, &uniqueData.ref(), enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    EvalScope::XDR(XDRState<XDR_ENCODE>* xdr, ScopeKind kind,
                   HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    EvalScope::XDR(XDRState<XDR_DECODE>* xdr, ScopeKind kind,
                   HandleScope enclosing, MutableHandleScope scope);

ModuleScope::RuntimeData::RuntimeData(size_t length) {
  PoisonNames(this, length);
}

static void InitModule(ModuleScope::RuntimeData& data,
                       HandleModuleObject module) {
  data.module.init(module);
}

static void InitModule(ModuleScope::ParserData& data,
                       HandleModuleObject module) {}

/* static */
template <typename AtomT, typename ShapeT>
bool ModuleScope::prepareForScopeCreation(
    JSContext* cx,
    typename MaybeRootedScopeData<ModuleScope, AtomT>::MutableHandleType data,
    HandleModuleObject module, ShapeT envShape) {
  uint32_t firstFrameSlot = 0;
  AbstractBindingIter<AtomT> bi(*data);
  if (!PrepareScopeData<ModuleScope, AtomT, ModuleEnvironmentObject>(
          cx, bi, data, firstFrameSlot, envShape)) {
    return false;
  }

  InitModule(*data, module);

  // Modules always need an environment object for now.
  bool needsEnvironment = true;

  return updateEnvShapeIfRequired<ModuleEnvironmentObject>(cx, envShape,
                                                           needsEnvironment);
}

/* static */
ModuleScope* ModuleScope::createWithData(
    JSContext* cx, MutableHandle<UniquePtr<RuntimeData>> data,
    HandleModuleObject module, HandleScope enclosing) {
  MOZ_ASSERT(data);
  MOZ_ASSERT(enclosing->is<GlobalScope>());

  RootedShape envShape(cx);
  if (!prepareForScopeCreation<JSAtom>(cx, data, module, &envShape)) {
    return nullptr;
  }

  return Scope::create<ModuleScope>(cx, ScopeKind::Module, enclosing, envShape,
                                    data);
}

template <size_t ArrayLength>
static JSAtom* GenerateWasmName(JSContext* cx,
                                const char (&prefix)[ArrayLength],
                                uint32_t index) {
  StringBuffer sb(cx);
  if (!sb.append(prefix)) {
    return nullptr;
  }
  if (!NumberValueToStringBuffer(cx, Int32Value(index), sb)) {
    return nullptr;
  }

  return sb.finishAtom();
}

template <XDRMode mode>
/* static */
XDRResult ModuleScope::XDR(XDRState<mode>* xdr, HandleModuleObject module,
                           HandleScope enclosing, MutableHandleScope scope) {
  JSContext* cx = xdr->cx();
  Rooted<RuntimeData*> data(cx);
  MOZ_TRY(
      XDRSizedBindingNames<ModuleScope>(xdr, scope.as<ModuleScope>(), &data));

  {
    Maybe<Rooted<UniquePtr<RuntimeData>>> uniqueData;
    if (mode == XDR_DECODE) {
      uniqueData.emplace(cx, data);
    }

    uint32_t nextFrameSlot;
    if (mode == XDR_ENCODE) {
      nextFrameSlot = data->slotInfo.nextFrameSlot;
    }

    MOZ_TRY(xdr->codeUint32(&data->slotInfo.varStart));
    MOZ_TRY(xdr->codeUint32(&data->slotInfo.letStart));
    MOZ_TRY(xdr->codeUint32(&data->slotInfo.constStart));
    MOZ_TRY(xdr->codeUint32(&nextFrameSlot));

    if (mode == XDR_DECODE) {
      if (!data->length) {
        MOZ_ASSERT(!data->slotInfo.varStart);
        MOZ_ASSERT(!data->slotInfo.letStart);
        MOZ_ASSERT(!data->slotInfo.constStart);
        MOZ_ASSERT(!data->slotInfo.nextFrameSlot);
      }

      scope.set(createWithData(cx, &uniqueData.ref(), module, enclosing));
      if (!scope) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // nextFrameSlot is used only for this correctness check.
      MOZ_ASSERT(nextFrameSlot ==
                 scope->as<ModuleScope>().data().slotInfo.nextFrameSlot);
    }
  }

  return Ok();
}

template
    /* static */
    XDRResult
    ModuleScope::XDR(XDRState<XDR_ENCODE>* xdr, HandleModuleObject module,
                     HandleScope enclosing, MutableHandleScope scope);

template
    /* static */
    XDRResult
    ModuleScope::XDR(XDRState<XDR_DECODE>* xdr, HandleModuleObject module,
                     HandleScope enclosing, MutableHandleScope scope);

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
  if (instance->instance().memory()) {
    namesCount++;
  }
  size_t globalsStart = namesCount;
  size_t globalsCount = instance->instance().metadata().globals.length();
  namesCount += globalsCount;

  Rooted<UniquePtr<RuntimeData>> data(
      cx, NewEmptyScopeData<WasmInstanceScope, JSAtom>(cx, namesCount));
  if (!data) {
    return nullptr;
  }

  if (instance->instance().memory()) {
    JSAtom* wasmName = GenerateWasmName(cx, "memory", /* index = */ 0);
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

  data->instance.init(instance);
  data->slotInfo.globalsStart = globalsStart;

  RootedScope enclosing(cx, &cx->global()->emptyGlobalScope());
  return Scope::create<WasmInstanceScope>(cx, ScopeKind::WasmInstance,
                                          enclosing,
                                          /* envShape = */ nullptr, &data);
}

/* static */
WasmFunctionScope* WasmFunctionScope::create(JSContext* cx,
                                             HandleScope enclosing,
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
    init(/* positionalFormalStart= */ 0,
         /* nonPositionalFormalStart= */ 0,
         /* varStart= */ 0,
         /* letStart= */ 0,
         /* constStart= */ slotInfo.constStart,
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
  init(
      /* positionalFormalStart= */ slotInfo.varStart,
      /* nonPositionalFormalStart= */ slotInfo.varStart,
      /* varStart= */ slotInfo.varStart,
      /* letStart= */ slotInfo.letStart,
      /* constStart= */ slotInfo.constStart,
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

PositionalFormalParameterIter::PositionalFormalParameterIter(Scope* scope)
    : BindingIter(scope) {
  // Reinit with flags = 0, i.e., iterate over all positional parameters.
  if (scope->is<FunctionScope>()) {
    init(scope->as<FunctionScope>().data(), /* flags = */ 0);
  }
  settle();
}

PositionalFormalParameterIter::PositionalFormalParameterIter(JSScript* script)
    : PositionalFormalParameterIter(script->bodyScope()) {}

void js::DumpBindings(JSContext* cx, Scope* scopeArg) {
  RootedScope scope(cx, scopeArg);
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
    JSContext* cx, CompilationState& compilationState,
    BaseParserScopeData* data, ScopeIndex* indexOut, Args&&... args) {
  *indexOut = ScopeIndex(compilationState.scopeData.length());
  if (uint32_t(*indexOut) >= TaggedScriptThingIndex::IndexLimit) {
    ReportAllocationOverflow(cx);
    return false;
  }

  if (!compilationState.scopeData.emplaceBack(std::forward<Args>(args)...)) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  if (!compilationState.scopeNames.append(data)) {
    compilationState.scopeData.popBack();
    MOZ_ASSERT(compilationState.scopeData.length() ==
               compilationState.scopeNames.length());

    js::ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

/* static */
bool ScopeStencil::createForFunctionScope(
    JSContext* cx, frontend::CompilationState& compilationState,
    FunctionScope::ParserData* data, bool hasParameterExprs,
    bool needsEnvironment, ScriptIndex functionIndex, bool isArrow,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  auto kind = ScopeKind::Function;
  using ScopeType = FunctionScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  // We do not initialize the canonical function while the data is owned by the
  // ScopeStencil. It gets set in ScopeStencil::releaseData.
  RootedFunction fun(cx, nullptr);

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  if (!FunctionScope::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, &data, hasParameterExprs, needsEnvironment, fun, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape,
                                   mozilla::Some(functionIndex), isArrow);
}

/* static */
bool ScopeStencil::createForLexicalScope(
    JSContext* cx, frontend::CompilationState& compilationState, ScopeKind kind,
    LexicalScope::ParserData* data, uint32_t firstFrameSlot,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = LexicalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  if (!ScopeType::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, kind, firstFrameSlot, &data, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForClassBodyScope(
    JSContext* cx, frontend::CompilationState& compilationState, ScopeKind kind,
    ClassBodyScope::ParserData* data, uint32_t firstFrameSlot,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = ClassBodyScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  if (!ScopeType::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, kind, firstFrameSlot, &data, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

bool ScopeStencil::createForVarScope(
    JSContext* cx, frontend::CompilationState& compilationState, ScopeKind kind,
    VarScope::ParserData* data, uint32_t firstFrameSlot, bool needsEnvironment,
    mozilla::Maybe<ScopeIndex> enclosing, ScopeIndex* index) {
  using ScopeType = VarScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  mozilla::Maybe<uint32_t> envShape;
  if (!VarScope::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, kind, &data, firstFrameSlot, needsEnvironment, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForGlobalScope(
    JSContext* cx, frontend::CompilationState& compilationState, ScopeKind kind,
    GlobalScope::ParserData* data, ScopeIndex* index) {
  using ScopeType = GlobalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
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

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForEvalScope(
    JSContext* cx, frontend::CompilationState& compilationState, ScopeKind kind,
    EvalScope::ParserData* data, mozilla::Maybe<ScopeIndex> enclosing,
    ScopeIndex* index) {
  using ScopeType = EvalScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  if (!EvalScope::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, kind, &data, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

/* static */
bool ScopeStencil::createForModuleScope(
    JSContext* cx, frontend::CompilationState& compilationState,
    ModuleScope::ParserData* data, mozilla::Maybe<ScopeIndex> enclosing,
    ScopeIndex* index) {
  auto kind = ScopeKind::Module;
  using ScopeType = ModuleScope;
  MOZ_ASSERT(matchScopeKind<ScopeType>(kind));

  if (data) {
    MarkParserScopeData<ScopeType>(cx, data, compilationState);
  } else {
    data = NewEmptyParserScopeData<ScopeType>(cx, compilationState.alloc);
    if (!data) {
      return false;
    }
  }

  MOZ_ASSERT(enclosing.isNothing());

  // We do not initialize the canonical module while the data is owned by the
  // ScopeStencil. It gets set in ScopeStencil::releaseData.
  RootedModuleObject module(cx, nullptr);

  // The data that's passed in is from the frontend and is LifoAlloc'd.
  // Copy it now that we're creating a permanent VM scope.
  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;
  if (!ModuleScope::prepareForScopeCreation<frontend::TaggedParserAtomIndex>(
          cx, &data, module, &envShape)) {
    return false;
  }

  return appendScopeStencilAndData(cx, compilationState, data, index, kind,
                                   enclosing, firstFrameSlot, envShape);
}

template <typename SpecificEnvironmentT>
bool ScopeStencil::createSpecificShape(JSContext* cx, ScopeKind kind,
                                       BaseScopeData* scopeData,
                                       MutableHandleShape shape) const {
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
bool ScopeStencil::createForWithScope(JSContext* cx,
                                      CompilationState& compilationState,
                                      mozilla::Maybe<ScopeIndex> enclosing,
                                      ScopeIndex* index) {
  auto kind = ScopeKind::With;
  MOZ_ASSERT(matchScopeKind<WithScope>(kind));

  uint32_t firstFrameSlot = 0;
  mozilla::Maybe<uint32_t> envShape;

  return appendScopeStencilAndData(cx, compilationState, nullptr, index, kind,
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
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const {
  return Scope::create(cx, ScopeKind::With, enclosingScope, nullptr);
}

// GlobalScope has bindings but no environment shape.
template <>
Scope* ScopeStencil::createSpecificScope<GlobalScope, std::nullptr_t>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const {
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
                                         HandleScope enclosingScope,
                                         BaseParserScopeData* baseData) const {
  Rooted<UniquePtr<typename SpecificScopeT::RuntimeData>> rootedData(
      cx, createSpecificScopeData<SpecificScopeT>(cx, atomCache, baseData));
  if (!rootedData) {
    return nullptr;
  }

  RootedShape shape(cx);
  if (!createSpecificShape<SpecificEnvironmentT>(
          cx, kind(), rootedData.get().get(), &shape)) {
    return nullptr;
  }

  // Because we already baked the data here, we needn't do it again.
  return Scope::create<SpecificScopeT>(cx, kind(), enclosingScope, shape,
                                       &rootedData);
}

template Scope* ScopeStencil::createSpecificScope<FunctionScope, CallObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<LexicalScope, BlockLexicalEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
template Scope* ScopeStencil::createSpecificScope<
    ClassBodyScope, BlockLexicalEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<EvalScope, VarEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<VarScope, VarEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
template Scope*
ScopeStencil::createSpecificScope<ModuleScope, ModuleEnvironmentObject>(
    JSContext* cx, CompilationAtomCache& atomCache, HandleScope enclosingScope,
    BaseParserScopeData* baseData) const;
