/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_ModuleObject_h
#define builtin_ModuleObject_h

#include "mozilla/HashTable.h"  // mozilla::{HashMap, DefaultHasher}
#include "mozilla/Maybe.h"      // mozilla::Maybe
#include "mozilla/Span.h"

#include <stddef.h>  // size_t
#include <stdint.h>  // int32_t, uint32_t

#include "gc/Barrier.h"        // HeapPtr
#include "gc/ZoneAllocator.h"  // CellAllocPolicy
#include "js/Class.h"          // JSClass, ObjectOpResult
#include "js/ColumnNumber.h"   // JS::ColumnNumberOneOrigin
#include "js/GCVector.h"
#include "js/Id.h"  // jsid
#include "js/Modules.h"
#include "js/Proxy.h"       // BaseProxyHandler
#include "js/RootingAPI.h"  // Rooted, Handle, MutableHandle
#include "js/TypeDecls.h"  // HandleValue, HandleId, HandleObject, HandleScript, MutableHandleValue, MutableHandleIdVector, MutableHandleObject
#include "js/UniquePtr.h"  // UniquePtr
#include "vm/JSObject.h"   // JSObject
#include "vm/NativeObject.h"   // NativeObject
#include "vm/ProxyObject.h"    // ProxyObject
#include "vm/SharedStencil.h"  // FunctionDeclarationVector

class JSAtom;
class JSScript;
class JSTracer;

namespace JS {
class PropertyDescriptor;
class Value;
}  // namespace JS

namespace js {

class ArrayObject;
class CyclicModuleFields;
class SyntheticModuleFields;
class ListObject;
class ModuleEnvironmentObject;
class ModuleObject;
class PromiseObject;
class ScriptSourceObject;

class ImportAttribute {
  const HeapPtr<JSAtom*> key_;
  const HeapPtr<JSString*> value_;

 public:
  ImportAttribute(Handle<JSAtom*> key, Handle<JSString*> value);

  JSAtom* key() const { return key_; }
  JSString* value() const { return value_; }

  void trace(JSTracer* trc);
};

using ImportAttributeVector = GCVector<ImportAttribute, 0, SystemAllocPolicy>;

class ModuleRequestObject : public NativeObject {
 public:
  enum {
    SpecifierSlot = 0,
    FirstUnsupportedAttributeKeySlot,
    ModuleTypeSlot,
    SlotCount
  };

  static const JSClass class_;
  static bool isInstance(HandleValue value);
  [[nodiscard]] static ModuleRequestObject* create(
      JSContext* cx, Handle<JSAtom*> specifier,
      Handle<ImportAttributeVector> maybeAttributes);
  [[nodiscard]] static ModuleRequestObject* create(JSContext* cx,
                                                   Handle<JSAtom*> specifier,
                                                   JS::ModuleType moduleType);

  JSAtom* specifier() const;
  JS::ModuleType moduleType() const;

  // We process import attributes earlier in the process, but according to the
  // spec, we should error during module evaluation if we encounter an
  // unsupported attribute. We want to generate a nice error message, so we need
  // to keep track of the first unsupported key we encounter.
  void setFirstUnsupportedAttributeKey(Handle<JSAtom*> key);
  bool hasFirstUnsupportedAttributeKey() const;
  JSAtom* getFirstUnsupportedAttributeKey() const;
};

using ModuleRequestVector =
    GCVector<HeapPtr<ModuleRequestObject*>, 0, SystemAllocPolicy>;

class ImportEntry {
  const HeapPtr<ModuleRequestObject*> moduleRequest_;
  const HeapPtr<JSAtom*> importName_;
  const HeapPtr<JSAtom*> localName_;

  // Line number (1-origin).
  const uint32_t lineNumber_;

  // Column number in UTF-16 code units.
  const JS::ColumnNumberOneOrigin columnNumber_;

 public:
  ImportEntry(Handle<ModuleRequestObject*> moduleRequest,
              Handle<JSAtom*> maybeImportName, Handle<JSAtom*> localName,
              uint32_t lineNumber, JS::ColumnNumberOneOrigin columnNumber);

  ModuleRequestObject* moduleRequest() const { return moduleRequest_; }
  JSAtom* importName() const { return importName_; }
  JSAtom* localName() const { return localName_; }
  uint32_t lineNumber() const { return lineNumber_; }
  JS::ColumnNumberOneOrigin columnNumber() const { return columnNumber_; }

  void trace(JSTracer* trc);
};

using ImportEntryVector = GCVector<ImportEntry, 0, SystemAllocPolicy>;

class ExportEntry {
  const HeapPtr<JSAtom*> exportName_;
  const HeapPtr<ModuleRequestObject*> moduleRequest_;
  const HeapPtr<JSAtom*> importName_;
  const HeapPtr<JSAtom*> localName_;

  // Line number (1-origin).
  const uint32_t lineNumber_;

  // Column number in UTF-16 code units.
  const JS::ColumnNumberOneOrigin columnNumber_;

 public:
  ExportEntry(Handle<JSAtom*> maybeExportName,
              Handle<ModuleRequestObject*> maybeModuleRequest,
              Handle<JSAtom*> maybeImportName, Handle<JSAtom*> maybeLocalName,
              uint32_t lineNumber, JS::ColumnNumberOneOrigin columnNumber);
  JSAtom* exportName() const { return exportName_; }
  ModuleRequestObject* moduleRequest() const { return moduleRequest_; }
  JSAtom* importName() const { return importName_; }
  JSAtom* localName() const { return localName_; }
  uint32_t lineNumber() const { return lineNumber_; }
  JS::ColumnNumberOneOrigin columnNumber() const { return columnNumber_; }

  void trace(JSTracer* trc);
};

using ExportEntryVector = GCVector<ExportEntry, 0, SystemAllocPolicy>;

class RequestedModule {
  const HeapPtr<ModuleRequestObject*> moduleRequest_;

  // Line number (1-origin).
  const uint32_t lineNumber_;

  // Column number in UTF-16 code units.
  const JS::ColumnNumberOneOrigin columnNumber_;

 public:
  RequestedModule(Handle<ModuleRequestObject*> moduleRequest,
                  uint32_t lineNumber, JS::ColumnNumberOneOrigin columnNumber);
  ModuleRequestObject* moduleRequest() const { return moduleRequest_; }
  uint32_t lineNumber() const { return lineNumber_; }
  JS::ColumnNumberOneOrigin columnNumber() const { return columnNumber_; }

  void trace(JSTracer* trc);
};

using RequestedModuleVector = GCVector<RequestedModule, 0, SystemAllocPolicy>;

class ResolvedBindingObject : public NativeObject {
 public:
  enum { ModuleSlot = 0, BindingNameSlot, SlotCount };

  static const JSClass class_;
  static bool isInstance(HandleValue value);
  static ResolvedBindingObject* create(JSContext* cx,
                                       Handle<ModuleObject*> module,
                                       Handle<JSAtom*> bindingName);
  ModuleObject* module() const;
  JSAtom* bindingName() const;
};

class IndirectBindingMap {
 public:
  void trace(JSTracer* trc);

  bool put(JSContext* cx, HandleId name,
           Handle<ModuleEnvironmentObject*> environment, HandleId targetName);

  size_t count() const { return map_ ? map_->count() : 0; }

  bool has(jsid name) const { return map_ ? map_->has(name) : false; }

  bool lookup(jsid name, ModuleEnvironmentObject** envOut,
              mozilla::Maybe<PropertyInfo>* propOut) const;

  template <typename Func>
  void forEachExportedName(Func func) const {
    if (!map_) {
      return;
    }

    for (auto r = map_->all(); !r.empty(); r.popFront()) {
      func(r.front().key());
    }
  }

 private:
  struct Binding {
    Binding(ModuleEnvironmentObject* environment, jsid targetName,
            PropertyInfo prop);
    HeapPtr<ModuleEnvironmentObject*> environment;
#ifdef DEBUG
    HeapPtr<jsid> targetName;
#endif
    PropertyInfo prop;
  };

  using Map = mozilla::HashMap<PreBarriered<jsid>, Binding,
                               mozilla::DefaultHasher<PreBarriered<jsid>>,
                               CellAllocPolicy>;

  mozilla::Maybe<Map> map_;
};

// Vector of atoms representing the names exported from a module namespace.
//
// This is used both on the stack and in the heap.
using ExportNameVector = GCVector<HeapPtr<JSAtom*>, 0, SystemAllocPolicy>;

class ModuleNamespaceObject : public ProxyObject {
 public:
  enum ModuleNamespaceSlot { ExportsSlot = 0, BindingsSlot };

  static bool isInstance(HandleValue value);
  static ModuleNamespaceObject* create(
      JSContext* cx, Handle<ModuleObject*> module,
      MutableHandle<UniquePtr<ExportNameVector>> exports,
      MutableHandle<UniquePtr<IndirectBindingMap>> bindings);

  ModuleObject& module();
  const ExportNameVector& exports() const;
  IndirectBindingMap& bindings();

  bool addBinding(JSContext* cx, Handle<JSAtom*> exportedName,
                  Handle<ModuleObject*> targetModule,
                  Handle<JSAtom*> targetName);

 private:
  struct ProxyHandler : public BaseProxyHandler {
    constexpr ProxyHandler() : BaseProxyHandler(&family, false) {}

    bool getOwnPropertyDescriptor(
        JSContext* cx, HandleObject proxy, HandleId id,
        MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const override;
    bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                        Handle<PropertyDescriptor> desc,
                        ObjectOpResult& result) const override;
    bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                         MutableHandleIdVector props) const override;
    bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                 ObjectOpResult& result) const override;
    bool getPrototype(JSContext* cx, HandleObject proxy,
                      MutableHandleObject protop) const override;
    bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                      ObjectOpResult& result) const override;
    bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy,
                                bool* isOrdinary,
                                MutableHandleObject protop) const override;
    bool setImmutablePrototype(JSContext* cx, HandleObject proxy,
                               bool* succeeded) const override;

    bool preventExtensions(JSContext* cx, HandleObject proxy,
                           ObjectOpResult& result) const override;
    bool isExtensible(JSContext* cx, HandleObject proxy,
                      bool* extensible) const override;
    bool has(JSContext* cx, HandleObject proxy, HandleId id,
             bool* bp) const override;
    bool get(JSContext* cx, HandleObject proxy, HandleValue receiver,
             HandleId id, MutableHandleValue vp) const override;
    bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
             HandleValue receiver, ObjectOpResult& result) const override;

    void trace(JSTracer* trc, JSObject* proxy) const override;
    void finalize(JS::GCContext* gcx, JSObject* proxy) const override;

    static const char family;
  };

  bool hasBindings() const;
  bool hasExports() const;

  ExportNameVector& mutableExports();

 public:
  static const ProxyHandler proxyHandler;
};

// Value types of [[Status]] in a Cyclic Module Record
// https://tc39.es/ecma262/#table-cyclic-module-fields
enum class ModuleStatus : int8_t {
  Unlinked,
  Linking,
  Linked,
  Evaluating,
  EvaluatingAsync,
  Evaluated,

  // Sub-state of Evaluated with error value set.
  //
  // This is not returned from ModuleObject::status(); use hadEvaluationError()
  // to check this.
  Evaluated_Error
};

// Special values for CyclicModuleFields' asyncEvaluatingPostOrderSlot field,
// which is used as part of the implementation of the AsyncEvaluation field of
// cyclic module records.
//
// The spec requires us to be able to tell the order in which the field was set
// to true for async evaluating modules.
//
// This is arranged by using an integer to record the order. After evaluation is
// complete the value is set to ASYNC_EVALUATING_POST_ORDER_CLEARED.
//
// See https://tc39.es/ecma262/#sec-cyclic-module-records for field defintion.
// See https://tc39.es/ecma262/#sec-async-module-execution-fulfilled for sort
// requirement.

// Initial value for the runtime's counter used to generate these values.
constexpr uint32_t ASYNC_EVALUATING_POST_ORDER_INIT = 1;

// Value that the field is set to after being cleared.
constexpr uint32_t ASYNC_EVALUATING_POST_ORDER_CLEARED = 0;

// Currently, the ModuleObject class is used to represent both the Source Text
// Module Record and the Synthetic Module Record. Ideally, this is something
// that should be refactored to follow the same hierarchy as in the spec.
// TODO: See Bug 1880519.
class ModuleObject : public NativeObject {
 public:
  // Module fields including those for AbstractModuleRecords described by:
  // https://tc39.es/ecma262/#sec-abstract-module-records
  enum ModuleSlot {
    ScriptSlot = 0,
    EnvironmentSlot,
    NamespaceSlot,
    CyclicModuleFieldsSlot,
    // `SyntheticModuleFields` if a synthetic module. Otherwise `undefined`.
    SyntheticModuleFieldsSlot,
    SlotCount
  };

  static const JSClass class_;

  static bool isInstance(HandleValue value);

  static ModuleObject* create(JSContext* cx);

  static ModuleObject* createSynthetic(
      JSContext* cx, MutableHandle<ExportNameVector> exportNames);

  // Initialize the slots on this object that are dependent on the script.
  void initScriptSlots(HandleScript script);

  void setInitialEnvironment(
      Handle<ModuleEnvironmentObject*> initialEnvironment);

  void initFunctionDeclarations(UniquePtr<FunctionDeclarationVector> decls);
  void initImportExportData(
      MutableHandle<RequestedModuleVector> requestedModules,
      MutableHandle<ImportEntryVector> importEntries,
      MutableHandle<ExportEntryVector> exportEntries, uint32_t localExportCount,
      uint32_t indirectExportCount, uint32_t starExportCount);
  static bool Freeze(JSContext* cx, Handle<ModuleObject*> self);
#ifdef DEBUG
  static bool AssertFrozen(JSContext* cx, Handle<ModuleObject*> self);
#endif

  JSScript* maybeScript() const;
  JSScript* script() const;
  const char* filename() const;
  ModuleEnvironmentObject& initialEnvironment() const;
  ModuleEnvironmentObject* environment() const;
  ModuleNamespaceObject* namespace_();
  ModuleStatus status() const;
  mozilla::Maybe<uint32_t> maybeDfsIndex() const;
  uint32_t dfsIndex() const;
  mozilla::Maybe<uint32_t> maybeDfsAncestorIndex() const;
  uint32_t dfsAncestorIndex() const;
  bool hadEvaluationError() const;
  Value maybeEvaluationError() const;
  Value evaluationError() const;
  JSObject* metaObject() const;
  ScriptSourceObject* scriptSourceObject() const;
  mozilla::Span<const RequestedModule> requestedModules() const;
  mozilla::Span<const ImportEntry> importEntries() const;
  mozilla::Span<const ExportEntry> localExportEntries() const;
  mozilla::Span<const ExportEntry> indirectExportEntries() const;
  mozilla::Span<const ExportEntry> starExportEntries() const;
  const ExportNameVector& syntheticExportNames() const;

  IndirectBindingMap& importBindings();

  void setStatus(ModuleStatus newStatus);
  void setDfsIndex(uint32_t index);
  void setDfsAncestorIndex(uint32_t index);
  void clearDfsIndexes();

  static PromiseObject* createTopLevelCapability(JSContext* cx,
                                                 Handle<ModuleObject*> module);
  bool hasTopLevelAwait() const;
  bool isAsyncEvaluating() const;
  void setAsyncEvaluating();
  void setEvaluationError(HandleValue newValue);
  void setPendingAsyncDependencies(uint32_t newValue);
  void setInitialTopLevelCapability(Handle<PromiseObject*> capability);
  bool hasTopLevelCapability() const;
  PromiseObject* maybeTopLevelCapability() const;
  PromiseObject* topLevelCapability() const;
  ListObject* asyncParentModules() const;
  mozilla::Maybe<uint32_t> maybePendingAsyncDependencies() const;
  uint32_t pendingAsyncDependencies() const;
  mozilla::Maybe<uint32_t> maybeAsyncEvaluatingPostOrder() const;
  uint32_t getAsyncEvaluatingPostOrder() const;
  void clearAsyncEvaluatingPostOrder();
  void setCycleRoot(ModuleObject* cycleRoot);
  ModuleObject* getCycleRoot() const;
  bool hasCyclicModuleFields() const;
  bool hasSyntheticModuleFields() const;

  static void onTopLevelEvaluationFinished(ModuleObject* module);

  static bool appendAsyncParentModule(JSContext* cx, Handle<ModuleObject*> self,
                                      Handle<ModuleObject*> parent);

  [[nodiscard]] static bool topLevelCapabilityResolve(
      JSContext* cx, Handle<ModuleObject*> module);
  [[nodiscard]] static bool topLevelCapabilityReject(
      JSContext* cx, Handle<ModuleObject*> module, HandleValue error);

  void setMetaObject(JSObject* obj);

  static bool instantiateFunctionDeclarations(JSContext* cx,
                                              Handle<ModuleObject*> self);

  static bool execute(JSContext* cx, Handle<ModuleObject*> self);

  static ModuleNamespaceObject* createNamespace(
      JSContext* cx, Handle<ModuleObject*> self,
      MutableHandle<UniquePtr<ExportNameVector>> exports);

  static bool createEnvironment(JSContext* cx, Handle<ModuleObject*> self);
  static bool createSyntheticEnvironment(JSContext* cx,
                                         Handle<ModuleObject*> self,
                                         JS::HandleVector<Value> values);

  void initAsyncSlots(JSContext* cx, bool hasTopLevelAwait,
                      Handle<ListObject*> asyncParentModules);

 private:
  static const JSClassOps classOps_;

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);

  CyclicModuleFields* cyclicModuleFields();
  const CyclicModuleFields* cyclicModuleFields() const;

  SyntheticModuleFields* syntheticModuleFields();
  const SyntheticModuleFields* syntheticModuleFields() const;
};

JSObject* GetOrCreateModuleMetaObject(JSContext* cx, HandleObject module);

ModuleObject* CallModuleResolveHook(JSContext* cx,
                                    HandleValue referencingPrivate,
                                    HandleObject moduleRequest);

JSObject* StartDynamicModuleImport(JSContext* cx, HandleScript script,
                                   HandleValue specifier, HandleValue options);

bool OnModuleEvaluationFailure(JSContext* cx, HandleObject evaluationPromise,
                               JS::ModuleErrorBehaviour errorBehaviour);

bool FinishDynamicModuleImport(JSContext* cx, HandleObject evaluationPromise,
                               HandleValue referencingPrivate,
                               HandleObject moduleRequest,
                               HandleObject promise);

}  // namespace js

template <>
inline bool JSObject::is<js::ModuleNamespaceObject>() const {
  return js::IsDerivedProxyObject(this,
                                  &js::ModuleNamespaceObject::proxyHandler);
}

#endif /* builtin_ModuleObject_h */
