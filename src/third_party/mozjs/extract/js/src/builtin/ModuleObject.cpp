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
#include "frontend/SharedContext.h"
#include "gc/FreeOp.h"
#include "gc/Policy.h"
#include "gc/Tracer.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Modules.h"  // JS::GetModulePrivate, JS::ModuleDynamicImportHook
#include "js/PropertySpec.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/ModuleBuilder.h"       // js::ModuleBuilder
#include "vm/PlainObject.h"         // js::PlainObject
#include "vm/PromiseObject.h"       // js::PromiseObject
#include "vm/SelfHosting.h"
#include "vm/SharedStencil.h"  // js::GCThingIndex

#include "builtin/HandlerFunction-inl.h"  // js::ExtraValueFromHandler, js::NewHandler{,WithExtraValue}, js::TargetFromHandler
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/List-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

static_assert(MODULE_STATUS_UNLINKED < MODULE_STATUS_LINKING &&
                  MODULE_STATUS_LINKING < MODULE_STATUS_LINKED &&
                  MODULE_STATUS_LINKED < MODULE_STATUS_EVALUATED &&
                  MODULE_STATUS_EVALUATED < MODULE_STATUS_EVALUATED_ERROR,
              "Module statuses are ordered incorrectly");

template <typename T, Value ValueGetter(const T* obj)>
static bool ModuleValueGetterImpl(JSContext* cx, const CallArgs& args) {
  args.rval().set(ValueGetter(&args.thisv().toObject().as<T>()));
  return true;
}

template <typename T, Value ValueGetter(const T* obj)>
static bool ModuleValueGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<T::isInstance,
                              ModuleValueGetterImpl<T, ValueGetter>>(cx, args);
}

#define DEFINE_GETTER_FUNCTIONS(cls, name, slot)                              \
  static Value cls##_##name##Value(const cls* obj) {                          \
    return obj->getReservedSlot(cls::slot);                                   \
  }                                                                           \
                                                                              \
  static bool cls##_##name##Getter(JSContext* cx, unsigned argc, Value* vp) { \
    return ModuleValueGetter<cls, cls##_##name##Value>(cx, argc, vp);         \
  }

#define DEFINE_ATOM_ACCESSOR_METHOD(cls, name) \
  JSAtom* cls::name() const {                  \
    Value value = cls##_##name##Value(this);   \
    return &value.toString()->asAtom();        \
  }

#define DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(cls, name) \
  JSAtom* cls::name() const {                          \
    Value value = cls##_##name##Value(this);           \
    if (value.isNull()) return nullptr;                \
    return &value.toString()->asAtom();                \
  }

#define DEFINE_UINT32_ACCESSOR_METHOD(cls, name) \
  uint32_t cls::name() const {                   \
    Value value = cls##_##name##Value(this);     \
    MOZ_ASSERT(value.toNumber() >= 0);           \
    if (value.isInt32()) return value.toInt32(); \
    return JS::ToUint32(value.toDouble());       \
  }

static Value StringOrNullValue(JSString* maybeString) {
  return maybeString ? StringValue(maybeString) : NullValue();
}

///////////////////////////////////////////////////////////////////////////
// ImportEntryObject

/* static */ const JSClass ImportEntryObject::class_ = {
    "ImportEntry", JSCLASS_HAS_RESERVED_SLOTS(ImportEntryObject::SlotCount)};

DEFINE_GETTER_FUNCTIONS(ImportEntryObject, moduleRequest, ModuleRequestSlot)
DEFINE_GETTER_FUNCTIONS(ImportEntryObject, importName, ImportNameSlot)
DEFINE_GETTER_FUNCTIONS(ImportEntryObject, localName, LocalNameSlot)
DEFINE_GETTER_FUNCTIONS(ImportEntryObject, lineNumber, LineNumberSlot)
DEFINE_GETTER_FUNCTIONS(ImportEntryObject, columnNumber, ColumnNumberSlot)

DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ImportEntryObject, importName)
DEFINE_ATOM_ACCESSOR_METHOD(ImportEntryObject, localName)
DEFINE_UINT32_ACCESSOR_METHOD(ImportEntryObject, lineNumber)
DEFINE_UINT32_ACCESSOR_METHOD(ImportEntryObject, columnNumber)

ModuleRequestObject* ImportEntryObject::moduleRequest() const {
  Value value = getReservedSlot(ModuleRequestSlot);
  return &value.toObject().as<ModuleRequestObject>();
}

/* static */
bool ImportEntryObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ImportEntryObject>();
}

/* static */
bool GlobalObject::initImportEntryProto(JSContext* cx,
                                        Handle<GlobalObject*> global) {
  static const JSPropertySpec protoAccessors[] = {
      JS_PSG("moduleRequest", ImportEntryObject_moduleRequestGetter, 0),
      JS_PSG("importName", ImportEntryObject_importNameGetter, 0),
      JS_PSG("localName", ImportEntryObject_localNameGetter, 0),
      JS_PSG("lineNumber", ImportEntryObject_lineNumberGetter, 0),
      JS_PSG("columnNumber", ImportEntryObject_columnNumberGetter, 0),
      JS_PS_END};

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!DefinePropertiesAndFunctions(cx, proto, protoAccessors, nullptr)) {
    return false;
  }

  global->initReservedSlot(IMPORT_ENTRY_PROTO, ObjectValue(*proto));
  return true;
}

/* static */
ImportEntryObject* ImportEntryObject::create(
    JSContext* cx, HandleObject moduleRequest, HandleAtom maybeImportName,
    HandleAtom localName, uint32_t lineNumber, uint32_t columnNumber) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateImportEntryPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  ImportEntryObject* self =
      NewObjectWithGivenProto<ImportEntryObject>(cx, proto);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(ModuleRequestSlot, ObjectValue(*moduleRequest));
  self->initReservedSlot(ImportNameSlot, StringOrNullValue(maybeImportName));
  self->initReservedSlot(LocalNameSlot, StringValue(localName));
  self->initReservedSlot(LineNumberSlot, NumberValue(lineNumber));
  self->initReservedSlot(ColumnNumberSlot, NumberValue(columnNumber));
  return self;
}

///////////////////////////////////////////////////////////////////////////
// ExportEntryObject

/* static */ const JSClass ExportEntryObject::class_ = {
    "ExportEntry", JSCLASS_HAS_RESERVED_SLOTS(ExportEntryObject::SlotCount)};

DEFINE_GETTER_FUNCTIONS(ExportEntryObject, exportName, ExportNameSlot)
DEFINE_GETTER_FUNCTIONS(ExportEntryObject, moduleRequest, ModuleRequestSlot)
DEFINE_GETTER_FUNCTIONS(ExportEntryObject, importName, ImportNameSlot)
DEFINE_GETTER_FUNCTIONS(ExportEntryObject, localName, LocalNameSlot)
DEFINE_GETTER_FUNCTIONS(ExportEntryObject, lineNumber, LineNumberSlot)
DEFINE_GETTER_FUNCTIONS(ExportEntryObject, columnNumber, ColumnNumberSlot)

DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ExportEntryObject, exportName)
DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ExportEntryObject, importName)
DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ExportEntryObject, localName)
DEFINE_UINT32_ACCESSOR_METHOD(ExportEntryObject, lineNumber)
DEFINE_UINT32_ACCESSOR_METHOD(ExportEntryObject, columnNumber)

ModuleRequestObject* ExportEntryObject::moduleRequest() const {
  Value value = getReservedSlot(ModuleRequestSlot);
  return &value.toObject().as<ModuleRequestObject>();
}

/* static */
bool ExportEntryObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ExportEntryObject>();
}

/* static */
bool GlobalObject::initExportEntryProto(JSContext* cx,
                                        Handle<GlobalObject*> global) {
  static const JSPropertySpec protoAccessors[] = {
      JS_PSG("exportName", ExportEntryObject_exportNameGetter, 0),
      JS_PSG("moduleRequest", ExportEntryObject_moduleRequestGetter, 0),
      JS_PSG("importName", ExportEntryObject_importNameGetter, 0),
      JS_PSG("localName", ExportEntryObject_localNameGetter, 0),
      JS_PSG("lineNumber", ExportEntryObject_lineNumberGetter, 0),
      JS_PSG("columnNumber", ExportEntryObject_columnNumberGetter, 0),
      JS_PS_END};

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!DefinePropertiesAndFunctions(cx, proto, protoAccessors, nullptr)) {
    return false;
  }

  global->initReservedSlot(EXPORT_ENTRY_PROTO, ObjectValue(*proto));
  return true;
}

/* static */
ExportEntryObject* ExportEntryObject::create(
    JSContext* cx, HandleAtom maybeExportName, HandleObject moduleRequest,
    HandleAtom maybeImportName, HandleAtom maybeLocalName, uint32_t lineNumber,
    uint32_t columnNumber) {
  // Line and column numbers are optional for export entries since direct
  // entries are checked at parse time.

  RootedObject proto(
      cx, GlobalObject::getOrCreateExportEntryPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  ExportEntryObject* self =
      NewObjectWithGivenProto<ExportEntryObject>(cx, proto);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(ExportNameSlot, StringOrNullValue(maybeExportName));
  self->initReservedSlot(ModuleRequestSlot, ObjectValue(*moduleRequest));
  self->initReservedSlot(ImportNameSlot, StringOrNullValue(maybeImportName));
  self->initReservedSlot(LocalNameSlot, StringOrNullValue(maybeLocalName));
  self->initReservedSlot(LineNumberSlot, NumberValue(lineNumber));
  self->initReservedSlot(ColumnNumberSlot, NumberValue(columnNumber));
  return self;
}

///////////////////////////////////////////////////////////////////////////
// RequestedModuleObject

/* static */ const JSClass RequestedModuleObject::class_ = {
    "RequestedModule",
    JSCLASS_HAS_RESERVED_SLOTS(RequestedModuleObject::SlotCount)};

DEFINE_GETTER_FUNCTIONS(RequestedModuleObject, moduleRequest, ModuleRequestSlot)
DEFINE_GETTER_FUNCTIONS(RequestedModuleObject, lineNumber, LineNumberSlot)
DEFINE_GETTER_FUNCTIONS(RequestedModuleObject, columnNumber, ColumnNumberSlot)

DEFINE_UINT32_ACCESSOR_METHOD(RequestedModuleObject, lineNumber)
DEFINE_UINT32_ACCESSOR_METHOD(RequestedModuleObject, columnNumber)

ModuleRequestObject* RequestedModuleObject::moduleRequest() const {
  Value value = getReservedSlot(ModuleRequestSlot);
  return &value.toObject().as<ModuleRequestObject>();
}

/* static */
bool RequestedModuleObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<RequestedModuleObject>();
}

/* static */
bool GlobalObject::initRequestedModuleProto(JSContext* cx,
                                            Handle<GlobalObject*> global) {
  static const JSPropertySpec protoAccessors[] = {
      JS_PSG("moduleRequest", RequestedModuleObject_moduleRequestGetter, 0),
      JS_PSG("lineNumber", RequestedModuleObject_lineNumberGetter, 0),
      JS_PSG("columnNumber", RequestedModuleObject_columnNumberGetter, 0),
      JS_PS_END};

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!DefinePropertiesAndFunctions(cx, proto, protoAccessors, nullptr)) {
    return false;
  }

  global->initReservedSlot(REQUESTED_MODULE_PROTO, ObjectValue(*proto));
  return true;
}

/* static */
RequestedModuleObject* RequestedModuleObject::create(JSContext* cx,
                                                     HandleObject moduleRequest,
                                                     uint32_t lineNumber,
                                                     uint32_t columnNumber) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateRequestedModulePrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  RequestedModuleObject* self =
      NewObjectWithGivenProto<RequestedModuleObject>(cx, proto);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(ModuleRequestSlot, ObjectValue(*moduleRequest));
  self->initReservedSlot(LineNumberSlot, NumberValue(lineNumber));
  self->initReservedSlot(ColumnNumberSlot, NumberValue(columnNumber));
  return self;
}

///////////////////////////////////////////////////////////////////////////
// ModuleRequestObject
/* static */ const JSClass ModuleRequestObject::class_ = {
    "ModuleRequest",
    JSCLASS_HAS_RESERVED_SLOTS(ModuleRequestObject::SlotCount)};

DEFINE_GETTER_FUNCTIONS(ModuleRequestObject, specifier, SpecifierSlot)

DEFINE_ATOM_OR_NULL_ACCESSOR_METHOD(ModuleRequestObject, specifier)

/* static */
bool ModuleRequestObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ModuleRequestObject>();
}

/* static */
bool GlobalObject::initModuleRequestProto(JSContext* cx,
                                          Handle<GlobalObject*> global) {
  static const JSPropertySpec protoAccessors[] = {
      JS_PSG("specifier", ModuleRequestObject_specifierGetter, 0), JS_PS_END};

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!DefinePropertiesAndFunctions(cx, proto, protoAccessors, nullptr)) {
    return false;
  }

  global->initReservedSlot(MODULE_REQUEST_PROTO, ObjectValue(*proto));
  return true;
}

/* static */
ModuleRequestObject* ModuleRequestObject::create(JSContext* cx,
                                                 HandleAtom specifier) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateModuleRequestPrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  ModuleRequestObject* self =
      NewObjectWithGivenProto<ModuleRequestObject>(cx, proto);
  if (!self) {
    return nullptr;
  }

  self->initReservedSlot(SpecifierSlot, StringOrNullValue(specifier));
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
                             HandleModuleEnvironmentObject environment,
                             HandleId targetName) {
  // This object might have been allocated on the background parsing thread in
  // different zone to the final module. Lazily allocate the map so we don't
  // have to switch its zone when merging realms.
  if (!map_) {
    MOZ_ASSERT(!cx->zone()->createdForHelperThread());
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
  *propOut = mozilla::Some(binding.prop);
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
    JSContext* cx, HandleModuleObject module, HandleArrayObject exports,
    UniquePtr<IndirectBindingMap> bindings) {
  RootedValue priv(cx, ObjectValue(*module));
  ProxyOptions options;
  options.setLazyProto(true);
  Rooted<UniquePtr<IndirectBindingMap>> rootedBindings(cx, std::move(bindings));
  RootedObject object(
      cx, NewProxyObject(cx, &proxyHandler, priv, nullptr, options));
  if (!object) {
    return nullptr;
  }

  SetProxyReservedSlot(object, ExportsSlot, ObjectValue(*exports));
  SetProxyReservedSlot(object, BindingsSlot,
                       PrivateValue(rootedBindings.release()));
  AddCellMemory(object, sizeof(IndirectBindingMap),
                MemoryUse::ModuleBindingMap);

  return &object->as<ModuleNamespaceObject>();
}

ModuleObject& ModuleNamespaceObject::module() {
  return GetProxyPrivate(this).toObject().as<ModuleObject>();
}

ArrayObject& ModuleNamespaceObject::exports() {
  return GetProxyReservedSlot(this, ExportsSlot).toObject().as<ArrayObject>();
}

IndirectBindingMap& ModuleNamespaceObject::bindings() {
  Value value = GetProxyReservedSlot(this, BindingsSlot);
  auto bindings = static_cast<IndirectBindingMap*>(value.toPrivate());
  MOZ_ASSERT(bindings);
  return *bindings;
}

bool ModuleNamespaceObject::hasBindings() const {
  // Import bindings may not be present if we hit OOM in initialization.
  return !GetProxyReservedSlot(this, BindingsSlot).isUndefined();
}

bool ModuleNamespaceObject::addBinding(JSContext* cx, HandleAtom exportedName,
                                       HandleModuleObject targetModule,
                                       HandleAtom targetName) {
  RootedModuleEnvironmentObject environment(
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
      desc.set(mozilla::Some(
          PropertyDescriptor::Data(StringValue(cx->names().Module))));
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

  desc.set(mozilla::Some(PropertyDescriptor::Data(
      value,
      {JS::PropertyAttribute::Enumerable, JS::PropertyAttribute::Writable})));
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
  Rooted<ArrayObject*> exports(cx, &ns->exports());
  uint32_t count = exports->length();
  if (!props.reserve(props.length() + count + 1)) {
    return false;
  }

  Rooted<ValueVector> names(cx, ValueVector(cx));
  if (!names.resize(count) || !GetElements(cx, exports, count, names.begin())) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    props.infallibleAppend(AtomToId(&names[i].toString()->asAtom()));
  }

  props.infallibleAppend(SYMBOL_TO_JSID(cx->wellKnownSymbols().toStringTag));

  return true;
}

void ModuleNamespaceObject::ProxyHandler::trace(JSTracer* trc,
                                                JSObject* proxy) const {
  auto& self = proxy->as<ModuleNamespaceObject>();

  if (self.hasBindings()) {
    self.bindings().trace(trc);
  }
}

void ModuleNamespaceObject::ProxyHandler::finalize(JSFreeOp* fop,
                                                   JSObject* proxy) const {
  auto& self = proxy->as<ModuleNamespaceObject>();

  if (self.hasBindings()) {
    fop->delete_(proxy, &self.bindings(), MemoryUse::ModuleBindingMap);
  }
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
    nullptr,                 // hasInstance
    nullptr,                 // construct
    ModuleObject::trace,     // trace
};

/* static */ const JSClass ModuleObject::class_ = {
    "Module",
    JSCLASS_HAS_RESERVED_SLOTS(ModuleObject::SlotCount) |
        JSCLASS_BACKGROUND_FINALIZE,
    &ModuleObject::classOps_};

#define DEFINE_ARRAY_SLOT_ACCESSOR(cls, name, slot)                 \
  ArrayObject& cls::name() const {                                  \
    return getReservedSlot(cls::slot).toObject().as<ArrayObject>(); \
  }

DEFINE_ARRAY_SLOT_ACCESSOR(ModuleObject, requestedModules, RequestedModulesSlot)
DEFINE_ARRAY_SLOT_ACCESSOR(ModuleObject, importEntries, ImportEntriesSlot)
DEFINE_ARRAY_SLOT_ACCESSOR(ModuleObject, localExportEntries,
                           LocalExportEntriesSlot)
DEFINE_ARRAY_SLOT_ACCESSOR(ModuleObject, indirectExportEntries,
                           IndirectExportEntriesSlot)
DEFINE_ARRAY_SLOT_ACCESSOR(ModuleObject, starExportEntries,
                           StarExportEntriesSlot)

/* static */
bool ModuleObject::isInstance(HandleValue value) {
  return value.isObject() && value.toObject().is<ModuleObject>();
}

// Declared as static function instead of ModuleObject method in order to
// avoid recursive #include dependency between frontend and VM.
static frontend::FunctionDeclarationVector* GetFunctionDeclarations(
    ModuleObject* module) {
  Value value = module->getReservedSlot(ModuleObject::FunctionDeclarationsSlot);
  if (value.isUndefined()) {
    return nullptr;
  }

  return static_cast<frontend::FunctionDeclarationVector*>(value.toPrivate());
}

static void InitFunctionDeclarations(
    ModuleObject* module, frontend::FunctionDeclarationVector&& decls) {
  *GetFunctionDeclarations(module) = std::move(decls);
}

/* static */
ModuleObject* ModuleObject::create(JSContext* cx) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateModulePrototype(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  RootedModuleObject self(cx, NewObjectWithGivenProto<ModuleObject>(cx, proto));
  if (!self) {
    return nullptr;
  }

  IndirectBindingMap* bindings = cx->new_<IndirectBindingMap>();
  if (!bindings) {
    return nullptr;
  }

  InitReservedSlot(self, ImportBindingsSlot, bindings,
                   MemoryUse::ModuleBindingMap);

  frontend::FunctionDeclarationVector* funDecls =
      cx->new_<frontend::FunctionDeclarationVector>();
  if (!funDecls) {
    return nullptr;
  }

  self->initReservedSlot(FunctionDeclarationsSlot, PrivateValue(funDecls));
  return self;
}

/* static */
void ModuleObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->maybeOnHelperThread());
  ModuleObject* self = &obj->as<ModuleObject>();
  if (self->hasImportBindings()) {
    fop->delete_(obj, &self->importBindings(), MemoryUse::ModuleBindingMap);
  }
  if (frontend::FunctionDeclarationVector* funDecls =
          GetFunctionDeclarations(self)) {
    // Not tracked as these may move between zones on merge.
    fop->deleteUntracked(funDecls);
  }
}

ModuleEnvironmentObject& ModuleObject::initialEnvironment() const {
  Value value = getReservedSlot(EnvironmentSlot);
  return value.toObject().as<ModuleEnvironmentObject>();
}

ModuleEnvironmentObject* ModuleObject::environment() const {
  // Note that this it's valid to call this even if there was an error
  // evaluating the module.

  // According to the spec the environment record is created during
  // instantiation, but we create it earlier than that.
  if (status() < MODULE_STATUS_LINKED) {
    return nullptr;
  }

  return &initialEnvironment();
}

bool ModuleObject::hasImportBindings() const {
  // Import bindings may not be present if we hit OOM in initialization.
  return !getReservedSlot(ImportBindingsSlot).isUndefined();
}

IndirectBindingMap& ModuleObject::importBindings() {
  return *static_cast<IndirectBindingMap*>(
      getReservedSlot(ImportBindingsSlot).toPrivate());
}

ModuleNamespaceObject* ModuleObject::namespace_() {
  Value value = getReservedSlot(NamespaceSlot);
  if (value.isUndefined()) {
    return nullptr;
  }
  return &value.toObject().as<ModuleNamespaceObject>();
}

ScriptSourceObject* ModuleObject::scriptSourceObject() const {
  return &getReservedSlot(ScriptSourceObjectSlot)
              .toObject()
              .as<ScriptSourceObject>();
}

bool ModuleObject::initAsyncSlots(JSContext* cx, bool isAsync,
                                  HandleObject asyncParentModulesList) {
  initReservedSlot(AsyncSlot, BooleanValue(isAsync));
  initReservedSlot(AsyncParentModulesSlot,
                   ObjectValue(*asyncParentModulesList));
  return true;
}

constexpr uint32_t ASYNC_EVALUATING_POST_ORDER_FALSE = 0;
constexpr uint32_t ASYNC_EVALUATING_POST_ORDER_INIT = 1;
uint32_t AsyncPostOrder = ASYNC_EVALUATING_POST_ORDER_INIT;

uint32_t nextPostOrder() {
  uint32_t ordinal = AsyncPostOrder;
  MOZ_ASSERT(AsyncPostOrder < MAX_UINT32);
  AsyncPostOrder++;
  return ordinal;
}

bool ModuleObject::initAsyncEvaluatingSlot() {
  initReservedSlot(AsyncEvaluatingPostOrderSlot,
                   PrivateUint32Value(nextPostOrder()));
  return true;
}

void ModuleObject::initScriptSlots(HandleScript script) {
  MOZ_ASSERT(script);
  initReservedSlot(ScriptSlot, PrivateGCThingValue(script));
  initReservedSlot(ScriptSourceObjectSlot,
                   ObjectValue(*script->sourceObject()));
}

void ModuleObject::setInitialEnvironment(
    HandleModuleEnvironmentObject initialEnvironment) {
  initReservedSlot(EnvironmentSlot, ObjectValue(*initialEnvironment));
}

void ModuleObject::initStatusSlot() {
  initReservedSlot(StatusSlot, Int32Value(MODULE_STATUS_UNLINKED));
}

void ModuleObject::initImportExportData(HandleArrayObject requestedModules,
                                        HandleArrayObject importEntries,
                                        HandleArrayObject localExportEntries,
                                        HandleArrayObject indirectExportEntries,
                                        HandleArrayObject starExportEntries) {
  initReservedSlot(RequestedModulesSlot, ObjectValue(*requestedModules));
  initReservedSlot(ImportEntriesSlot, ObjectValue(*importEntries));
  initReservedSlot(LocalExportEntriesSlot, ObjectValue(*localExportEntries));
  initReservedSlot(IndirectExportEntriesSlot,
                   ObjectValue(*indirectExportEntries));
  initReservedSlot(StarExportEntriesSlot, ObjectValue(*starExportEntries));
}

static bool FreezeObjectProperty(JSContext* cx, HandleNativeObject obj,
                                 uint32_t slot) {
  RootedObject property(cx, &obj->getSlot(slot).toObject());
  return FreezeObject(cx, property);
}

/* static */
bool ModuleObject::Freeze(JSContext* cx, HandleModuleObject self) {
  return FreezeObjectProperty(cx, self, RequestedModulesSlot) &&
         FreezeObjectProperty(cx, self, ImportEntriesSlot) &&
         FreezeObjectProperty(cx, self, LocalExportEntriesSlot) &&
         FreezeObjectProperty(cx, self, IndirectExportEntriesSlot) &&
         FreezeObjectProperty(cx, self, StarExportEntriesSlot) &&
         FreezeObject(cx, self);
}

#ifdef DEBUG

static inline bool CheckObjectFrozen(JSContext* cx, HandleObject obj,
                                     bool* result) {
  return TestIntegrityLevel(cx, obj, IntegrityLevel::Frozen, result);
}

static inline bool CheckObjectPropertyFrozen(JSContext* cx,
                                             HandleNativeObject obj,
                                             uint32_t slot, bool* result) {
  RootedObject property(cx, &obj->getSlot(slot).toObject());
  return CheckObjectFrozen(cx, property, result);
}

/* static */ inline bool ModuleObject::AssertFrozen(JSContext* cx,
                                                    HandleModuleObject self) {
  static const mozilla::EnumSet<ModuleSlot> slotsToCheck = {
      RequestedModulesSlot, ImportEntriesSlot, LocalExportEntriesSlot,
      IndirectExportEntriesSlot, StarExportEntriesSlot};

  bool frozen = false;
  for (auto slot : slotsToCheck) {
    if (!CheckObjectPropertyFrozen(cx, self, slot, &frozen)) {
      return false;
    }
    MOZ_ASSERT(frozen);
  }

  if (!CheckObjectFrozen(cx, self, &frozen)) {
    return false;
  }
  MOZ_ASSERT(frozen);

  return true;
}

#endif

inline static void AssertModuleScopesMatch(ModuleObject* module) {
  MOZ_ASSERT(module->enclosingScope()->is<GlobalScope>());
  MOZ_ASSERT(IsGlobalLexicalEnvironment(
      &module->initialEnvironment().enclosingEnvironment()));
}

void ModuleObject::fixEnvironmentsAfterRealmMerge() {
  AssertModuleScopesMatch(this);
  initialEnvironment().fixEnclosingEnvironmentAfterRealmMerge(
      script()->global());
  AssertModuleScopesMatch(this);
}

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
  MOZ_ASSERT(status >= MODULE_STATUS_UNLINKED &&
             status <= MODULE_STATUS_EVALUATED_ERROR);
}

ModuleStatus ModuleObject::status() const {
  ModuleStatus status = getReservedSlot(StatusSlot).toInt32();
  AssertValidModuleStatus(status);
  return status;
}

bool ModuleObject::isAsync() const {
  return getReservedSlot(AsyncSlot).toBoolean();
}

bool ModuleObject::isAsyncEvaluating() const {
  if (getReservedSlot(AsyncEvaluatingPostOrderSlot).isUndefined()) {
    return false;
  }
  return getReservedSlot(AsyncEvaluatingPostOrderSlot).toPrivateUint32() !=
         ASYNC_EVALUATING_POST_ORDER_FALSE;
}

void ModuleObject::setAsyncEvaluatingFalse() {
  if (AsyncPostOrder == getAsyncEvaluatingPostOrder()) {
    // If this condition is true, we can reset postOrder.
    // Graph is not re-entrant and any future modules will be independent from
    // this one.
    AsyncPostOrder = ASYNC_EVALUATING_POST_ORDER_INIT;
  }
  return setReservedSlot(AsyncEvaluatingPostOrderSlot,
                         PrivateUint32Value(ASYNC_EVALUATING_POST_ORDER_FALSE));
}

uint32_t ModuleObject::dfsIndex() const {
  return getReservedSlot(DFSIndexSlot).toInt32();
}

uint32_t ModuleObject::dfsAncestorIndex() const {
  return getReservedSlot(DFSAncestorIndexSlot).toInt32();
}

JSObject* ModuleObject::topLevelCapability() const {
  Value capability = getReservedSlot(TopLevelCapabilitySlot);
  MOZ_RELEASE_ASSERT(capability.isObject());
  return &capability.toObject();
}

PromiseObject* ModuleObject::createTopLevelCapability(
    JSContext* cx, HandleModuleObject module) {
  MOZ_ASSERT(module->getReservedSlot(TopLevelCapabilitySlot).isUndefined());
  Rooted<PromiseObject*> resultPromise(cx, CreatePromiseObjectForAsync(cx));
  if (!resultPromise) {
    return nullptr;
  }
  module->setInitialTopLevelCapability(resultPromise);
  return resultPromise;
}

void ModuleObject::setInitialTopLevelCapability(HandleObject promiseObj) {
  initReservedSlot(TopLevelCapabilitySlot, ObjectValue(*promiseObj));
}

inline ListObject* ModuleObject::asyncParentModules() const {
  return &getReservedSlot(AsyncParentModulesSlot).toObject().as<ListObject>();
}

bool ModuleObject::appendAsyncParentModule(JSContext* cx,
                                           HandleModuleObject self,
                                           HandleModuleObject parent) {
  Rooted<Value> parentValue(cx, ObjectValue(*parent));
  return self->asyncParentModules()->append(cx, parentValue);
}

uint32_t ModuleObject::pendingAsyncDependencies() const {
  return getReservedSlot(PendingAsyncDependenciesSlot).toInt32();
}

uint32_t ModuleObject::getAsyncEvaluatingPostOrder() const {
  MOZ_ASSERT(isAsyncEvaluating());
  return getReservedSlot(AsyncEvaluatingPostOrderSlot).toPrivateUint32();
}

void ModuleObject::setPendingAsyncDependencies(uint32_t newValue) {
  return setReservedSlot(PendingAsyncDependenciesSlot, NumberValue(newValue));
}

void ModuleObject::setCycleRoot(ModuleObject* cycleRoot) {
  return setReservedSlot(CycleRootSlot, ObjectValue(*cycleRoot));
}

ModuleObject* ModuleObject::getCycleRoot() const {
  Value cycleRoot = getReservedSlot(CycleRootSlot);
  MOZ_RELEASE_ASSERT(cycleRoot.isObject());
  return &cycleRoot.toObject().as<ModuleObject>();
}

bool ModuleObject::hasTopLevelCapability() const {
  return !getReservedSlot(TopLevelCapabilitySlot).isUndefined();
}

bool ModuleObject::hadEvaluationError() const {
  return status() == MODULE_STATUS_EVALUATED_ERROR;
}

void ModuleObject::setEvaluationError(HandleValue newValue) {
  setReservedSlot(StatusSlot, Int32Value(MODULE_STATUS_EVALUATED_ERROR));
  return setReservedSlot(EvaluationErrorSlot, newValue);
}

Value ModuleObject::evaluationError() const {
  MOZ_ASSERT(hadEvaluationError());
  return getReservedSlot(EvaluationErrorSlot);
}

JSObject* ModuleObject::metaObject() const {
  Value value = getReservedSlot(MetaObjectSlot);
  if (value.isObject()) {
    return &value.toObject();
  }

  MOZ_ASSERT(value.isUndefined());
  return nullptr;
}

void ModuleObject::setMetaObject(JSObject* obj) {
  MOZ_ASSERT(obj);
  MOZ_ASSERT(!metaObject());
  setReservedSlot(MetaObjectSlot, ObjectValue(*obj));
}

Scope* ModuleObject::enclosingScope() const {
  return script()->enclosingScope();
}

/* static */
void ModuleObject::trace(JSTracer* trc, JSObject* obj) {
  ModuleObject& module = obj->as<ModuleObject>();

  if (module.hasImportBindings()) {
    module.importBindings().trace(trc);
  }
}

/* static */
bool ModuleObject::instantiateFunctionDeclarations(JSContext* cx,
                                                   HandleModuleObject self) {
#ifdef DEBUG
  MOZ_ASSERT(self->status() == MODULE_STATUS_LINKING);
  if (!AssertFrozen(cx, self)) {
    return false;
  }
#endif
  // |self| initially manages this vector.
  frontend::FunctionDeclarationVector* funDecls =
      GetFunctionDeclarations(self.get());
  if (!funDecls) {
    JS_ReportErrorASCII(
        cx, "Module function declarations have already been instantiated");
    return false;
  }

  RootedModuleEnvironmentObject env(cx, &self->initialEnvironment());
  RootedObject obj(cx);
  RootedValue value(cx);
  RootedFunction fun(cx);
  RootedPropertyName name(cx);

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

  // Transfer ownership of the vector from |self|, then free the vector, once
  // its contents are no longer needed.
  self->setReservedSlot(FunctionDeclarationsSlot, UndefinedValue());
  js_delete(funDecls);
  return true;
}

/* static */
bool ModuleObject::execute(JSContext* cx, HandleModuleObject self,
                           MutableHandleValue rval) {
#ifdef DEBUG
  MOZ_ASSERT(self->status() == MODULE_STATUS_EVALUATING ||
             self->status() == MODULE_STATUS_EVALUATED);
  if (!AssertFrozen(cx, self)) {
    return false;
  }
#endif

  RootedScript script(cx, self->script());

  // The top-level script if a module is only ever executed once. Clear the
  // reference at exit to prevent us keeping this alive unnecessarily. This is
  // kept while executing so it is available to the debugger.
  auto guardA = mozilla::MakeScopeExit(
      [&] { self->setReservedSlot(ScriptSlot, UndefinedValue()); });

  RootedModuleEnvironmentObject env(cx, self->environment());
  if (!env) {
    JS_ReportErrorASCII(cx,
                        "Module declarations have not yet been instantiated");
    return false;
  }

  return Execute(cx, script, env, rval);
}

/* static */
ModuleNamespaceObject* ModuleObject::createNamespace(JSContext* cx,
                                                     HandleModuleObject self,
                                                     HandleObject exports) {
  MOZ_ASSERT(!self->namespace_());
  MOZ_ASSERT(exports->is<ArrayObject>());

  auto bindings = cx->make_unique<IndirectBindingMap>();
  if (!bindings) {
    return nullptr;
  }

  auto ns = ModuleNamespaceObject::create(cx, self, exports.as<ArrayObject>(),
                                          std::move(bindings));
  if (!ns) {
    return nullptr;
  }

  self->initReservedSlot(NamespaceSlot, ObjectValue(*ns));
  return ns;
}

/* static */
bool ModuleObject::createEnvironment(JSContext* cx, HandleModuleObject self) {
  RootedModuleEnvironmentObject env(cx,
                                    ModuleEnvironmentObject::create(cx, self));
  if (!env) {
    return false;
  }

  self->setInitialEnvironment(env);
  return true;
}

static bool InvokeSelfHostedMethod(JSContext* cx, HandleModuleObject self,
                                   HandlePropertyName name,
                                   MutableHandleValue rval) {
  RootedValue thisv(cx, ObjectValue(*self));
  FixedInvokeArgs<0> args(cx);

  return CallSelfHostedFunction(cx, name, thisv, args, rval);
}

/* static */
bool ModuleObject::Instantiate(JSContext* cx, HandleModuleObject self) {
  RootedValue ignored(cx);
  return InvokeSelfHostedMethod(cx, self, cx->names().ModuleInstantiate,
                                &ignored);
}

/* static */
bool ModuleObject::Evaluate(JSContext* cx, HandleModuleObject self,
                            MutableHandleValue rval) {
  return InvokeSelfHostedMethod(cx, self, cx->names().ModuleEvaluate, rval);
}

/* static */
ModuleNamespaceObject* ModuleObject::GetOrCreateModuleNamespace(
    JSContext* cx, HandleModuleObject self) {
  FixedInvokeArgs<1> args(cx);
  args[0].setObject(*self);

  RootedValue result(cx);
  if (!CallSelfHostedFunction(cx, cx->names().GetModuleNamespace,
                              UndefinedHandleValue, args, &result)) {
    return nullptr;
  }

  return &result.toObject().as<ModuleNamespaceObject>();
}

DEFINE_GETTER_FUNCTIONS(ModuleObject, namespace_, NamespaceSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, status, StatusSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, evaluationError, EvaluationErrorSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, requestedModules, RequestedModulesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, importEntries, ImportEntriesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, localExportEntries,
                        LocalExportEntriesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, indirectExportEntries,
                        IndirectExportEntriesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, starExportEntries, StarExportEntriesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, dfsIndex, DFSIndexSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, dfsAncestorIndex, DFSAncestorIndexSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, async, AsyncSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, topLevelCapability,
                        TopLevelCapabilitySlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, asyncEvaluatingPostOrder,
                        AsyncEvaluatingPostOrderSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, asyncParentModules,
                        AsyncParentModulesSlot)
DEFINE_GETTER_FUNCTIONS(ModuleObject, pendingAsyncDependencies,
                        PendingAsyncDependenciesSlot)

/* static */
bool GlobalObject::initModuleProto(JSContext* cx,
                                   Handle<GlobalObject*> global) {
  static const JSPropertySpec protoAccessors[] = {
      JS_PSG("namespace", ModuleObject_namespace_Getter, 0),
      JS_PSG("status", ModuleObject_statusGetter, 0),
      JS_PSG("evaluationError", ModuleObject_evaluationErrorGetter, 0),
      JS_PSG("requestedModules", ModuleObject_requestedModulesGetter, 0),
      JS_PSG("importEntries", ModuleObject_importEntriesGetter, 0),
      JS_PSG("localExportEntries", ModuleObject_localExportEntriesGetter, 0),
      JS_PSG("indirectExportEntries", ModuleObject_indirectExportEntriesGetter,
             0),
      JS_PSG("starExportEntries", ModuleObject_starExportEntriesGetter, 0),
      JS_PSG("dfsIndex", ModuleObject_dfsIndexGetter, 0),
      JS_PSG("dfsAncestorIndex", ModuleObject_dfsAncestorIndexGetter, 0),
      JS_PSG("async", ModuleObject_asyncGetter, 0),
      JS_PSG("topLevelCapability", ModuleObject_topLevelCapabilityGetter, 0),
      JS_PSG("asyncEvaluatingPostOrder",
             ModuleObject_asyncEvaluatingPostOrderGetter, 0),
      JS_PSG("asyncParentModules", ModuleObject_asyncParentModulesGetter, 0),
      JS_PSG("pendingAsyncDependencies",
             ModuleObject_pendingAsyncDependenciesGetter, 0),
      JS_PS_END};

  static const JSFunctionSpec protoFunctions[] = {
      JS_SELF_HOSTED_FN("getExportedNames", "ModuleGetExportedNames", 1, 0),
      JS_SELF_HOSTED_FN("resolveExport", "ModuleResolveExport", 2, 0),
      JS_SELF_HOSTED_FN("declarationInstantiation", "ModuleInstantiate", 0, 0),
      JS_SELF_HOSTED_FN("evaluation", "ModuleEvaluate", 0, 0),
      JS_SELF_HOSTED_FN("gatherAsyncParentCompletions",
                        "GatherAsyncParentCompletions", 2, 0),
      JS_FS_END};

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
  if (!proto) {
    return false;
  }

  if (!DefinePropertiesAndFunctions(cx, proto, protoAccessors,
                                    protoFunctions)) {
    return false;
  }

  global->setReservedSlot(MODULE_PROTO, ObjectValue(*proto));
  return true;
}

#undef DEFINE_GETTER_FUNCTIONS
#undef DEFINE_STRING_ACCESSOR_METHOD
#undef DEFINE_ARRAY_SLOT_ACCESSOR

///////////////////////////////////////////////////////////////////////////
// ModuleBuilder

ModuleBuilder::ModuleBuilder(JSContext* cx,
                             const frontend::EitherParser& eitherParser)
    : cx_(cx),
      eitherParser_(eitherParser),
      requestedModuleSpecifiers_(cx),
      importEntries_(cx),
      exportEntries_(cx),
      exportNames_(cx) {}

bool ModuleBuilder::noteFunctionDeclaration(JSContext* cx, uint32_t funIndex) {
  if (!functionDecls_.emplaceBack(funIndex)) {
    js::ReportOutOfMemory(cx);
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
  metadata.requestedModules = std::move(requestedModules_);

  // Step 5.
  if (!metadata.importEntries.reserve(importEntries_.count())) {
    js::ReportOutOfMemory(cx_);
    return false;
  }
  for (auto r = importEntries_.all(); !r.empty(); r.popFront()) {
    frontend::StencilModuleEntry& entry = r.front().value();
    metadata.importEntries.infallibleAppend(entry);
  }

  // Steps 6-11.
  for (const frontend::StencilModuleEntry& exp : exportEntries_) {
    if (!exp.specifier) {
      frontend::StencilModuleEntry* importEntry = importEntryFor(exp.localName);
      if (!importEntry) {
        if (!metadata.localExportEntries.append(exp)) {
          js::ReportOutOfMemory(cx_);
          return false;
        }
      } else {
        if (!importEntry->importName) {
          if (!metadata.localExportEntries.append(exp)) {
            js::ReportOutOfMemory(cx_);
            return false;
          }
        } else {
          // All names should have already been marked as used-by-stencil.
          auto entry = frontend::StencilModuleEntry::exportFromEntry(
              importEntry->specifier, importEntry->importName, exp.exportName,
              exp.lineno, exp.column);
          if (!metadata.indirectExportEntries.append(entry)) {
            js::ReportOutOfMemory(cx_);
            return false;
          }
        }
      }
    } else if (!exp.importName && !exp.exportName) {
      if (!metadata.starExportEntries.append(exp)) {
        js::ReportOutOfMemory(cx_);
        return false;
      }
    } else {
      if (!metadata.indirectExportEntries.append(exp)) {
        js::ReportOutOfMemory(cx_);
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

enum class ModuleArrayType {
  ImportEntryObject,
  ExportEntryObject,
  RequestedModuleObject,
};

static ArrayObject* ModuleBuilderInitArray(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    ModuleArrayType arrayType,
    const frontend::StencilModuleMetadata::EntryVector& vector) {
  RootedArrayObject resultArray(
      cx, NewDenseFullyAllocatedArray(cx, vector.length()));
  if (!resultArray) {
    return nullptr;
  }

  resultArray->ensureDenseInitializedLength(0, vector.length());

  RootedAtom specifier(cx);
  RootedAtom localName(cx);
  RootedAtom importName(cx);
  RootedAtom exportName(cx);
  RootedObject req(cx);
  RootedObject moduleRequest(cx);

  for (uint32_t i = 0; i < vector.length(); ++i) {
    const frontend::StencilModuleEntry& entry = vector[i];

    if (entry.specifier) {
      specifier = atomCache.getExistingAtomAt(cx, entry.specifier);
      MOZ_ASSERT(specifier);
    } else {
      MOZ_ASSERT(!specifier);
    }

    if (entry.localName) {
      localName = atomCache.getExistingAtomAt(cx, entry.localName);
      MOZ_ASSERT(localName);
    } else {
      MOZ_ASSERT(!localName);
    }

    if (entry.importName) {
      importName = atomCache.getExistingAtomAt(cx, entry.importName);
      MOZ_ASSERT(importName);
    } else {
      importName = nullptr;
    }

    if (entry.exportName) {
      exportName = atomCache.getExistingAtomAt(cx, entry.exportName);
      MOZ_ASSERT(exportName);
    } else {
      MOZ_ASSERT(!exportName);
    }

    moduleRequest = ModuleRequestObject::create(cx, specifier);
    if (!moduleRequest) {
      return nullptr;
    }

    switch (arrayType) {
      case ModuleArrayType::ImportEntryObject:
        MOZ_ASSERT(localName);
        req = ImportEntryObject::create(cx, moduleRequest, importName,
                                        localName, entry.lineno, entry.column);
        break;
      case ModuleArrayType::ExportEntryObject:
        req =
            ExportEntryObject::create(cx, exportName, moduleRequest, importName,
                                      localName, entry.lineno, entry.column);
        break;
      case ModuleArrayType::RequestedModuleObject:
        req = RequestedModuleObject::create(cx, moduleRequest, entry.lineno,
                                            entry.column);
        // TODO: Make this consistent with other object types.
        if (req && !FreezeObject(cx, req)) {
          return nullptr;
        }
        break;
    }
    if (!req) {
      return nullptr;
    }
    resultArray->initDenseElement(i, ObjectValue(*req));
  }

  return resultArray;
}

// Use StencilModuleMetadata data to fill in ModuleObject
bool frontend::StencilModuleMetadata::initModule(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    JS::Handle<ModuleObject*> module) const {
  RootedArrayObject requestedModulesObject(
      cx, ModuleBuilderInitArray(cx, atomCache,
                                 ModuleArrayType::RequestedModuleObject,
                                 requestedModules));
  if (!requestedModulesObject) {
    return false;
  }

  RootedArrayObject importEntriesObject(
      cx,
      ModuleBuilderInitArray(cx, atomCache, ModuleArrayType::ImportEntryObject,
                             importEntries));
  if (!importEntriesObject) {
    return false;
  }

  RootedArrayObject localExportEntriesObject(
      cx,
      ModuleBuilderInitArray(cx, atomCache, ModuleArrayType::ExportEntryObject,
                             localExportEntries));
  if (!localExportEntriesObject) {
    return false;
  }

  RootedArrayObject indirectExportEntriesObject(
      cx,
      ModuleBuilderInitArray(cx, atomCache, ModuleArrayType::ExportEntryObject,
                             indirectExportEntries));
  if (!indirectExportEntriesObject) {
    return false;
  }

  RootedArrayObject starExportEntriesObject(
      cx,
      ModuleBuilderInitArray(cx, atomCache, ModuleArrayType::ExportEntryObject,
                             starExportEntries));
  if (!starExportEntriesObject) {
    return false;
  }

  // Copy the vector of declarations to the ModuleObject.
  FunctionDeclarationVector functionDeclsCopy;
  if (!functionDeclsCopy.appendAll(functionDecls)) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  InitFunctionDeclarations(module.get(), std::move(functionDeclsCopy));

  Rooted<ListObject*> asyncParentModulesList(cx, ListObject::create(cx));
  if (!asyncParentModulesList) {
    return false;
  }

  if (!module->initAsyncSlots(cx, isAsync, asyncParentModulesList)) {
    return false;
  }

  module->initImportExportData(
      requestedModulesObject, importEntriesObject, localExportEntriesObject,
      indirectExportEntriesObject, starExportEntriesObject);

  return true;
}

bool ModuleBuilder::processImport(frontend::BinaryNode* importNode) {
  using namespace js::frontend;

  MOZ_ASSERT(importNode->isKind(ParseNodeKind::ImportDecl));

  auto* specList = &importNode->left()->as<ListNode>();
  MOZ_ASSERT(specList->isKind(ParseNodeKind::ImportSpecList));

  auto* moduleSpec = &importNode->right()->as<NameNode>();
  MOZ_ASSERT(moduleSpec->isKind(ParseNodeKind::StringExpr));

  auto module = moduleSpec->atom();
  if (!maybeAppendRequestedModule(module, moduleSpec)) {
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

      markUsedByStencil(module);
      markUsedByStencil(localName);
      markUsedByStencil(importName);
      entry = StencilModuleEntry::importEntry(module, localName, importName,
                                              line, column);
    } else {
      MOZ_ASSERT(item->isKind(ParseNodeKind::ImportNamespaceSpec));
      auto* spec = &item->as<UnaryNode>();

      auto* localNameNode = &spec->kid()->as<NameNode>();

      localName = localNameNode->atom();

      markUsedByStencil(module);
      markUsedByStencil(localName);
      entry = StencilModuleEntry::importNamespaceEntry(module, localName, line,
                                                       column);
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

  auto* moduleSpec = &exportNode->right()->as<NameNode>();
  MOZ_ASSERT(moduleSpec->isKind(ParseNodeKind::StringExpr));

  auto module = moduleSpec->atom();

  if (!maybeAppendRequestedModule(module, moduleSpec)) {
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

      markUsedByStencil(module);
      markUsedByStencil(importName);
      markUsedByStencil(exportName);
      entry = StencilModuleEntry::exportFromEntry(module, importName,
                                                  exportName, line, column);
    } else if (spec->isKind(ParseNodeKind::ExportNamespaceSpec)) {
      auto* exportNameNode = &spec->as<UnaryNode>().kid()->as<NameNode>();

      exportName = exportNameNode->atom();

      markUsedByStencil(module);
      markUsedByStencil(exportName);
      entry = StencilModuleEntry::exportNamespaceFromEntry(module, exportName,
                                                           line, column);
    } else {
      MOZ_ASSERT(spec->isKind(ParseNodeKind::ExportBatchSpecStmt));

      markUsedByStencil(module);
      entry = StencilModuleEntry::exportBatchFromEntry(module, line, column);
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

bool ModuleBuilder::maybeAppendRequestedModule(
    frontend::TaggedParserAtomIndex specifier, frontend::ParseNode* node) {
  if (requestedModuleSpecifiers_.has(specifier)) {
    return true;
  }

  uint32_t line;
  uint32_t column;
  eitherParser_.computeLineAndColumn(node->pn_pos.begin, &line, &column);

  markUsedByStencil(specifier);
  auto entry =
      frontend::StencilModuleEntry::moduleRequest(specifier, line, column);
  if (!requestedModules_.append(entry)) {
    js::ReportOutOfMemory(cx_);
    return false;
  }

  return requestedModuleSpecifiers_.put(specifier);
}

void ModuleBuilder::markUsedByStencil(frontend::TaggedParserAtomIndex name) {
  eitherParser_.parserAtoms().markUsedByStencil(name);
}

template <typename T>
ArrayObject* js::CreateArray(JSContext* cx,
                             const JS::Rooted<GCVector<T>>& vector) {
  uint32_t length = vector.length();
  RootedArrayObject array(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!array) {
    return nullptr;
  }

  array->setDenseInitializedLength(length);
  for (uint32_t i = 0; i < length; i++) {
    array->initDenseElement(i, ObjectValue(*vector[i]));
  }

  return array;
}

JSObject* js::GetOrCreateModuleMetaObject(JSContext* cx,
                                          HandleObject moduleArg) {
  HandleModuleObject module = moduleArg.as<ModuleObject>();
  if (JSObject* obj = module->metaObject()) {
    return obj;
  }

  RootedObject metaObject(cx,
                          NewObjectWithGivenProto<PlainObject>(cx, nullptr));
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

JSObject* js::CallModuleResolveHook(JSContext* cx,
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

  return result;
}

bool js::AsyncModuleExecutionFulfilledHandler(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSFunction& func = args.callee().as<JSFunction>();

  Rooted<ModuleObject*> module(
      cx, &func.getExtendedSlot(FunctionExtended::MODULE_SLOT)
               .toObject()
               .as<ModuleObject>());
  AsyncModuleExecutionFulfilled(cx, module);
  args.rval().setUndefined();
  return true;
}

bool js::AsyncModuleExecutionRejectedHandler(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSFunction& func = args.callee().as<JSFunction>();
  Rooted<ModuleObject*> module(
      cx, &func.getExtendedSlot(FunctionExtended::MODULE_SLOT)
               .toObject()
               .as<ModuleObject>());
  AsyncModuleExecutionRejected(cx, module, args.get(0));
  args.rval().setUndefined();
  return true;
}

// Top Level Await
// https://tc39.es/proposal-top-level-await/#sec-gather-async-parent-completions
bool ModuleObject::GatherAsyncParentCompletions(
    JSContext* cx, HandleModuleObject module,
    MutableHandleArrayObject execList) {
  FixedInvokeArgs<1> args(cx);
  args[0].setObject(*module);

  RootedValue rval(cx);
  if (!CallSelfHostedFunction(cx, cx->names().GatherAsyncParentCompletions,
                              UndefinedHandleValue, args, &rval)) {
    // This will happen if we OOM, we don't have a good way of handling this in
    // this specific situationn (promise resolution is in progress) so we will
    // reject the promise.
    return false;
  }
  execList.set(&rval.toObject().as<ArrayObject>());
  return true;
}

// Top Level Await
// https://tc39.es/proposal-top-level-await/#sec-asyncmodulexecutionfulfilled
void js::AsyncModuleExecutionFulfilled(JSContext* cx,
                                       HandleModuleObject module) {
  // Step 1.
  MOZ_ASSERT(module->status() == MODULE_STATUS_EVALUATED);

  // Step 2.
  MOZ_ASSERT(module->isAsyncEvaluating());

  if (module->hasTopLevelCapability()) {
    MOZ_ASSERT(module->getCycleRoot() == module);
    ModuleObject::topLevelCapabilityResolve(cx, module);
  }

  RootedArrayObject sortedList(cx);
  if (!ModuleObject::GatherAsyncParentCompletions(cx, module, &sortedList)) {
    // We have OOM'd -- all bets are off, reject the promise. Not much more we
    // can do.
    MOZ_ASSERT(cx->isExceptionPending());
    RootedValue exception(cx);
    if (!cx->getPendingException(&exception)) {
      return;
    }
    cx->clearPendingException();
    AsyncModuleExecutionRejected(cx, module, exception);
  }

  // this is out of step with the spec in order to be able to OOM
  module->setAsyncEvaluatingFalse();

  RootedValue ignored(cx);
  Rooted<ModuleObject*> m(cx);

  uint32_t length = sortedList->length();
  for (uint32_t i = 0; i < length; i++) {
    m = &sortedList->getDenseElement(i).toObject().as<ModuleObject>();
    // Step 2.
    if (!m->isAsyncEvaluating()) {
      MOZ_ASSERT(m->hadEvaluationError());
      return;
    }

    if (m->isAsync()) {
      // Steps for ExecuteAsyncModule
      MOZ_ASSERT(m->status() == MODULE_STATUS_EVALUATING ||
                 m->status() == MODULE_STATUS_EVALUATED);
      MOZ_ASSERT(m->isAsync());
      MOZ_ASSERT(m->isAsyncEvaluating());
      ModuleObject::execute(cx, m, &ignored);
    } else {
      if (!ModuleObject::execute(cx, m, &ignored)) {
        MOZ_ASSERT(cx->isExceptionPending());
        RootedValue exception(cx);
        if (!cx->getPendingException(&exception)) {
          return;
        }
        cx->clearPendingException();
        AsyncModuleExecutionRejected(cx, m, exception);
      } else {
        m->setAsyncEvaluatingFalse();
        if (m->hasTopLevelCapability()) {
          MOZ_ASSERT(m->getCycleRoot() == m);
          ModuleObject::topLevelCapabilityResolve(cx, m);
        }
      }
    }
  }

  // Step 6.
  // Return undefined.
}

// https://tc39.es/proposal-top-level-await/#sec-asyncmodulexecutionrejected
void js::AsyncModuleExecutionRejected(JSContext* cx, HandleModuleObject module,
                                      HandleValue error) {
  // Step 1.
  MOZ_ASSERT(module->status() == MODULE_STATUS_EVALUATED ||
             module->status() == MODULE_STATUS_EVALUATED_ERROR);

  // Step 2.
  if (!module->isAsyncEvaluating()) {
    MOZ_ASSERT(module->hadEvaluationError());
    return;
  }

  // Step 3.
  MOZ_ASSERT(!module->hadEvaluationError());

  // Step 4.
  module->setEvaluationError(error);

  // Step 5.
  module->setAsyncEvaluatingFalse();

  // Step 6.
  uint32_t length = module->asyncParentModules()->length();
  Rooted<ModuleObject*> parent(cx);
  for (uint32_t i = 0; i < length; i++) {
    parent =
        &module->asyncParentModules()->get(i).toObject().as<ModuleObject>();
    AsyncModuleExecutionRejected(cx, parent, error);
  }

  // Step 7.
  if (module->hasTopLevelCapability()) {
    MOZ_ASSERT(module->getCycleRoot() == module);
    ModuleObject::topLevelCapabilityReject(cx, module, error);
  }

  // Return undefined.
}

bool ModuleObject::topLevelCapabilityResolve(JSContext* cx,
                                             HandleModuleObject module) {
  RootedValue rval(cx);
  Rooted<PromiseObject*> promise(
      cx, &module->topLevelCapability()->as<PromiseObject>());
  return AsyncFunctionReturned(cx, promise, rval);
}

bool ModuleObject::topLevelCapabilityReject(JSContext* cx,
                                            HandleModuleObject module,
                                            HandleValue error) {
  Rooted<PromiseObject*> promise(
      cx, &module->topLevelCapability()->as<PromiseObject>());
  return AsyncFunctionThrown(cx, promise, error);
}

JSObject* js::StartDynamicModuleImport(JSContext* cx, HandleScript script,
                                       HandleValue specifierArg) {
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

  RootedValue referencingPrivate(cx,
                                 script->sourceObject()->canonicalPrivate());
  cx->runtime()->addRefScriptPrivate(referencingPrivate);

  RootedAtom specifierAtom(cx, AtomizeString(cx, specifier));
  if (!specifierAtom) {
    if (!RejectPromiseWithPendingError(cx, promise)) {
      return nullptr;
    }
    return promise;
  }

  RootedObject moduleRequest(cx,
                             ModuleRequestObject::create(cx, specifierAtom));
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

  js::ReportExceptionClosure reportExn(error);
  PrepareScriptEnvironmentAndInvoke(cx, cx->global(), reportExn);

  args.rval().setUndefined();
  return true;
};

bool js::OnModuleEvaluationFailure(JSContext* cx,
                                   HandleObject evaluationPromise) {
  if (evaluationPromise == nullptr) {
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

  RootedAtom specifier(
      cx, AtomizeString(cx, resolvedModuleParams->get(1).toString()));
  if (!specifier) {
    return false;
  }

  Rooted<PromiseObject*> promise(cx, TargetFromHandler<PromiseObject>(args));

  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  RootedObject moduleRequest(cx, ModuleRequestObject::create(cx, specifier));
  if (!moduleRequest) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedObject result(
      cx, CallModuleResolveHook(cx, referencingPrivate, moduleRequest));

  if (!result) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedModuleObject module(cx, &result->as<ModuleObject>());
  if (module->status() != MODULE_STATUS_EVALUATED) {
    JS_ReportErrorASCII(
        cx, "Unevaluated or errored module returned by module resolve hook");
    return RejectPromiseWithPendingError(cx, promise);
  }

  MOZ_ASSERT(module->getCycleRoot()
                 ->topLevelCapability()
                 ->as<PromiseObject>()
                 .state() == JS::PromiseState::Fulfilled);

  RootedObject ns(cx, ModuleObject::GetOrCreateModuleNamespace(cx, module));
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
  // If we do not have an evaluation promise for the module, we can assume that
  // evaluation has failed or been interrupted -- we can reject the dynamic
  // module.
  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  if (!evaluationPromise) {
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

bool js::FinishDynamicModuleImport_NoTLA(JSContext* cx,
                                         JS::DynamicImportStatus status,
                                         HandleValue referencingPrivate,
                                         HandleObject moduleRequest,
                                         HandleObject promiseArg) {
  MOZ_ASSERT_IF(cx->isExceptionPending(),
                status == JS::DynamicImportStatus::Failed);

  Handle<PromiseObject*> promise = promiseArg.as<PromiseObject>();

  auto releasePrivate = mozilla::MakeScopeExit(
      [&] { cx->runtime()->releaseScriptPrivate(referencingPrivate); });

  if (status == JS::DynamicImportStatus::Failed) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedObject result(
      cx, CallModuleResolveHook(cx, referencingPrivate, moduleRequest));
  if (!result) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedModuleObject module(cx, &result->as<ModuleObject>());
  if (module->status() != MODULE_STATUS_EVALUATED) {
    JS_ReportErrorASCII(
        cx, "Unevaluated or errored module returned by module resolve hook");
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedObject ns(cx, ModuleObject::GetOrCreateModuleNamespace(cx, module));
  if (!ns) {
    return RejectPromiseWithPendingError(cx, promise);
  }

  RootedValue value(cx, ObjectValue(*ns));
  return PromiseObject::resolve(cx, promise, value);
}

template <XDRMode mode>
XDRResult js::XDRExportEntries(XDRState<mode>* xdr,
                               MutableHandleArrayObject vec) {
  JSContext* cx = xdr->cx();
  Rooted<GCVector<ExportEntryObject*>> expVec(cx);
  RootedExportEntryObject expObj(cx);
  RootedAtom exportName(cx);
  RootedModuleRequestObject moduleRequest(cx);
  RootedAtom importName(cx);
  RootedAtom localName(cx);

  uint32_t length = 0;
  uint32_t lineNumber = 0;
  uint32_t columnNumber = 0;

  if (mode == XDR_ENCODE) {
    length = vec->length();
  }
  MOZ_TRY(xdr->codeUint32(&length));
  for (uint32_t i = 0; i < length; i++) {
    if (mode == XDR_ENCODE) {
      expObj = &vec->getDenseElement(i).toObject().as<ExportEntryObject>();

      exportName = expObj->exportName();
      moduleRequest = expObj->moduleRequest();
      importName = expObj->importName();
      localName = expObj->localName();
      lineNumber = expObj->lineNumber();
      columnNumber = expObj->columnNumber();
    }

    MOZ_TRY(XDRAtomOrNull(xdr, &exportName));
    MOZ_TRY(XDRModuleRequestObject(xdr, &moduleRequest, true));
    MOZ_TRY(XDRAtomOrNull(xdr, &importName));
    MOZ_TRY(XDRAtomOrNull(xdr, &localName));

    MOZ_TRY(xdr->codeUint32(&lineNumber));
    MOZ_TRY(xdr->codeUint32(&columnNumber));

    if (mode == XDR_DECODE) {
      expObj.set(ExportEntryObject::create(cx, exportName, moduleRequest,
                                           importName, localName, lineNumber,
                                           columnNumber));
      if (!expObj) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      if (!expVec.append(expObj)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  if (mode == XDR_DECODE) {
    RootedArrayObject expArr(cx, js::CreateArray(cx, expVec));
    if (!expArr) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    vec.set(expArr);
  }

  return Ok();
}

template <XDRMode mode>
XDRResult js::XDRRequestedModuleObject(
    XDRState<mode>* xdr, MutableHandleRequestedModuleObject reqObj) {
  JSContext* cx = xdr->cx();
  RootedModuleRequestObject moduleRequest(cx);
  uint32_t lineNumber = 0;
  uint32_t columnNumber = 0;
  if (mode == XDR_ENCODE) {
    moduleRequest = reqObj->moduleRequest();
    lineNumber = reqObj->lineNumber();
    columnNumber = reqObj->columnNumber();
  }

  MOZ_TRY(XDRModuleRequestObject(xdr, &moduleRequest, false));
  MOZ_TRY(xdr->codeUint32(&lineNumber));
  MOZ_TRY(xdr->codeUint32(&columnNumber));

  if (mode == XDR_DECODE) {
    reqObj.set(RequestedModuleObject::create(cx, moduleRequest, lineNumber,
                                             columnNumber));
    if (!reqObj) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template <XDRMode mode>
XDRResult js::XDRModuleRequestObject(
    XDRState<mode>* xdr, MutableHandleModuleRequestObject moduleRequestObj,
    bool allowNullSpecifier) {
  JSContext* cx = xdr->cx();
  RootedAtom specifier(cx);
  if (mode == XDR_ENCODE) {
    specifier = moduleRequestObj->specifier();
  }

  MOZ_TRY(XDRAtomOrNull(xdr, &specifier));

  if (mode == XDR_DECODE) {
    if (!allowNullSpecifier && !specifier) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    moduleRequestObj.set(ModuleRequestObject::create(cx, specifier));
    if (!moduleRequestObj) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template <XDRMode mode>
XDRResult js::XDRImportEntryObject(XDRState<mode>* xdr,
                                   MutableHandleImportEntryObject impObj) {
  JSContext* cx = xdr->cx();
  RootedModuleRequestObject moduleRequest(cx);
  RootedAtom importName(cx);
  RootedAtom localName(cx);
  uint32_t lineNumber = 0;
  uint32_t columnNumber = 0;
  if (mode == XDR_ENCODE) {
    moduleRequest = impObj->moduleRequest();
    importName = impObj->importName();
    localName = impObj->localName();
    lineNumber = impObj->lineNumber();
    columnNumber = impObj->columnNumber();
  }

  MOZ_TRY(XDRModuleRequestObject(xdr, &moduleRequest, true));
  MOZ_TRY(XDRAtomOrNull(xdr, &importName));
  MOZ_TRY(XDRAtomOrNull(xdr, &localName));
  MOZ_TRY(xdr->codeUint32(&lineNumber));
  MOZ_TRY(xdr->codeUint32(&columnNumber));

  if (mode == XDR_DECODE) {
    impObj.set(ImportEntryObject::create(cx, moduleRequest, importName,
                                         localName, lineNumber, columnNumber));
    if (!impObj) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template <XDRMode mode>
XDRResult js::XDRModuleObject(XDRState<mode>* xdr,
                              MutableHandleModuleObject modp) {
  JSContext* cx = xdr->cx();
  RootedModuleObject module(cx, modp);

  RootedScope enclosingScope(cx);
  RootedScript script(cx);

  RootedArrayObject requestedModules(cx);
  RootedArrayObject importEntries(cx);
  RootedArrayObject localExportEntries(cx);
  RootedArrayObject indirectExportEntries(cx);
  RootedArrayObject starExportEntries(cx);
  // funcDecls points to data traced by the ModuleObject,
  // but is itself heap-allocated so we don't need to
  // worry about rooting it again here.
  frontend::FunctionDeclarationVector* funcDecls;

  uint32_t requestedModulesLength = 0;
  uint32_t importEntriesLength = 0;
  uint32_t funcDeclLength = 0;

  if (mode == XDR_ENCODE) {
    module = modp.get();

    script.set(module->script());
    enclosingScope.set(module->enclosingScope());
    MOZ_ASSERT(!enclosingScope->as<GlobalScope>().hasBindings());

    requestedModules = &module->requestedModules();
    importEntries = &module->importEntries();
    localExportEntries = &module->localExportEntries();
    indirectExportEntries = &module->indirectExportEntries();
    starExportEntries = &module->starExportEntries();
    funcDecls = GetFunctionDeclarations(module.get());

    requestedModulesLength = requestedModules->length();
    importEntriesLength = importEntries->length();
    funcDeclLength = funcDecls->length();
  }

  /* ScriptSourceObject slot - ScriptSourceObject is created in XDRScript and is
   * set when init is called. */
  if (mode == XDR_DECODE) {
    enclosingScope.set(&cx->global()->emptyGlobalScope());
    module.set(ModuleObject::create(cx));
    if (!module) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  /* Script slot */
  MOZ_TRY(XDRScript(xdr, enclosingScope, nullptr, module, &script));

  if (mode == XDR_DECODE) {
    module->initScriptSlots(script);
    module->initStatusSlot();
  }

  /* Environment Slot */
  if (mode == XDR_DECODE) {
    if (!ModuleObject::createEnvironment(cx, module)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  /* Namespace Slot, Status Slot, EvaluationErrorSlot, MetaObject - Initialized
   * at instantiation */

  /* RequestedModules slot */
  RootedRequestedModuleVector reqVec(cx, GCVector<RequestedModuleObject*>(cx));
  RootedRequestedModuleObject reqObj(cx);
  MOZ_TRY(xdr->codeUint32(&requestedModulesLength));
  for (uint32_t i = 0; i < requestedModulesLength; i++) {
    if (mode == XDR_ENCODE) {
      reqObj = &module->requestedModules()
                    .getDenseElement(i)
                    .toObject()
                    .as<RequestedModuleObject>();
    }
    MOZ_TRY(XDRRequestedModuleObject(xdr, &reqObj));
    if (mode == XDR_DECODE) {
      if (!reqVec.append(reqObj)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }
  if (mode == XDR_DECODE) {
    RootedArrayObject reqArr(cx, js::CreateArray(cx, reqVec));
    if (!reqArr) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    requestedModules.set(reqArr);
  }

  /* ImportEntries slot */
  RootedImportEntryVector impVec(cx, GCVector<ImportEntryObject*>(cx));
  RootedImportEntryObject impObj(cx);
  MOZ_TRY(xdr->codeUint32(&importEntriesLength));
  for (uint32_t i = 0; i < importEntriesLength; i++) {
    if (mode == XDR_ENCODE) {
      impObj = &module->importEntries()
                    .getDenseElement(i)
                    .toObject()
                    .as<ImportEntryObject>();
    }
    MOZ_TRY(XDRImportEntryObject(xdr, &impObj));
    if (mode == XDR_DECODE) {
      if (!impVec.append(impObj)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  if (mode == XDR_DECODE) {
    RootedArrayObject impArr(cx, js::CreateArray(cx, impVec));
    if (!impArr) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    importEntries.set(impArr);
  }

  /* LocalExportEntries slot */
  MOZ_TRY(XDRExportEntries(xdr, &localExportEntries));
  /* IndirectExportEntries slot */
  MOZ_TRY(XDRExportEntries(xdr, &indirectExportEntries));
  /* StarExportEntries slot */
  MOZ_TRY(XDRExportEntries(xdr, &starExportEntries));

  /* FunctionDeclarations slot */
  uint32_t funIndex = 0;
  MOZ_TRY(xdr->codeUint32(&funcDeclLength));
  for (uint32_t i = 0; i < funcDeclLength; i++) {
    if (mode == XDR_ENCODE) {
      funIndex = (*funcDecls)[i];
    }

    MOZ_TRY(xdr->codeUint32(&funIndex));

    if (mode == XDR_DECODE) {
      if (!GetFunctionDeclarations(module.get())->append(funIndex)) {
        ReportOutOfMemory(cx);
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  /* ImportBindings slot, DFSIndex slot, DFSAncestorIndex slot -
   * Initialized at instantiation */
  if (mode == XDR_DECODE) {
    module->initImportExportData(requestedModules, importEntries,
                                 localExportEntries, indirectExportEntries,
                                 starExportEntries);
  }

  /* isAsync Slot */
  uint8_t isAsyncModule = 0;
  if (mode == XDR_ENCODE) {
    isAsyncModule = module->isAsync() ? 1 : 0;
  }

  MOZ_TRY(xdr->codeUint8(&isAsyncModule));

  if (mode == XDR_DECODE) {
    Rooted<ListObject*> asyncParentModulesList(cx, ListObject::create(cx));
    if (!asyncParentModulesList) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    module->initAsyncSlots(cx, isAsyncModule == 1, asyncParentModulesList);
  }

  modp.set(module);
  return Ok();
}

template XDRResult js::XDRModuleObject(XDRState<XDR_ENCODE>* xdr,
                                       MutableHandleModuleObject scriptp);

template XDRResult js::XDRModuleObject(XDRState<XDR_DECODE>* xdr,
                                       MutableHandleModuleObject scriptp);
