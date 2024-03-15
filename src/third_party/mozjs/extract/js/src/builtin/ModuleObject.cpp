/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/ModuleObject.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/EnumSet.h"
#include "mozilla/ScopeExit.h"

#include "builtin/Promise.h"
#include "builtin/SelfHostingDefines.h"
#include "frontend/ParseNode.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, ParserAtomsTable, ParserAtom
#include "frontend/SharedContext.h"
#include "frontend/Stencil.h"
#include "gc/GCContext.h"
#include "gc/Tracer.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Modules.h"  // JS::GetModulePrivate, JS::ModuleDynamicImportHook
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/Interpreter.h"    // Execute, Lambda, ReportRuntimeLexicalError
#include "vm/ModuleBuilder.h"  // js::ModuleBuilder
#include "vm/Modules.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/SharedStencil.h"  // js::GCThingIndex

#include "builtin/HandlerFunction-inl.h"  // js::ExtraValueFromHandler, js::NewHandler{,WithExtraValue}, js::TargetFromHandler
#include "gc/GCContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/List-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
using mozilla::Span;

static_assert(ModuleStatus::Unlinked < ModuleStatus::Linking &&
                  ModuleStatus::Linking < ModuleStatus::Linked &&
                  ModuleStatus::Linked < ModuleStatus::Evaluating &&
                  ModuleStatus::Evaluating < ModuleStatus::EvaluatingAsync &&
                  ModuleStatus::EvaluatingAsync < ModuleStatus::Evaluated &&
                  ModuleStatus::Evaluated < ModuleStatus::Evaluated_Error,
              "Module statuses are ordered incorrectly");

static Value StringOrNullValue(JSString* maybeString) {
  return maybeString ? StringValue(maybeString) : NullValue();
}

#define DEFINE_ATOM_ACCESSOR_METHOD(cls, name, slot) \
  JSAtom* cls::name() const {                        \
    Value value = getReservedSlot(slot);             \
    return &value.toString()->asAtom();              \
  }

#define DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(cls, name, slot) \
  JSAtom* cls::name() const {                                \
    Value value = getReservedSlot(slot);                     \
    if (value.isNull()) {                                    \
      return nullptr;                                        \
    }                                                        \
    return &value.toString()->asAtom();                      \
  }

#define DEFINE_UINT32_ACCESSOR_METHOD(cls, name, slot) \
  uint32_t cls::name() const {                         \
    Value value = getReservedSlot(slot);               \
    MOZ_ASSERT(value.toNumber() >= 0);                 \
    if (value.isInt32()) {                             \
      return value.toInt32();                          \
    }                                                  \
    return JS::ToUint32(value.toDouble());             \
  }

///////////////////////////////////////////////////////////////////////////
// ImportEntry

ImportEntry::ImportEntry(Handle<ModuleRequestObject*> moduleRequest,
                         Handle<JSAtom*> maybeImportName,
                         Handle<JSAtom*> localName, uint32_t lineNumber,
                         uint32_t columnNumber)
    : moduleRequest_(moduleRequest),
      importName_(maybeImportName),
      localName_(localName),
      lineNumber_(lineNumber),
      columnNumber_(columnNumber) {}

void ImportEntry::trace(JSTracer* trc) {
  TraceEdge(trc, &moduleRequest_, "ImportEntry::moduleRequest_");
  TraceNullableEdge(trc, &importName_, "ImportEntry::importName_");
  TraceNullableEdge(trc, &localName_, "ImportEntry::localName_");
}

///////////////////////////////////////////////////////////////////////////
// ExportEntry

ExportEntry::ExportEntry(Handle<JSAtom*> maybeExportName,
                         Handle<ModuleRequestObject*> moduleRequest,
                         Handle<JSAtom*> maybeImportName,
                         Handle<JSAtom*> maybeLocalName, uint32_t lineNumber,
                         uint32_t columnNumber)
    : exportName_(maybeExportName),
      moduleRequest_(moduleRequest),
      importName_(maybeImportName),
      localName_(maybeLocalName),
      lineNumber_(lineNumber),
      columnNumber_(columnNumber) {
  // Line and column numbers are optional for export entries since direct
  // entries are checked at parse time.
}

void ExportEntry::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &exportName_, "ExportEntry::exportName_");
  TraceNullableEdge(trc, &moduleRequest_, "ExportEntry::moduleRequest_");
  TraceNullableEdge(trc, &importName_, "ExportEntry::importName_");
  TraceNullableEdge(trc, &localName_, "ExportEntry::localName_");
}

///////////////////////////////////////////////////////////////////////////
// RequestedModule

/* static */
RequestedModule::RequestedModule(Handle<ModuleRequestObject*> moduleRequest,
                                 uint32_t lineNumber, uint32_t columnNumber)
    : moduleRequest_(moduleRequest),
      lineNumber_(lineNumber),
      columnNumber_(columnNumber) {}

void RequestedModule::trace(JSTracer* trc) {
  TraceEdge(trc, &moduleRequest_, "ExportEntry::moduleRequest_");
}

///////////////////////////////////////////////////////////////////////////
// ResolvedBindingObject

/* static */ const JSClass ResolvedBindingObject::class_ = {
    "ResolvedBinding",
    JSCLASS_HAS_RESERVED_SLOTS(ResolvedBindingObject::SlotCount)};

ModuleObject* ResolvedBindingObject::module() const {
  Value value = getReservedSlot(ModuleSlot);
  return &value.toObject().as<ModuleObject>();
}

JSAtom* ResolvedBindingObject::bindingName() const {
  Value value = getReservedSlot(BindingNameSlot);
  return &value.toString()->asAtom();
}

/* static */
bool ResolvedBindingObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ResolvedBindingObject>();
}

/* static */
ResolvedBindingObject* ResolvedBindingObject::create(
    JSContext* cx, Handle<ModuleObject*> module, Handle<JSAtom*> bindingName) {
  ResolvedBindingObject* self =
      NewObjectWithGivenProto<ResolvedBindingObject>(cx, nullptr);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(ModuleSlot, ObjectValue(*module));
  self->initReservedSlot(BindingNameSlot, StringValue(bindingName));
  return self;
}

///////////////////////////////////////////////////////////////////////////
// ModuleRequestObject
/* static */ const JSClass ModuleRequestObject::class_ = {
    "ModuleRequest",
    JSCLASS_HAS_RESERVED_SLOTS(ModuleRequestObject::SlotCount)};

DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ModuleRequestObject, specifier,
                                    SpecifierSlot)

ArrayObject* ModuleRequestObject::assertions() const {
  JSObject* obj = getReservedSlot(AssertionSlot).toObjectOrNull();
  if (!obj) {
    return nullptr;
  }

  return &obj->as<ArrayObject>();
}

/* static */
bool ModuleRequestObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ModuleRequestObject>();
}

/* static */
ModuleRequestObject* ModuleRequestObject::create(
    JSContext* cx, Handle<JSAtom*> specifier,
    Handle<ArrayObject*> maybeAssertions) {
  ModuleRequestObject* self =
      NewObjectWithGivenProto<ModuleRequestObject>(cx, nullptr);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(SpecifierSlot, StringOrNullValue(specifier));
  self->initReservedSlot(AssertionSlot, ObjectOrNullValue(maybeAssertions));
  return self;
}

///////////////////////////////////////////////////////////////////////////
// IndirectBindingMap

IndirectBindingMap::Binding::Binding(ModuleEnvironmentObject* environment,
                                     jsid targetName, PropertyInfo prop)
    : environment(environment),
#ifdef DEBUG
      targetName(targetName),
#endif
      prop(prop) {
}

void IndirectBindingMap::trace(JSTracer* trc) {
  if (!map_) {
    return;
  }

  for (Map::Enum e(*map_); !e.empty(); e.popFront()) {
    Binding& b = e.front().value();
    TraceEdge(trc, &b.environment, "module bindings environment");
#ifdef DEBUG
    TraceEdge(trc, &b.targetName, "module bindings target name");
#endif
    mozilla::DebugOnly<jsid> prev(e.front().key());
    TraceEdge(trc, &e.front().mutableKey(), "module bindings binding name");
    MOZ_ASSERT(e.front().key() == prev);
  }
}

bool IndirectBindingMap::put(JSContext* cx, HandleId name,
                             Handle<ModuleEnvironmentObject*> environment,
                             HandleId targetName) {
  if (!map_) {
    map_.emplace(cx->zone());
  }

  mozilla::Maybe<PropertyInfo> prop = environment->lookup(cx, targetName);
  MOZ_ASSERT(prop.isSome());
  if (!map_->put(name, Binding(environment, targetName, *prop))) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool IndirectBindingMap::lookup(jsid name, ModuleEnvironmentObject** envOut,
                                mozilla::Maybe<PropertyInfo>* propOut) const {
  if (!map_) {
    return false;
  }

  auto ptr = map_->lookup(name);
  if (!ptr) {
    return false;
  }

  const Binding& binding = ptr->value();
  MOZ_ASSERT(binding.environment);
  MOZ_ASSERT(
      binding.environment->containsPure(binding.targetName, binding.prop));
  *envOut = binding.environment;
  *propOut = Some(binding.prop);
  return true;
}

///////////////////////////////////////////////////////////////////////////
// ModuleNamespaceObject

/* static */
const ModuleNamespaceObject::ProxyHandler ModuleNamespaceObject::proxyHandler;

/* static */
bool ModuleNamespaceObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ModuleNamespaceObject>();
}

/* static */
ModuleNamespaceObject* ModuleNamespaceObject::create(
    JSContext* cx, Handle<ModuleObject*> module,
    MutableHandle<UniquePtr<ExportNameVector>> exports,
    MutableHandle<UniquePtr<IndirectBindingMap>> bindings) {
  RootedValue priv(cx, ObjectValue(*module));
  ProxyOptions options;
  options.setLazyProto(true);

  RootedObject object(
      cx, NewProxyObject(cx, &proxyHandler, priv, nullptr, options));
  if (!object) {
    return nullptr;
  }

  SetProxyReservedSlot(object, ExportsSlot,
                       PrivateValue(exports.get().release()));
  AddCellMemory(object, sizeof(ExportNameVector), MemoryUse::ModuleExports);

  SetProxyReservedSlot(object, BindingsSlot,
                       PrivateValue(bindings.get().release()));
  AddCellMemory(object, sizeof(IndirectBindingMap),
                MemoryUse::ModuleBindingMap);

  return &object->as<ModuleNamespaceObject>();
}

ModuleObject& ModuleNamespaceObject::module() {
  return GetProxyPrivate(this).toObject().as<ModuleObject>();
}

const ExportNameVector& ModuleNamespaceObject::exports() const {
  Value value = GetProxyReservedSlot(this, ExportsSlot);
  auto* exports = static_cast<ExportNameVector*>(value.toPrivate());
  MOZ_ASSERT(exports);
  return *exports;
}

ExportNameVector& ModuleNamespaceObject::mutableExports() {
  // Get a non-const reference for tracing/destruction. Do not actually mutate
  // this vector!  This would be incorrect without adding barriers.
  return const_cast<ExportNameVector&>(exports());
}

IndirectBindingMap& ModuleNamespaceObject::bindings() {
  Value value = GetProxyReservedSlot(this, BindingsSlot);
  auto* bindings = static_cast<IndirectBindingMap*>(value.toPrivate());
  MOZ_ASSERT(bindings);
  return *bindings;
}

bool ModuleNamespaceObject::hasExports() const {
  // Exports may not be present if we hit OOM in initialization.
  return !GetProxyReservedSlot(this, ExportsSlot).isUndefined();
}

bool ModuleNamespaceObject::hasBindings() const {
  // Import bindings may not be present if we hit OOM in initialization.
  return !GetProxyReservedSlot(this, BindingsSlot).isUndefined();
}

bool ModuleNamespaceObject::addBinding(JSContext* cx,
                                       Handle<JSAtom*> exportedName,
                                       Handle<ModuleObject*> targetModule,
                                       Handle<JSAtom*> targetName) {
  Rooted<ModuleEnvironmentObject*> environment(
      cx, &targetModule->initialEnvironment());
  RootedId exportedNameId(cx, AtomToId(exportedName));
  RootedId targetNameId(cx, AtomToId(targetName));
  return bindings().put(cx, exportedNameId, environment, targetNameId);
}

const char ModuleNamespaceObject::ProxyHandler::family = 0;

ModuleNamespaceObject::ProxyHandler::ProxyHandler()
    : BaseProxyHandler(&family, false) {}

bool ModuleNamespaceObject::ProxyHandler::getPrototype(
    JSContext* cx, HandleObject proxy, MutableHandleObject protop) const {
  protop.set(nullptr);
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::setPrototype(
    JSContext* cx, HandleObject proxy, HandleObject proto,
    ObjectOpResult& result) const {
  if (!proto) {
    return result.succeed();
  }
  return result.failCantSetProto();
}

bool ModuleNamespaceObject::ProxyHandler::getPrototypeIfOrdinary(
    JSContext* cx, HandleObject proxy, bool* isOrdinary,
    MutableHandleObject protop) const {
  *isOrdinary = false;
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::setImmutablePrototype(
    JSContext* cx, HandleObject proxy, bool* succeeded) const {
  *succeeded = true;
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::isExtensible(JSContext* cx,
                                                       HandleObject proxy,
                                                       bool* extensible) const {
  *extensible = false;
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::preventExtensions(
    JSContext* cx, HandleObject proxy, ObjectOpResult& result) const {
  result.succeed();
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject proxy, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
  Rooted<ModuleNamespaceObject*> ns(cx, &proxy->as<ModuleNamespaceObject>());
  if (id.isSymbol()) {
    if (id.isWellKnownSymbol(JS::SymbolCode::toStringTag)) {
      desc.set(Some(PropertyDescriptor::Data(StringValue(cx->names().Module))));
      return true;
    }

    desc.reset();
    return true;
  }

  const IndirectBindingMap& bindings = ns->bindings();
  ModuleEnvironmentObject* env;
  mozilla::Maybe<PropertyInfo> prop;
  if (!bindings.lookup(id, &env, &prop)) {
    // Not found.
    desc.reset();
    return true;
  }

  RootedValue value(cx, env->getSlot(prop->slot()));
  if (value.isMagic(JS_UNINITIALIZED_LEXICAL)) {
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, id);
    return false;
  }

  desc.set(
      Some(PropertyDescriptor::Data(value, {JS::PropertyAttribute::Enumerable,
                                            JS::PropertyAttribute::Writable})));
  return true;
}

static bool ValidatePropertyDescriptor(
    JSContext* cx, Handle<PropertyDescriptor> desc, bool expectedWritable,
    bool expectedEnumerable, bool expectedConfigurable,
    HandleValue expectedValue, ObjectOpResult& result) {
  if (desc.isAccessorDescriptor()) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  if (desc.hasWritable() && desc.writable() != expectedWritable) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  if (desc.hasEnumerable() && desc.enumerable() != expectedEnumerable) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  if (desc.hasConfigurable() && desc.configurable() != expectedConfigurable) {
    return result.fail(JSMSG_CANT_REDEFINE_PROP);
  }

  if (desc.hasValue()) {
    bool same;
    if (!SameValue(cx, desc.value(), expectedValue, &same)) {
      return false;
    }
    if (!same) {
      return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }
  }

  return result.succeed();
}

bool ModuleNamespaceObject::ProxyHandler::defineProperty(
    JSContext* cx, HandleObject proxy, HandleId id,
    Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  if (id.isSymbol()) {
    if (id.isWellKnownSymbol(JS::SymbolCode::toStringTag)) {
      RootedValue value(cx, StringValue(cx->names().Module));
      return ValidatePropertyDescriptor(cx, desc, false, false, false, value,
                                        result);
    }
    return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);
  }

  const IndirectBindingMap& bindings =
      proxy->as<ModuleNamespaceObject>().bindings();
  ModuleEnvironmentObject* env;
  mozilla::Maybe<PropertyInfo> prop;
  if (!bindings.lookup(id, &env, &prop)) {
    return result.fail(JSMSG_CANT_DEFINE_PROP_OBJECT_NOT_EXTENSIBLE);
  }

  RootedValue value(cx, env->getSlot(prop->slot()));
  if (value.isMagic(JS_UNINITIALIZED_LEXICAL)) {
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, id);
    return false;
  }

  return ValidatePropertyDescriptor(cx, desc, true, true, false, value, result);
}

bool ModuleNamespaceObject::ProxyHandler::has(JSContext* cx, HandleObject proxy,
                                              HandleId id, bool* bp) const {
  Rooted<ModuleNamespaceObject*> ns(cx, &proxy->as<ModuleNamespaceObject>());
  if (id.isSymbol()) {
    *bp = id.isWellKnownSymbol(JS::SymbolCode::toStringTag);
    return true;
  }

  *bp = ns->bindings().has(id);
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::get(JSContext* cx, HandleObject proxy,
                                              HandleValue receiver, HandleId id,
                                              MutableHandleValue vp) const {
  Rooted<ModuleNamespaceObject*> ns(cx, &proxy->as<ModuleNamespaceObject>());
  if (id.isSymbol()) {
    if (id.isWellKnownSymbol(JS::SymbolCode::toStringTag)) {
      vp.setString(cx->names().Module);
      return true;
    }

    vp.setUndefined();
    return true;
  }

  ModuleEnvironmentObject* env;
  mozilla::Maybe<PropertyInfo> prop;
  if (!ns->bindings().lookup(id, &env, &prop)) {
    vp.setUndefined();
    return true;
  }

  RootedValue value(cx, env->getSlot(prop->slot()));
  if (value.isMagic(JS_UNINITIALIZED_LEXICAL)) {
    ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, id);
    return false;
  }

  vp.set(value);
  return true;
}

bool ModuleNamespaceObject::ProxyHandler::set(JSContext* cx, HandleObject proxy,
                                              HandleId id, HandleValue v,
                                              HandleValue receiver,
                                              ObjectOpResult& result) const {
  return result.failReadOnly();
}

bool ModuleNamespaceObject::ProxyHandler::delete_(
    JSContext* cx, HandleObject proxy, HandleId id,
    ObjectOpResult& result) const {
  Rooted<ModuleNamespaceObject*> ns(cx, &proxy->as<ModuleNamespaceObject>());
  if (id.isSymbol()) {
    if (id.isWellKnownSymbol(JS::SymbolCode::toStringTag)) {
      return result.failCantDelete();
    }

    return result.succeed();
  }

  if (ns->bindings().has(id)) {
    return result.failCantDelete();
  }

  return result.succeed();
}

bool ModuleNamespaceObject::ProxyHandler::ownPropertyKeys(
    JSContext* cx, HandleObject proxy, MutableHandleIdVector props) const {
  Rooted<ModuleNamespaceObject*> ns(cx, &proxy->as<ModuleNamespaceObject>());
  uint32_t count = ns->exports().length();
  if (!props.reserve(props.length() + count + 1)) {
    return false;
  }

  for (JSAtom* atom : ns->exports()) {
    props.infallibleAppend(AtomToId(atom));
  }
  props.infallibleAppend(
      PropertyKey::Symbol(cx->wellKnownSymbols().toStringTag));

  return true;
}

void ModuleNamespaceObject::ProxyHandler::trace(JSTracer* trc,
                                                JSObject* proxy) const {
  auto& self = proxy->as<ModuleNamespaceObject>();

  if (self.hasExports()) {
    self.mutableExports().trace(trc);
  }

  if (self.hasBindings()) {
    self.bindings().trace(trc);
  }
}

void ModuleNamespaceObject::ProxyHandler::finalize(JS::GCContext* gcx,
                                                   JSObject* proxy) const {
  auto& self = proxy->as<ModuleNamespaceObject>();

  if (self.hasExports()) {
    gcx->delete_(proxy, &self.mutableExports(), MemoryUse::ModuleExports);
  }

  if (self.hasBindings()) {
    gcx->delete_(proxy, &self.bindings(), MemoryUse::ModuleBindingMap);
  }
}

///////////////////////////////////////////////////////////////////////////
// CyclicModuleFields

// The fields of a cyclic module record, as described in:
// https://tc39.es/ecma262/#sec-cyclic-module-records
class js::CyclicModuleFields {
 public:
  ModuleStatus status = ModuleStatus::Unlinked;

  bool hasTopLevelAwait : 1;

 private:
  // Flag bits that determine whether other fields are present.
  bool hasDfsIndex : 1;
  bool hasDfsAncestorIndex : 1;
  bool isAsyncEvaluating : 1;
  bool hasPendingAsyncDependencies : 1;

  // Fields whose presence is conditional on the flag bits above.
  uint32_t dfsIndex = 0;
  uint32_t dfsAncestorIndex = 0;
  uint32_t asyncEvaluatingPostOrder = 0;
  uint32_t pendingAsyncDependencies = 0;

  // Fields describing the layout of exportEntries.
  uint32_t indirectExportEntriesStart = 0;
  uint32_t starExportEntriesStart = 0;

 public:
  HeapPtr<Value> evaluationError;
  HeapPtr<JSObject*> metaObject;
  HeapPtr<ScriptSourceObject*> scriptSourceObject;
  RequestedModuleVector requestedModules;
  ImportEntryVector importEntries;
  ExportEntryVector exportEntries;
  IndirectBindingMap importBindings;
  UniquePtr<FunctionDeclarationVector> functionDeclarations;
  HeapPtr<PromiseObject*> topLevelCapability;
  HeapPtr<ListObject*> asyncParentModules;
  HeapPtr<ModuleObject*> cycleRoot;

 public:
  CyclicModuleFields();

  void trace(JSTracer* trc);

  void initExportEntries(MutableHandle<ExportEntryVector> allEntries,
                         uint32_t localExportCount,
                         uint32_t indirectExportCount,
                         uint32_t starExportCount);
  Span<const ExportEntry> localExportEntries() const;
  Span<const ExportEntry> indirectExportEntries() const;
  Span<const ExportEntry> starExportEntries() const;

  void setDfsIndex(uint32_t index);
  Maybe<uint32_t> maybeDfsIndex() const;
  void setDfsAncestorIndex(uint32_t index);
  Maybe<uint32_t> maybeDfsAncestorIndex() const;
  void clearDfsIndexes();

  void setAsyncEvaluating(uint32_t postOrder);
  bool getIsAsyncEvaluating() const;
  Maybe<uint32_t> maybeAsyncEvaluatingPostOrder() const;
  void clearAsyncEvaluatingPostOrder();

  void setPendingAsyncDependencies(uint32_t newValue);
  Maybe<uint32_t> maybePendingAsyncDependencies() const;
};

CyclicModuleFields::CyclicModuleFields()
    : hasTopLevelAwait(false),
      hasDfsIndex(false),
      hasDfsAncestorIndex(false),
      isAsyncEvaluating(false),
      hasPendingAsyncDependencies(false) {}

void CyclicModuleFields::trace(JSTracer* trc) {
  TraceEdge(trc, &evaluationError, "CyclicModuleFields::evaluationError");
  TraceNullableEdge(trc, &metaObject, "CyclicModuleFields::metaObject");
  TraceNullableEdge(trc, &scriptSourceObject,
                    "CyclicModuleFields::scriptSourceObject");
  requestedModules.trace(trc);
  importEntries.trace(trc);
  exportEntries.trace(trc);
  importBindings.trace(trc);
  TraceNullableEdge(trc, &topLevelCapability,
                    "CyclicModuleFields::topLevelCapability");
  TraceNullableEdge(trc, &asyncParentModules,
                    "CyclicModuleFields::asyncParentModules");
  TraceNullableEdge(trc, &cycleRoot, "CyclicModuleFields::cycleRoot");
}

void CyclicModuleFields::initExportEntries(
    MutableHandle<ExportEntryVector> allEntries, uint32_t localExportCount,
    uint32_t indirectExportCount, uint32_t starExportCount) {
  MOZ_ASSERT(allEntries.length() ==
             localExportCount + indirectExportCount + starExportCount);

  exportEntries = std::move(allEntries.get());
  indirectExportEntriesStart = localExportCount;
  starExportEntriesStart = indirectExportEntriesStart + indirectExportCount;
}

Span<const ExportEntry> CyclicModuleFields::localExportEntries() const {
  MOZ_ASSERT(indirectExportEntriesStart <= exportEntries.length());
  return Span(exportEntries.begin(),
              exportEntries.begin() + indirectExportEntriesStart);
}

Span<const ExportEntry> CyclicModuleFields::indirectExportEntries() const {
  MOZ_ASSERT(indirectExportEntriesStart <= starExportEntriesStart);
  MOZ_ASSERT(starExportEntriesStart <= exportEntries.length());
  return Span(exportEntries.begin() + indirectExportEntriesStart,
              exportEntries.begin() + starExportEntriesStart);
}

Span<const ExportEntry> CyclicModuleFields::starExportEntries() const {
  MOZ_ASSERT(starExportEntriesStart <= exportEntries.length());
  return Span(exportEntries.begin() + starExportEntriesStart,
              exportEntries.end());
}

void CyclicModuleFields::setDfsIndex(uint32_t index) {
  dfsIndex = index;
  hasDfsIndex = true;
}

Maybe<uint32_t> CyclicModuleFields::maybeDfsIndex() const {
  return hasDfsIndex ? Some(dfsIndex) : Nothing();
}

void CyclicModuleFields::setDfsAncestorIndex(uint32_t index) {
  dfsAncestorIndex = index;
  hasDfsAncestorIndex = true;
}

Maybe<uint32_t> CyclicModuleFields::maybeDfsAncestorIndex() const {
  return hasDfsAncestorIndex ? Some(dfsAncestorIndex) : Nothing();
}

void CyclicModuleFields::clearDfsIndexes() {
  dfsIndex = 0;
  hasDfsIndex = false;
  dfsAncestorIndex = 0;
  hasDfsAncestorIndex = false;
}

void CyclicModuleFields::setAsyncEvaluating(uint32_t postOrder) {
  isAsyncEvaluating = true;
  asyncEvaluatingPostOrder = postOrder;
}

bool CyclicModuleFields::getIsAsyncEvaluating() const {
  return isAsyncEvaluating;
}

Maybe<uint32_t> CyclicModuleFields::maybeAsyncEvaluatingPostOrder() const {
  if (!isAsyncEvaluating ||
      asyncEvaluatingPostOrder == ASYNC_EVALUATING_POST_ORDER_CLEARED) {
    return Nothing();
  }

  return Some(asyncEvaluatingPostOrder);
}

void CyclicModuleFields::clearAsyncEvaluatingPostOrder() {
  asyncEvaluatingPostOrder = ASYNC_EVALUATING_POST_ORDER_CLEARED;
}

void CyclicModuleFields::setPendingAsyncDependencies(uint32_t newValue) {
  pendingAsyncDependencies = newValue;
  hasPendingAsyncDependencies = true;
}

Maybe<uint32_t> CyclicModuleFields::maybePendingAsyncDependencies() const {
  return hasPendingAsyncDependencies ? Some(pendingAsyncDependencies)
                                     : Nothing();
}

///////////////////////////////////////////////////////////////////////////
// ModuleObject

/* static */ const JSClassOps ModuleObject::classOps_ = {
    nullptr,                 // addProperty
    nullptr,                 // delProperty
    nullptr,                 // enumerate
    nullptr,                 // newEnumerate
    nullptr,                 // resolve
    nullptr,                 // mayResolve
    ModuleObject::finalize,  // finalize
    nullptr,                 // call
    nullptr,                 // construct
    ModuleObject::trace,     // trace
};

/* static */ const JSClass ModuleObject::class_ = {
    "Module",
    JSCLASS_HAS_RESERVED_SLOTS(ModuleObject::SlotCount) |
        JSCLASS_BACKGROUND_FINALIZE,
    &ModuleObject::classOps_};

/* static */
bool ModuleObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ModuleObject>();
}

bool ModuleObject::hasCyclicModuleFields() const {
  // This currently only returns false if we GC during initialization.
  return !getReservedSlot(CyclicModuleFieldsSlot).isUndefined();
}

CyclicModuleFields* ModuleObject::cyclicModuleFields() {
  void* ptr = getReservedSlot(CyclicModuleFieldsSlot).toPrivate();
  MOZ_ASSERT(ptr);
  return static_cast<CyclicModuleFields*>(ptr);
}
const CyclicModuleFields* ModuleObject::cyclicModuleFields() const {
  return const_cast<ModuleObject*>(this)->cyclicModuleFields();
}

Span<const RequestedModule> ModuleObject::requestedModules() const {
  return cyclicModuleFields()->requestedModules;
}

Span<const ImportEntry> ModuleObject::importEntries() const {
  return cyclicModuleFields()->importEntries;
}

Span<const ExportEntry> ModuleObject::localExportEntries() const {
  return cyclicModuleFields()->localExportEntries();
}

Span<const ExportEntry> ModuleObject::indirectExportEntries() const {
  return cyclicModuleFields()->indirectExportEntries();
}

Span<const ExportEntry> ModuleObject::starExportEntries() const {
  return cyclicModuleFields()->starExportEntries();
}

void ModuleObject::initFunctionDeclarations(
    UniquePtr<FunctionDeclarationVector> decls) {
  cyclicModuleFields()->functionDeclarations = std::move(decls);
}

/* static */
ModuleObject* ModuleObject::create(JSContext* cx) {
  Rooted<UniquePtr<CyclicModuleFields>> fields(cx);
  fields = cx->make_unique<CyclicModuleFields>();
  if (!fields) {
    return nullptr;
  }

  Rooted<ModuleObject*> self(
      cx, NewObjectWithGivenProto<ModuleObject>(cx, nullptr));
  if (!self) {
    return nullptr;
  }

  InitReservedSlot(self, CyclicModuleFieldsSlot, fields.release(),
                   MemoryUse::ModuleCyclicFields);

  return self;
}

/* static */
void ModuleObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  ModuleObject* self = &obj->as<ModuleObject>();
  if (self->hasCyclicModuleFields()) {
    gcx->delete_(obj, self->cyclicModuleFields(),
                 MemoryUse::ModuleCyclicFields);
  }
}

ModuleEnvironmentObject& ModuleObject::initialEnvironment() const {
  Value value = getReservedSlot(EnvironmentSlot);
  return value.toObject().as<ModuleEnvironmentObject>();
}

ModuleEnvironmentObject* ModuleObject::environment() const {
  // Note that this it's valid to call this even if there was an error
  // evaluating the module.

  // According to the spec the environment record is created during linking, but
  // we create it earlier than that.
  if (status() < ModuleStatus::Linked) {
    return nullptr;
  }

  return &initialEnvironment();
}

IndirectBindingMap& ModuleObject::importBindings() {
  return cyclicModuleFields()->importBindings;
}

ModuleNamespaceObject* ModuleObject::namespace_() {
  Value value = getReservedSlot(NamespaceSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject().as<ModuleNamespaceObject>();
}

ScriptSourceObject* ModuleObject::scriptSourceObject() const {
  return cyclicModuleFields()->scriptSourceObject;
}

void ModuleObject::initAsyncSlots(JSContext* cx, bool hasTopLevelAwait,
                                  Handle<ListObject*> asyncParentModules) {
  cyclicModuleFields()->hasTopLevelAwait = hasTopLevelAwait;
  cyclicModuleFields()->asyncParentModules = asyncParentModules;
}

static uint32_t NextPostOrder(JSRuntime* rt) {
  uint32_t ordinal = rt->moduleAsyncEvaluatingPostOrder;
  MOZ_ASSERT(ordinal != ASYNC_EVALUATING_POST_ORDER_CLEARED);
  MOZ_ASSERT(ordinal < MAX_UINT32);
  rt->moduleAsyncEvaluatingPostOrder++;
  return ordinal;
}

// Reset the runtime's moduleAsyncEvaluatingPostOrder counter when the last
// module that was async evaluating is finished.
//
// The graph is not re-entrant and any future modules will be independent from
// this one.
static void MaybeResetPostOrderCounter(JSRuntime* rt,
                                       uint32_t finishedPostOrder) {
  if (rt->moduleAsyncEvaluatingPostOrder == finishedPostOrder + 1) {
    rt->moduleAsyncEvaluatingPostOrder = ASYNC_EVALUATING_POST_ORDER_INIT;
  }
}

void ModuleObject::setAsyncEvaluating() {
  MOZ_ASSERT(!isAsyncEvaluating());
  uint32_t postOrder = NextPostOrder(runtimeFromMainThread());
  cyclicModuleFields()->setAsyncEvaluating(postOrder);
}

void ModuleObject::initScriptSlots(HandleScript script) {
  MOZ_ASSERT(script);
  MOZ_ASSERT(script->sourceObject());
  initReservedSlot(ScriptSlot, PrivateGCThingValue(script));
  cyclicModuleFields()->scriptSourceObject = script->sourceObject();
}

void ModuleObject::setInitialEnvironment(
    Handle<ModuleEnvironmentObject*> initialEnvironment) {
  initReservedSlot(EnvironmentSlot, ObjectValue(*initialEnvironment));
}

void ModuleObject::initImportExportData(
    MutableHandle<RequestedModuleVector> requestedModules,
    MutableHandle<ImportEntryVector> importEntries,
    MutableHandle<ExportEntryVector> exportEntries, uint32_t localExportCount,
    uint32_t indirectExportCount, uint32_t starExportCount) {
  cyclicModuleFields()->requestedModules = std::move(requestedModules.get());
  cyclicModuleFields()->importEntries = std::move(importEntries.get());
  cyclicModuleFields()->initExportEntries(exportEntries, localExportCount,
                                          indirectExportCount, starExportCount);
}

/* static */
bool ModuleObject::Freeze(JSContext* cx, Handle<ModuleObject*> self) {
  return FreezeObject(cx, self);
}

#ifdef DEBUG
/* static */ inline bool ModuleObject::AssertFrozen(
    JSContext* cx, Handle<ModuleObject*> self) {
  bool frozen = false;
  if (!TestIntegrityLevel(cx, self, IntegrityLevel::Frozen, &frozen)) {
    return false;
  }
  MOZ_ASSERT(frozen);

  return true;
}
#endif

JSScript* ModuleObject::maybeScript() const {
  Value value = getReservedSlot(ScriptSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  BaseScript* script = value.toGCThing()->as<BaseScript>();
  MOZ_ASSERT(script->hasBytecode(),
             "Module scripts should always have bytecode");
  return script->asJSScript();
}

JSScript* ModuleObject::script() const {
  JSScript* ptr = maybeScript();
  MOZ_RELEASE_ASSERT(ptr);
  return ptr;
}

static inline void AssertValidModuleStatus(ModuleStatus status) {
  MOZ_ASSERT(status >= ModuleStatus::Unlinked &&
             status <= ModuleStatus::Evaluated_Error);
}

ModuleStatus ModuleObject::status() const {
  // TODO: When implementing synthetic module records it may be convenient to
  // make this method always return a ModuleStatus::Evaluated for such a module
  // so we can assert a module's status without checking which kind it is, even
  // though synthetic modules don't have this field according to the spec.

  ModuleStatus status = cyclicModuleFields()->status;
  AssertValidModuleStatus(status);

  if (status == ModuleStatus::Evaluated_Error) {
    return ModuleStatus::Evaluated;
  }

  return status;
}

void ModuleObject::setStatus(ModuleStatus newStatus) {
  AssertValidModuleStatus(newStatus);

  // Note that under OOM conditions we can fail the module linking process even
  // after modules have been marked as linked.
  MOZ_ASSERT((status() <= ModuleStatus::Linked &&
              newStatus == ModuleStatus::Unlinked) ||
                 newStatus > status(),
             "New module status inconsistent with current status");

  cyclicModuleFields()->status = newStatus;
}

bool ModuleObject::hasTopLevelAwait() const {
  return cyclicModuleFields()->hasTopLevelAwait;
}

bool ModuleObject::isAsyncEvaluating() const {
  return cyclicModuleFields()->getIsAsyncEvaluating();
}

Maybe<uint32_t> ModuleObject::maybeDfsIndex() const {
  return cyclicModuleFields()->maybeDfsIndex();
}

uint32_t ModuleObject::dfsIndex() const { return maybeDfsIndex().value(); }

void ModuleObject::setDfsIndex(uint32_t index) {
  cyclicModuleFields()->setDfsIndex(index);
}

Maybe<uint32_t> ModuleObject::maybeDfsAncestorIndex() const {
  return cyclicModuleFields()->maybeDfsAncestorIndex();
}

uint32_t ModuleObject::dfsAncestorIndex() const {
  return maybeDfsAncestorIndex().value();
}

void ModuleObject::setDfsAncestorIndex(uint32_t index) {
  cyclicModuleFields()->setDfsAncestorIndex(index);
}

void ModuleObject::clearDfsIndexes() {
  cyclicModuleFields()->clearDfsIndexes();
}

PromiseObject* ModuleObject::maybeTopLevelCapability() const {
  return cyclicModuleFields()->topLevelCapability;
}

PromiseObject* ModuleObject::topLevelCapability() const {
  PromiseObject* capability = maybeTopLevelCapability();
  MOZ_RELEASE_ASSERT(capability);
  return capability;
}

// static
PromiseObject* ModuleObject::createTopLevelCapability(
    JSContext* cx, Handle<ModuleObject*> module) {
  MOZ_ASSERT(!module->maybeTopLevelCapability());

  Rooted<PromiseObject*> resultPromise(cx, CreatePromiseObjectForAsync(cx));
  if (!resultPromise) {
    return nullptr;
  }

  module->setInitialTopLevelCapability(resultPromise);
  return resultPromise;
}

void ModuleObject::setInitialTopLevelCapability(
    Handle<PromiseObject*> capability) {
  cyclicModuleFields()->topLevelCapability = capability;
}

ListObject* ModuleObject::asyncParentModules() const {
  return cyclicModuleFields()->asyncParentModules;
}

bool ModuleObject::appendAsyncParentModule(JSContext* cx,
                                           Handle<ModuleObject*> self,
                                           Handle<ModuleObject*> parent) {
  Rooted<Value> parentValue(cx, ObjectValue(*parent));
  return self->asyncParentModules()->append(cx, parentValue);
}

Maybe<uint32_t> ModuleObject::maybePendingAsyncDependencies() const {
  return cyclicModuleFields()->maybePendingAsyncDependencies();
}

uint32_t ModuleObject::pendingAsyncDependencies() const {
  return maybePendingAsyncDependencies().value();
}

Maybe<uint32_t> ModuleObject::maybeAsyncEvaluatingPostOrder() const {
  return cyclicModuleFields()->maybeAsyncEvaluatingPostOrder();
}

uint32_t ModuleObject::getAsyncEvaluatingPostOrder() const {
  return cyclicModuleFields()->maybeAsyncEvaluatingPostOrder().value();
}

void ModuleObject::clearAsyncEvaluatingPostOrder() {
  MOZ_ASSERT(status() == ModuleStatus::Evaluated);

  JSRuntime* rt = runtimeFromMainThread();
  MaybeResetPostOrderCounter(rt, getAsyncEvaluatingPostOrder());

  cyclicModuleFields()->clearAsyncEvaluatingPostOrder();
}

void ModuleObject::setPendingAsyncDependencies(uint32_t newValue) {
  cyclicModuleFields()->setPendingAsyncDependencies(newValue);
}

void ModuleObject::setCycleRoot(ModuleObject* cycleRoot) {
  cyclicModuleFields()->cycleRoot = cycleRoot;
}

ModuleObject* ModuleObject::getCycleRoot() const {
  MOZ_RELEASE_ASSERT(cyclicModuleFields()->cycleRoot);
  return cyclicModuleFields()->cycleRoot;
}

bool ModuleObject::hasTopLevelCapability() const {
  return cyclicModuleFields()->topLevelCapability;
}

bool ModuleObject::hadEvaluationError() const {
  ModuleStatus fullStatus = cyclicModuleFields()->status;
  return fullStatus == ModuleStatus::Evaluated_Error;
}

void ModuleObject::setEvaluationError(HandleValue newValue) {
  MOZ_ASSERT(status() != ModuleStatus::Unlinked);
  MOZ_ASSERT(!hadEvaluationError());

  cyclicModuleFields()->status = ModuleStatus::Evaluated_Error;
  cyclicModuleFields()->evaluationError = newValue;

  MOZ_ASSERT(status() == ModuleStatus::Evaluated);
  MOZ_ASSERT(hadEvaluationError());
}

Value ModuleObject::maybeEvaluationError() const {
  return cyclicModuleFields()->evaluationError;
}

Value ModuleObject::evaluationError() const {
  MOZ_ASSERT(hadEvaluationError());
  return maybeEvaluationError();
}

JSObject* ModuleObject::metaObject() const {
  return cyclicModuleFields()->metaObject;
}

void ModuleObject::setMetaObject(JSObject* obj) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(!metaObject());
  cyclicModuleFields()->metaObject = obj;
}

/* static */
void ModuleObject::trace(JSTracer* trc, JSObject* obj) {
  ModuleObject& module = obj->as<ModuleObject>();
  if (module.hasCyclicModuleFields()) {
    module.cyclicModuleFields()->trace(trc);
  }
}

/* static */
bool ModuleObject::instantiateFunctionDeclarations(JSContext* cx,
                                                   Handle<ModuleObject*> self) {
#ifdef DEBUG
  MOZ_ASSERT(self->status() == ModuleStatus::Linking);
  if (!AssertFrozen(cx, self)) {
    return false;
  }
#endif
  // |self| initially manages this vector.
  UniquePtr<FunctionDeclarationVector>& funDecls =
      self->cyclicModuleFields()->functionDeclarations;
  if (!funDecls) {
    JS_ReportErrorASCII(
        cx, "Module function declarations have already been instantiated");
    return false;
  }

  Rooted<ModuleEnvironmentObject*> env(cx, &self->initialEnvironment());
  RootedObject obj(cx);
  RootedValue value(cx);
  RootedFunction fun(cx);
  Rooted<PropertyName*> name(cx);

  for (GCThingIndex funIndex : *funDecls) {
    fun.set(self->script()->getFunction(funIndex));
    obj = Lambda(cx, fun, env);
    if (!obj) {
      return false;
    }

    name = fun->explicitName()->asPropertyName();
    value = ObjectValue(*obj);
    if (!SetProperty(cx, env, name, value)) {
      return false;
    }
  }

  // Free the vector, now its contents are no longer needed.
  funDecls.reset();

  return true;
}

/* static */
bool ModuleObject::execute(JSContext* cx, Handle<ModuleObject*> self) {
#ifdef DEBUG
  MOZ_ASSERT(self->status() == ModuleStatus::Evaluating ||
             self->status() == ModuleStatus::EvaluatingAsync ||
             self->status() == ModuleStatus::Evaluated);
  MOZ_ASSERT(!self->hadEvaluationError());
  if (!AssertFrozen(cx, self)) {
    return false;
  }
#endif

  RootedScript script(cx, self->script());

  auto guardA = mozilla::MakeScopeExit([&] {
    if (self->hasTopLevelAwait()) {
      // Handled in AsyncModuleExecutionFulfilled and
      // AsyncModuleExecutionRejected.
      return;
    }
    ModuleObject::onTopLevelEvaluationFinished(self);
  });

  Rooted<ModuleEnvironmentObject*> env(cx, self->environment());
  if (!env) {
    JS_ReportErrorASCII(cx,
                        "Module declarations have not yet been instantiated");
    return false;
  }

  Rooted<Value> ignored(cx);
  return Execute(cx, script, env, &ignored);
}

/* static */
void ModuleObject::onTopLevelEvaluationFinished(ModuleObject* module) {
  // ScriptSlot is used by debugger to access environments during evaluating
  // the top-level script.
  // Clear the reference at exit to prevent us keeping this alive unnecessarily.
  module->setReservedSlot(ScriptSlot, UndefinedValue());
}

/* static */
ModuleNamespaceObject* ModuleObject::createNamespace(
    JSContext* cx, Handle<ModuleObject*> self,
    MutableHandle<UniquePtr<ExportNameVector>> exports) {
  MOZ_ASSERT(!self->namespace_());

  Rooted<UniquePtr<IndirectBindingMap>> bindings(cx);
  bindings = cx->make_unique<IndirectBindingMap>();
  if (!bindings) {
    return nullptr;
  }

  auto* ns = ModuleNamespaceObject::create(cx, self, exports, &bindings);
  if (!ns) {
    return nullptr;
  }

  self->initReservedSlot(NamespaceSlot, ObjectValue(*ns));
  return ns;
}

/* static */
bool ModuleObject::createEnvironment(JSContext* cx,
                                     Handle<ModuleObject*> self) {
  Rooted<ModuleEnvironmentObject*> env(
      cx, ModuleEnvironmentObject::create(cx, self));
  if (!env) {
    return false;
  }

  self->setInitialEnvironment(env);
  return true;
}

///////////////////////////////////////////////////////////////////////////
// ModuleBuilder

ModuleBuilder::ModuleBuilder(FrontendContext* fc,
                             const frontend::EitherParser& eitherParser)
    : fc_(fc),
      eitherParser_(eitherParser),
      requestedModuleSpecifiers_(fc),
      importEntries_(fc),
      exportEntries_(fc),
      exportNames_(fc) {}

bool ModuleBuilder::noteFunctionDeclaration(FrontendContext* fc,
                                            uint32_t funIndex) {
  if (!functionDecls_.emplaceBack(funIndex)) {
    js::ReportOutOfMemory(fc);
    return false;
  }
  return true;
}

void ModuleBuilder::noteAsync(frontend::StencilModuleMetadata& metadata) {
  metadata.isAsync = true;
}

bool ModuleBuilder::buildTables(frontend::StencilModuleMetadata& metadata) {
  // https://tc39.es/ecma262/#sec-parsemodule
  // 15.2.1.17.1 ParseModule, Steps 4-11.

  // Step 4.
  metadata.moduleRequests = std::move(moduleRequests_);
  metadata.requestedModules = std::move(requestedModules_);

  // Step 5.
  if (!metadata.importEntries.reserve(importEntries_.count())) {
    js::ReportOutOfMemory(fc_);
    return false;
  }
  for (auto r = importEntries_.all(); !r.empty(); r.popFront()) {
    frontend::StencilModuleEntry& entry = r.front().value();
    metadata.importEntries.infallibleAppend(entry);
  }

  // Steps 6-11.
  for (const frontend::StencilModuleEntry& exp : exportEntries_) {
    if (!exp.moduleRequest) {
      frontend::StencilModuleEntry* importEntry = importEntryFor(exp.localName);
      if (!importEntry) {
        if (!metadata.localExportEntries.append(exp)) {
          js::ReportOutOfMemory(fc_);
          return false;
        }
      } else {
        if (!importEntry->importName) {
          if (!metadata.localExportEntries.append(exp)) {
            js::ReportOutOfMemory(fc_);
            return false;
          }
        } else {
          // All names should have already been marked as used-by-stencil.
          auto entry = frontend::StencilModuleEntry::exportFromEntry(
              importEntry->moduleRequest, importEntry->importName,
              exp.exportName, exp.lineno, exp.column);
          if (!metadata.indirectExportEntries.append(entry)) {
            js::ReportOutOfMemory(fc_);
            return false;
          }
        }
      }
    } else if (!exp.importName && !exp.exportName) {
      if (!metadata.starExportEntries.append(exp)) {
        js::ReportOutOfMemory(fc_);
        return false;
      }
    } else {
      if (!metadata.indirectExportEntries.append(exp)) {
        js::ReportOutOfMemory(fc_);
        return false;
      }
    }
  }

  return true;
}

void ModuleBuilder::finishFunctionDecls(
    frontend::StencilModuleMetadata& metadata) {
  metadata.functionDecls = std::move(functionDecls_);
}

bool frontend::StencilModuleMetadata::createModuleRequestObjects(
    JSContext* cx, CompilationAtomCache& atomCache,
    MutableHandle<ModuleRequestVector> output) const {
  if (!output.reserve(moduleRequests.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<ModuleRequestObject*> object(cx);
  for (const StencilModuleRequest& request : moduleRequests) {
    object = createModuleRequestObject(cx, atomCache, request);
    if (!object) {
      return false;
    }

    output.infallibleEmplaceBack(object);
  }

  return true;
}

ModuleRequestObject* frontend::StencilModuleMetadata::createModuleRequestObject(
    JSContext* cx, CompilationAtomCache& atomCache,
    const StencilModuleRequest& request) const {
  Rooted<ArrayObject*> assertionArray(cx);
  uint32_t numberOfAssertions = request.assertions.length();
  if (numberOfAssertions > 0) {
    assertionArray = NewDenseFullyAllocatedArray(cx, numberOfAssertions);
    if (!assertionArray) {
      return nullptr;
    }
    assertionArray->ensureDenseInitializedLength(0, numberOfAssertions);

    Rooted<PlainObject*> assertionObject(cx);
    RootedId assertionKey(cx);
    RootedValue assertionValue(cx);
    for (uint32_t j = 0; j < numberOfAssertions; ++j) {
      assertionObject = NewPlainObject(cx);
      if (!assertionObject) {
        return nullptr;
      }

      JSAtom* jsatom =
          atomCache.getExistingAtomAt(cx, request.assertions[j].key);
      MOZ_ASSERT(jsatom);
      assertionKey = AtomToId(jsatom);

      jsatom = atomCache.getExistingAtomAt(cx, request.assertions[j].value);
      MOZ_ASSERT(jsatom);
      assertionValue = StringValue(jsatom);

      if (!DefineDataProperty(cx, assertionObject, assertionKey, assertionValue,
                              JSPROP_ENUMERATE)) {
        return nullptr;
      }

      assertionArray->initDenseElement(j, ObjectValue(*assertionObject));
    }
  }

  Rooted<JSAtom*> specifier(cx,
                            atomCache.getExistingAtomAt(cx, request.specifier));
  MOZ_ASSERT(specifier);

  return ModuleRequestObject::create(cx, specifier, assertionArray);
}

bool frontend::StencilModuleMetadata::createImportEntries(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<ModuleRequestVector> moduleRequests,
    MutableHandle<ImportEntryVector> output) const {
  if (!output.reserve(importEntries.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (const StencilModuleEntry& entry : importEntries) {
    Rooted<ModuleRequestObject*> moduleRequest(cx);
    moduleRequest = moduleRequests[entry.moduleRequest.value()].get();
    MOZ_ASSERT(moduleRequest);

    Rooted<JSAtom*> localName(cx);
    if (entry.localName) {
      localName = atomCache.getExistingAtomAt(cx, entry.localName);
      MOZ_ASSERT(localName);
    }

    Rooted<JSAtom*> importName(cx);
    if (entry.importName) {
      importName = atomCache.getExistingAtomAt(cx, entry.importName);
      MOZ_ASSERT(importName);
    }

    MOZ_ASSERT(!entry.exportName);

    output.infallibleEmplaceBack(moduleRequest, importName, localName,
                                 entry.lineno, entry.column);
  }

  return true;
}

bool frontend::StencilModuleMetadata::createExportEntries(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    Handle<ModuleRequestVector> moduleRequests,
    const frontend::StencilModuleMetadata::EntryVector& input,
    MutableHandle<ExportEntryVector> output) const {
  if (!output.reserve(output.length() + input.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (const frontend::StencilModuleEntry& entry : input) {
    Rooted<JSAtom*> exportName(cx);
    if (entry.exportName) {
      exportName = atomCache.getExistingAtomAt(cx, entry.exportName);
      MOZ_ASSERT(exportName);
    }

    Rooted<ModuleRequestObject*> moduleRequestObject(cx);
    if (entry.moduleRequest) {
      moduleRequestObject = moduleRequests[entry.moduleRequest.value()].get();
      MOZ_ASSERT(moduleRequestObject);
    }

    Rooted<JSAtom*> localName(cx);
    if (entry.localName) {
      localName = atomCache.getExistingAtomAt(cx, entry.localName);
      MOZ_ASSERT(localName);
    }

    Rooted<JSAtom*> importName(cx);
    if (entry.importName) {
      importName = atomCache.getExistingAtomAt(cx, entry.importName);
      MOZ_ASSERT(importName);
    }

    output.infallibleEmplaceBack(exportName, moduleRequestObject, importName,
                                 localName, entry.lineno, entry.column);
  }

  return true;
}

bool frontend::StencilModuleMetadata::createRequestedModules(
    JSContext* cx, CompilationAtomCache& atomCache,
    Handle<ModuleRequestVector> moduleRequests,
    MutableHandle<RequestedModuleVector> output) const {
  if (!output.reserve(requestedModules.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (const frontend::StencilModuleEntry& entry : requestedModules) {
    Rooted<ModuleRequestObject*> moduleRequest(cx);
    moduleRequest = moduleRequests[entry.moduleRequest.value()].get();
    MOZ_ASSERT(moduleRequest);

    MOZ_ASSERT(!entry.localName);
    MOZ_ASSERT(!entry.importName);
    MOZ_ASSERT(!entry.exportName);

    output.infallibleEmplaceBack(moduleRequest, entry.lineno, entry.column);
  }

  return true;
}

// Use StencilModuleMetadata data to fill in ModuleObject
bool frontend::StencilModuleMetadata::initModule(
    JSContext* cx, FrontendContext* fc,
    frontend::CompilationAtomCache& atomCache,
    JS::Handle<ModuleObject*> module) const {
  Rooted<ModuleRequestVector> moduleRequestsVector(cx);
  if (!createModuleRequestObjects(cx, atomCache, &moduleRequestsVector)) {
    return false;
  }

  Rooted<RequestedModuleVector> requestedModulesVector(cx);
  if (!createRequestedModules(cx, atomCache, moduleRequestsVector,
                              &requestedModulesVector)) {
    return false;
  }

  Rooted<ImportEntryVector> importEntriesVector(cx);
  if (!createImportEntries(cx, atomCache, moduleRequestsVector,
                           &importEntriesVector)) {
    return false;
  }

  Rooted<ExportEntryVector> exportEntriesVector(cx);
  if (!createExportEntries(cx, atomCache, moduleRequestsVector,
                           localExportEntries, &exportEntriesVector)) {
    return false;
  }

  Rooted<ExportEntryVector> indirectExportEntriesVector(cx);
  if (!createExportEntries(cx, atomCache, moduleRequestsVector,
                           indirectExportEntries, &exportEntriesVector)) {
    return false;
  }

  Rooted<ExportEntryVector> starExportEntriesVector(cx);
  if (!createExportEntries(cx, atomCache, moduleRequestsVector,
                           starExportEntries, &exportEntriesVector)) {
    return false;
  }

  // Copy the vector of declarations to the ModuleObject.
  auto functionDeclsCopy = MakeUnique<FunctionDeclarationVector>();
  if (!functionDeclsCopy || !functionDeclsCopy->appendAll(functionDecls)) {
    js::ReportOutOfMemory(fc);
    return false;
  }
  module->initFunctionDeclarations(std::move(functionDeclsCopy));

  Rooted<ListObject*> asyncParentModulesList(cx, ListObject::create(cx));
  if (!asyncParentModulesList) {
    return false;
  }

  module->initAsyncSlots(cx, isAsync, asyncParentModulesList);

  module->initImportExportData(
      &requestedModulesVector, &importEntriesVector, &exportEntriesVector,
      localExportEntries.length(), indirectExportEntries.length(),
      starExportEntries.length());

  return true;
}

bool ModuleBuilder::isAssertionSupported(JS::ImportAssertion supportedAssertion,
                                         frontend::TaggedParserAtomIndex key) {
  if (!key.isWellKnownAtomId()) {
    return false;
  }

  bool result = false;

  switch (supportedAssertion) {
    case JS::ImportAssertion::Type:
      result = key.toWellKnownAtomId() == WellKnownAtomId::type;
      break;
  }

  return result;
}

bool ModuleBuilder::processAssertions(frontend::StencilModuleRequest& request,
                                      frontend::ListNode* assertionList) {
  using namespace js::frontend;

  for (ParseNode* assertionItem : assertionList->contents()) {
    BinaryNode* assertion = &assertionItem->as<BinaryNode>();
    MOZ_ASSERT(assertion->isKind(ParseNodeKind::ImportAssertion));

    auto key = assertion->left()->as<NameNode>().atom();
    auto value = assertion->right()->as<NameNode>().atom();

    for (JS::ImportAssertion assertion : fc_->getSupportedImportAssertions()) {
      if (isAssertionSupported(assertion, key)) {
        markUsedByStencil(key);
        markUsedByStencil(value);

        StencilModuleAssertion assertionStencil(key, value);
        if (!request.assertions.append(assertionStencil)) {
          js::ReportOutOfMemory(fc_);
          return false;
        }
      }
    }
  }

  return true;
}

bool ModuleBuilder::processImport(frontend::BinaryNode* importNode) {
  using namespace js::frontend;

  MOZ_ASSERT(importNode->isKind(ParseNodeKind::ImportDecl));

  auto* specList = &importNode->left()->as<ListNode>();
  MOZ_ASSERT(specList->isKind(ParseNodeKind::ImportSpecList));

  auto* moduleRequest = &importNode->right()->as<BinaryNode>();
  MOZ_ASSERT(moduleRequest->isKind(ParseNodeKind::ImportModuleRequest));

  auto* moduleSpec = &moduleRequest->left()->as<NameNode>();
  MOZ_ASSERT(moduleSpec->isKind(ParseNodeKind::StringExpr));

  auto* assertionList = &moduleRequest->right()->as<ListNode>();
  MOZ_ASSERT(assertionList->isKind(ParseNodeKind::ImportAssertionList));

  auto specifier = moduleSpec->atom();
  MaybeModuleRequestIndex moduleRequestIndex =
      appendModuleRequest(specifier, assertionList);
  if (!moduleRequestIndex.isSome()) {
    return false;
  }

  if (!maybeAppendRequestedModule(moduleRequestIndex, moduleSpec)) {
    return false;
  }

  for (ParseNode* item : specList->contents()) {
    uint32_t line;
    uint32_t column;
    eitherParser_.computeLineAndColumn(item->pn_pos.begin, &line, &column);

    StencilModuleEntry entry;
    TaggedParserAtomIndex localName;
    if (item->isKind(ParseNodeKind::ImportSpec)) {
      auto* spec = &item->as<BinaryNode>();

      auto* importNameNode = &spec->left()->as<NameNode>();
      auto* localNameNode = &spec->right()->as<NameNode>();

      auto importName = importNameNode->atom();
      localName = localNameNode->atom();

      markUsedByStencil(localName);
      markUsedByStencil(importName);
      entry = StencilModuleEntry::importEntry(moduleRequestIndex, localName,
                                              importName, line, column);
    } else {
      MOZ_ASSERT(item->isKind(ParseNodeKind::ImportNamespaceSpec));
      auto* spec = &item->as<UnaryNode>();

      auto* localNameNode = &spec->kid()->as<NameNode>();

      localName = localNameNode->atom();

      markUsedByStencil(localName);
      entry = StencilModuleEntry::importNamespaceEntry(moduleRequestIndex,
                                                       localName, line, column);
    }

    if (!importEntries_.put(localName, entry)) {
      return false;
    }
  }

  return true;
}

bool ModuleBuilder::processExport(frontend::ParseNode* exportNode) {
  using namespace js::frontend;

  MOZ_ASSERT(exportNode->isKind(ParseNodeKind::ExportStmt) ||
             exportNode->isKind(ParseNodeKind::ExportDefaultStmt));

  bool isDefault = exportNode->isKind(ParseNodeKind::ExportDefaultStmt);
  ParseNode* kid = isDefault ? exportNode->as<BinaryNode>().left()
                             : exportNode->as<UnaryNode>().kid();

  if (isDefault && exportNode->as<BinaryNode>().right()) {
    // This is an export default containing an expression.
    auto localName = TaggedParserAtomIndex::WellKnown::default_();
    auto exportName = TaggedParserAtomIndex::WellKnown::default_();
    return appendExportEntry(exportName, localName);
  }

  switch (kid->getKind()) {
    case ParseNodeKind::ExportSpecList: {
      MOZ_ASSERT(!isDefault);
      for (ParseNode* item : kid->as<ListNode>().contents()) {
        BinaryNode* spec = &item->as<BinaryNode>();
        MOZ_ASSERT(spec->isKind(ParseNodeKind::ExportSpec));

        NameNode* localNameNode = &spec->left()->as<NameNode>();
        NameNode* exportNameNode = &spec->right()->as<NameNode>();

        auto localName = localNameNode->atom();
        auto exportName = exportNameNode->atom();

        if (!appendExportEntry(exportName, localName, spec)) {
          return false;
        }
      }
      break;
    }

    case ParseNodeKind::ClassDecl: {
      const ClassNode& cls = kid->as<ClassNode>();
      MOZ_ASSERT(cls.names());
      auto localName = cls.names()->innerBinding()->atom();
      auto exportName =
          isDefault ? TaggedParserAtomIndex::WellKnown::default_() : localName;
      if (!appendExportEntry(exportName, localName)) {
        return false;
      }
      break;
    }

    case ParseNodeKind::VarStmt:
    case ParseNodeKind::ConstDecl:
    case ParseNodeKind::LetDecl: {
      for (ParseNode* binding : kid->as<ListNode>().contents()) {
        if (binding->isKind(ParseNodeKind::AssignExpr)) {
          binding = binding->as<AssignmentNode>().left();
        } else {
          MOZ_ASSERT(binding->isKind(ParseNodeKind::Name));
        }

        if (binding->isKind(ParseNodeKind::Name)) {
          auto localName = binding->as<NameNode>().atom();
          auto exportName = isDefault
                                ? TaggedParserAtomIndex::WellKnown::default_()
                                : localName;
          if (!appendExportEntry(exportName, localName)) {
            return false;
          }
        } else if (binding->isKind(ParseNodeKind::ArrayExpr)) {
          if (!processExportArrayBinding(&binding->as<ListNode>())) {
            return false;
          }
        } else {
          MOZ_ASSERT(binding->isKind(ParseNodeKind::ObjectExpr));
          if (!processExportObjectBinding(&binding->as<ListNode>())) {
            return false;
          }
        }
      }
      break;
    }

    case ParseNodeKind::Function: {
      FunctionBox* box = kid->as<FunctionNode>().funbox();
      MOZ_ASSERT(!box->isArrow());
      auto localName = box->explicitName();
      auto exportName =
          isDefault ? TaggedParserAtomIndex::WellKnown::default_() : localName;
      if (!appendExportEntry(exportName, localName)) {
        return false;
      }
      break;
    }

    default:
      MOZ_CRASH("Unexpected parse node");
  }

  return true;
}

bool ModuleBuilder::processExportBinding(frontend::ParseNode* binding) {
  using namespace js::frontend;

  if (binding->isKind(ParseNodeKind::Name)) {
    auto name = binding->as<NameNode>().atom();
    return appendExportEntry(name, name);
  }

  if (binding->isKind(ParseNodeKind::ArrayExpr)) {
    return processExportArrayBinding(&binding->as<ListNode>());
  }

  MOZ_ASSERT(binding->isKind(ParseNodeKind::ObjectExpr));
  return processExportObjectBinding(&binding->as<ListNode>());
}

bool ModuleBuilder::processExportArrayBinding(frontend::ListNode* array) {
  using namespace js::frontend;

  MOZ_ASSERT(array->isKind(ParseNodeKind::ArrayExpr));

  for (ParseNode* node : array->contents()) {
    if (node->isKind(ParseNodeKind::Elision)) {
      continue;
    }

    if (node->isKind(ParseNodeKind::Spread)) {
      node = node->as<UnaryNode>().kid();
    } else if (node->isKind(ParseNodeKind::AssignExpr)) {
      node = node->as<AssignmentNode>().left();
    }

    if (!processExportBinding(node)) {
      return false;
    }
  }

  return true;
}

bool ModuleBuilder::processExportObjectBinding(frontend::ListNode* obj) {
  using namespace js::frontend;

  MOZ_ASSERT(obj->isKind(ParseNodeKind::ObjectExpr));

  for (ParseNode* node : obj->contents()) {
    MOZ_ASSERT(node->isKind(ParseNodeKind::MutateProto) ||
               node->isKind(ParseNodeKind::PropertyDefinition) ||
               node->isKind(ParseNodeKind::Shorthand) ||
               node->isKind(ParseNodeKind::Spread));

    ParseNode* target;
    if (node->isKind(ParseNodeKind::Spread)) {
      target = node->as<UnaryNode>().kid();
    } else {
      if (node->isKind(ParseNodeKind::MutateProto)) {
        target = node->as<UnaryNode>().kid();
      } else {
        target = node->as<BinaryNode>().right();
      }

      if (target->isKind(ParseNodeKind::AssignExpr)) {
        target = target->as<AssignmentNode>().left();
      }
    }

    if (!processExportBinding(target)) {
      return false;
    }
  }

  return true;
}

bool ModuleBuilder::processExportFrom(frontend::BinaryNode* exportNode) {
  using namespace js::frontend;

  MOZ_ASSERT(exportNode->isKind(ParseNodeKind::ExportFromStmt));

  auto* specList = &exportNode->left()->as<ListNode>();
  MOZ_ASSERT(specList->isKind(ParseNodeKind::ExportSpecList));

  auto* moduleRequest = &exportNode->right()->as<BinaryNode>();
  MOZ_ASSERT(moduleRequest->isKind(ParseNodeKind::ImportModuleRequest));

  auto* moduleSpec = &moduleRequest->left()->as<NameNode>();
  MOZ_ASSERT(moduleSpec->isKind(ParseNodeKind::StringExpr));

  auto* assertionList = &moduleRequest->right()->as<ListNode>();
  MOZ_ASSERT(assertionList->isKind(ParseNodeKind::ImportAssertionList));

  auto specifier = moduleSpec->atom();
  MaybeModuleRequestIndex moduleRequestIndex =
      appendModuleRequest(specifier, assertionList);
  if (!moduleRequestIndex.isSome()) {
    return false;
  }

  if (!maybeAppendRequestedModule(moduleRequestIndex, moduleSpec)) {
    return false;
  }

  for (ParseNode* spec : specList->contents()) {
    uint32_t line;
    uint32_t column;
    eitherParser_.computeLineAndColumn(spec->pn_pos.begin, &line, &column);

    StencilModuleEntry entry;
    TaggedParserAtomIndex exportName;
    if (spec->isKind(ParseNodeKind::ExportSpec)) {
      auto* importNameNode = &spec->as<BinaryNode>().left()->as<NameNode>();
      auto* exportNameNode = &spec->as<BinaryNode>().right()->as<NameNode>();

      auto importName = importNameNode->atom();
      exportName = exportNameNode->atom();

      markUsedByStencil(importName);
      markUsedByStencil(exportName);
      entry = StencilModuleEntry::exportFromEntry(
          moduleRequestIndex, importName, exportName, line, column);
    } else if (spec->isKind(ParseNodeKind::ExportNamespaceSpec)) {
      auto* exportNameNode = &spec->as<UnaryNode>().kid()->as<NameNode>();

      exportName = exportNameNode->atom();

      markUsedByStencil(exportName);
      entry = StencilModuleEntry::exportNamespaceFromEntry(
          moduleRequestIndex, exportName, line, column);
    } else {
      MOZ_ASSERT(spec->isKind(ParseNodeKind::ExportBatchSpecStmt));

      entry = StencilModuleEntry::exportBatchFromEntry(moduleRequestIndex, line,
                                                       column);
    }

    if (!exportEntries_.append(entry)) {
      return false;
    }
    if (exportName && !exportNames_.put(exportName)) {
      return false;
    }
  }

  return true;
}

frontend::StencilModuleEntry* ModuleBuilder::importEntryFor(
    frontend::TaggedParserAtomIndex localName) const {
  MOZ_ASSERT(localName);
  auto ptr = importEntries_.lookup(localName);
  if (!ptr) {
    return nullptr;
  }

  return &ptr->value();
}

bool ModuleBuilder::hasExportedName(
    frontend::TaggedParserAtomIndex name) const {
  MOZ_ASSERT(name);
  return exportNames_.has(name);
}

bool ModuleBuilder::appendExportEntry(
    frontend::TaggedParserAtomIndex exportName,
    frontend::TaggedParserAtomIndex localName, frontend::ParseNode* node) {
  uint32_t line = 0;
  uint32_t column = 0;
  if (node) {
    eitherParser_.computeLineAndColumn(node->pn_pos.begin, &line, &column);
  }

  markUsedByStencil(localName);
  markUsedByStencil(exportName);
  auto entry = frontend::StencilModuleEntry::exportAsEntry(
      localName, exportName, line, column);
  if (!exportEntries_.append(entry)) {
    return false;
  }

  if (!exportNames_.put(exportName)) {
    return false;
  }

  return true;
}

frontend::MaybeModuleRequestIndex ModuleBuilder::appendModuleRequest(
    frontend::TaggedParserAtomIndex specifier,
    frontend::ListNode* assertionList) {
  markUsedByStencil(specifier);
  auto request = frontend::StencilModuleRequest(specifier);

  if (!processAssertions(request, assertionList)) {
    return MaybeModuleRequestIndex();
  }

  uint32_t index = moduleRequests_.length();
  if (!moduleRequests_.append(request)) {
    js::ReportOutOfMemory(fc_);
    return MaybeModuleRequestIndex();
  }

  return MaybeModuleRequestIndex(index);
}

bool ModuleBuilder::maybeAppendRequestedModule(
    MaybeModuleRequestIndex moduleRequest, frontend::ParseNode* node) {
  auto specifier = moduleRequests_[moduleRequest.value()].specifier;
  if (requestedModuleSpecifiers_.has(specifier)) {
    return true;
  }

  uint32_t line;
  uint32_t column;
  eitherParser_.computeLineAndColumn(node->pn_pos.begin, &line, &column);

  auto entry = frontend::StencilModuleEntry::requestedModule(moduleRequest,
                                                             line, column);

  if (!requestedModules_.append(entry)) {
    js::ReportOutOfMemory(fc_);
    return false;
  }

  return requestedModuleSpecifiers_.put(specifier);
}

void ModuleBuilder::markUsedByStencil(frontend::TaggedParserAtomIndex name) {
  // Imported/exported identifiers must be atomized.
  eitherParser_.parserAtoms().markUsedByStencil(
      name, frontend::ParserAtom::Atomize::Yes);
}

JSObject* js::GetOrCreateModuleMetaObject(JSContext* cx,
                                          HandleObject moduleArg) {
  Handle<ModuleObject*> module = moduleArg.as<ModuleObject>();
  if (JSObject* obj = module->metaObject()) {
    return obj;
  }

  RootedObject metaObject(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!metaObject) {
    return nullptr;
  }

  JS::ModuleMetadataHook func = cx->runtime()->moduleMetadataHook;
  if (!func) {
    JS_ReportErrorASCII(cx, "Module metadata hook not set");
    return nullptr;
  }

  RootedValue modulePrivate(cx, JS::GetModulePrivate(module));
  if (!func(cx, modulePrivate, metaObject)) {
    return nullptr;
  }

  module->setMetaObject(metaObject);

  return metaObject;
}

ModuleObject* js::CallModuleResolveHook(JSContext* cx,
                                        HandleValue referencingPrivate,
                                        HandleObject moduleRequest) {
  JS::ModuleResolveHook moduleResolveHook = cx->runtime()->moduleResolveHook;
  if (!moduleResolveHook) {
    JS_ReportErrorASCII(cx, "Module resolve hook not set");
    return nullptr;
  }

  RootedObject result(cx,
                      moduleResolveHook(cx, referencingPrivate, moduleRequest));
  if (!result) {
    return nullptr;
  }

  if (!result->is<ModuleObject>()) {
    JS_ReportErrorASCII(cx, "Module resolve hook did not return Module object");
    return nullptr;
  }

  return &result->as<ModuleObject>();
}

bool ModuleObject::topLevelCapabilityResolve(JSContext* cx,
                                             Handle<ModuleObject*> module) {
  RootedValue rval(cx);
  Rooted<PromiseObject*> promise(
      cx, &module->topLevelCapability()->as<PromiseObject>());
  return AsyncFunctionReturned(cx, promise, rval);
}

bool ModuleObject::topLevelCapabilityReject(JSContext* cx,
                                            Handle<ModuleObject*> module,
                                            HandleValue error) {
  Rooted<PromiseObject*> promise(
      cx, &module->topLevelCapability()->as<PromiseObject>());
  return AsyncFunctionThrown(cx, promise, error);
}

// https://tc39.es/proposal-import-assertions/#sec-evaluate-import-call
// NOTE: The caller needs to handle the promise.
static bool EvaluateDynamicImportOptions(
    JSContext* cx, HandleValue optionsArg,
    MutableHandle<ArrayObject*> assertionArrayArg) {
  // Step 10. If options is not undefined, then.
  if (optionsArg.isUndefined()) {
    return true;
  }

  // Step 10.a. If Type(options) is not Object,
  if (!optionsArg.isObject()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE, "import",
        "object or undefined", InformalValueTypeName(optionsArg));
    return false;
  }

  RootedObject assertWrapperObject(cx, &optionsArg.toObject());
  RootedValue assertValue(cx);

  // Step 10.b. Let assertionsObj be Get(options, "assert").
  RootedId assertId(cx, NameToId(cx->names().assert_));
  if (!GetProperty(cx, assertWrapperObject, assertWrapperObject, assertId,
                   &assertValue)) {
    return false;
  }

  // Step 10.d. If assertionsObj is not undefined.
  if (assertValue.isUndefined()) {
    return true;
  }

  // Step 10.d.i. If Type(assertionsObj) is not Object.
  if (!assertValue.isObject()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE, "import",
        "object or undefined", InformalValueTypeName(assertValue));
    return false;
  }

  // Step 10.d.i. Let keys be EnumerableOwnPropertyNames(assertionsObj, key).
  RootedObject assertObject(cx, &assertValue.toObject());
  RootedIdVector assertions(cx);
  if (!GetPropertyKeys(cx, assertObject, JSITER_OWNONLY, &assertions)) {
    return false;
  }

  uint32_t numberOfAssertions = assertions.length();
  if (numberOfAssertions == 0) {
    return true;
  }

  // Step 9 (reordered). Let assertions be a new empty List.
  Rooted<ArrayObject*> assertionArray(
      cx, NewDenseFullyAllocatedArray(cx, numberOfAssertions));
  if (!assertionArray) {
    return false;
  }
  assertionArray->ensureDenseInitializedLength(0, numberOfAssertions);

  // Step 10.d.iv. Let supportedAssertions be
  // !HostGetSupportedImportAssertions().
  const JS::ImportAssertionVector& supportedAssertions =
      cx->runtime()->supportedImportAssertions;

  size_t numberOfValidAssertions = 0;

  // Step 10.d.v. For each String key of keys,
  RootedId key(cx);
  for (size_t i = 0; i < numberOfAssertions; i++) {
    key = assertions[i];

    // Step 10.d.v.1. Let value be Get(assertionsObj, key).
    RootedValue value(cx);
    if (!GetProperty(cx, assertObject, assertObject, key, &value)) {
      return false;
    }

    // Step 10.d.v.3. If Type(value) is not String, then.
    if (!value.isString()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_NOT_EXPECTED_TYPE, "import", "string",
                                InformalValueTypeName(value));
      return false;
    }

    // Step 10.d.v.4. If supportedAssertions contains key, then Append {
    // [[Key]]: key, [[Value]]: value } to assertions.
    for (JS::ImportAssertion assertion : supportedAssertions) {
      bool supported = false;
      switch (assertion) {
        case JS::ImportAssertion::Type: {
          supported = key.toAtom() == cx->names().type;
        } break;
      }

      if (supported) {
        Rooted<PlainObject*> assertionObj(cx, NewPlainObject(cx));
        if (!assertionObj) {
          return false;
        }

        if (!DefineDataProperty(cx, assertionObj, key, value,
                                JSPROP_ENUMERATE)) {
          return false;
        }

        assertionArray->initDenseElement(numberOfValidAssertions,
                                         ObjectValue(*assertionObj));
        ++numberOfValidAssertions;
      }
    }
  }

  if (numberOfValidAssertions == 0) {
    return true;
  }

  assertionArray->setLength(numberOfValidAssertions);
  assertionArrayArg.set(assertionArray);

  return true;
}

JSObject* js::StartDynamicModuleImport(JSContext* cx, HandleScript script,
                                       HandleValue specifierArg,
                                       HandleValue optionsArg) {
  RootedObject promiseConstructor(cx, JS::GetPromiseConstructor(cx));
  if (!promiseConstructor) {
    return nullptr;
  }

  RootedObject promiseObject(cx, JS::NewPromiseObject(cx, nullptr));
  if (!promiseObject) {
    return nullptr;
  }

  Handle<PromiseObject*> promise = promiseObject.as<PromiseObject>();

  JS::ModuleDynamicImportHook importHook =
      cx->runtime()->moduleDynamicImportHook;

  if (!importHook) {
    // Dynamic import can be disabled by a pref and is not supported in all
    // contexts (e.g. web workers).
    JS_ReportErrorASCII(
        cx,
        "Dynamic module import is disabled or not supported in this context");
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  RootedString specifier(cx, ToString(cx, specifierArg));
  if (!specifier) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  RootedValue referencingPrivate(cx, script->sourceObject()->getPrivate());
  cx->runtime()->addRefScriptPrivate(referencingPrivate);

  Rooted<JSAtom*> specifierAtom(cx, AtomizeString(cx, specifier));
  if (!specifierAtom) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  Rooted<ArrayObject*> assertionArray(cx);
  if (!EvaluateDynamicImportOptions(cx, optionsArg, &assertionArray)) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  RootedObject moduleRequest(
      cx, ModuleRequestObject::create(cx, specifierAtom, assertionArray));
  if (!moduleRequest) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  if (!importHook(cx, referencingPrivate, moduleRequest, promise)) {
    cx->runtime()->releaseScriptPrivate(referencingPrivate);

    // If there's no exception pending then the script is terminating
    // anyway, so just return nullptr.
    if (!cx->isExceptionPending() ||
        !RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  return promise;
}

static bool OnRootModuleRejected(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue error = args.get(0);

  ReportExceptionClosure reportExn(error);
  PrepareScriptEnvironmentAndInvoke(cx, cx->global(), reportExn);

  args.rval().setUndefined();
  return true;
};

bool js::OnModuleEvaluationFailure(JSContext* cx,
                                   HandleObject evaluationPromise,
                                   JS::ModuleErrorBehaviour errorBehaviour) {
  if (evaluationPromise == nullptr) {
    return false;
  }

  // To allow module evaluation to happen synchronously throw the error
  // immediately. This assumes that any error will already have caused the
  // promise to be rejected, and doesn't support top-level await.
  if (errorBehaviour == JS::ThrowModuleErrorsSync) {
    JS::PromiseState state = JS::GetPromiseState(evaluationPromise);
    MOZ_DIAGNOSTIC_ASSERT(state == JS::PromiseState::Rejected ||
                          state == JS::PromiseState::Fulfilled);

    JS::SetSettledPromiseIsHandled(cx, evaluationPromise);
    if (state == JS::PromiseState::Fulfilled) {
      return true;
    }

    RootedValue error(cx, JS::GetPromiseResult(evaluationPromise));
    JS_SetPendingException(cx, error);
    return false;
  }

  RootedFunction onRejected(
      cx, NewHandler(cx, OnRootModuleRejected, evaluationPromise));
  if (!onRejected) {
    return false;
  }

  return JS::AddPromiseReactions(cx, evaluationPromise, nullptr, onRejected);
}

// Adjustment for Top-level await;
// See: https://github.com/tc39/proposal-dynamic-import/pull/71/files
static bool OnResolvedDynamicModule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.get(0).isUndefined());

  // This is a hack to allow us to have the 2 extra variables needed
  // for FinishDynamicModuleImport in the resolve callback.
  Rooted<ListObject*> resolvedModuleParams(cx,
                                           ExtraFromHandler<ListObject>(args));
  MOZ_ASSERT(resolvedModuleParams->length() == 2);
  RootedValue referencingPrivate(cx, resolvedModuleParams->get(0));

  Rooted<JSAtom*> specifier(
      cx, AtomizeString(cx, resolvedModuleParams->get(1).toString()));
  if (!specifier) {
    return false;
  }

  Rooted<PromiseObject*> promise(cx, TargetFromHandler<PromiseObject>(args));

  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  RootedObject moduleRequest(
      cx, ModuleRequestObject::create(cx, specifier, nullptr));
  if (!moduleRequest) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedObject result(
      cx, CallModuleResolveHook(cx, referencingPrivate, moduleRequest));

  if (!result) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  Rooted<ModuleObject*> module(cx, &result->as<ModuleObject>());
  if (module->status() != ModuleStatus::EvaluatingAsync &&
      module->status() != ModuleStatus::Evaluated) {
    JS_ReportErrorASCII(
        cx, "Unevaluated or errored module returned by module resolve hook");
    return RejectPromiseWithPendingError(cx, promise);
  }

  MOZ_ASSERT(module->getCycleRoot()
                 ->topLevelCapability()
                 ->as<PromiseObject>()
                 .state() == JS::PromiseState::Fulfilled);

  RootedObject ns(cx, GetOrCreateModuleNamespace(cx, module));
  if (!ns) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  args.rval().setUndefined();
  RootedValue value(cx, ObjectValue(*ns));
  return PromiseObject::resolve(cx, promise, value);
};

static bool OnRejectedDynamicModule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  HandleValue error = args.get(0);

  RootedValue referencingPrivate(cx, ExtraValueFromHandler(args));
  Rooted<PromiseObject*> promise(cx, TargetFromHandler<PromiseObject>(args));

  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  args.rval().setUndefined();
  return PromiseObject::reject(cx, promise, error);
};

bool FinishDynamicModuleImport_impl(JSContext* cx,
                                    HandleObject evaluationPromise,
                                    HandleValue referencingPrivate,
                                    HandleObject moduleRequest,
                                    HandleObject promiseArg) {
  Rooted<ListObject*> resolutionArgs(cx, ListObject::create(cx));
  if (!resolutionArgs->append(cx, referencingPrivate)) {
    return false;
  }
  Rooted<Value> stringValue(
      cx, StringValue(moduleRequest->as<ModuleRequestObject>().specifier()));
  if (!resolutionArgs->append(cx, stringValue)) {
    return false;
  }

  Rooted<Value> resolutionArgsValue(cx, ObjectValue(*resolutionArgs));

  RootedFunction onResolved(
      cx, NewHandlerWithExtraValue(cx, OnResolvedDynamicModule, promiseArg,
                                   resolutionArgsValue));
  if (!onResolved) {
    return false;
  }

  RootedFunction onRejected(
      cx, NewHandlerWithExtraValue(cx, OnRejectedDynamicModule, promiseArg,
                                   referencingPrivate));
  if (!onRejected) {
    return false;
  }

  return JS::AddPromiseReactionsIgnoringUnhandledRejection(
      cx, evaluationPromise, onResolved, onRejected);
}

bool js::FinishDynamicModuleImport(JSContext* cx,
                                   HandleObject evaluationPromise,
                                   HandleValue referencingPrivate,
                                   HandleObject moduleRequest,
                                   HandleObject promiseArg) {
  // If we do not have an evaluation promise or a module request for the module,
  // we can assume that evaluation has failed or been interrupted -- we can
  // reject the dynamic module.
  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  if (!evaluationPromise || !moduleRequest) {
    Handle<PromiseObject*> promise = promiseArg.as<PromiseObject>();
    return RejectPromiseWithPendingError(cx, promise);
  }

  if (!FinishDynamicModuleImport_impl(cx, evaluationPromise, referencingPrivate,
                                      moduleRequest, promiseArg)) {
    return false;
  }

  releasePrivate.release();
  return true;
}
