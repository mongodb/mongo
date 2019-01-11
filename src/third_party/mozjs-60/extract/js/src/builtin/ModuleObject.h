/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_ModuleObject_h
#define builtin_ModuleObject_h

#include "mozilla/Maybe.h"

#include "jsapi.h"

#include "builtin/SelfHostingDefines.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/UniquePtr.h"
#include "vm/JSAtom.h"
#include "vm/NativeObject.h"
#include "vm/ProxyObject.h"

namespace js {

class ModuleEnvironmentObject;
class ModuleObject;

namespace frontend {
class ParseNode;
class TokenStreamAnyChars;
} /* namespace frontend */

typedef Rooted<ModuleObject*> RootedModuleObject;
typedef Handle<ModuleObject*> HandleModuleObject;
typedef Rooted<ModuleEnvironmentObject*> RootedModuleEnvironmentObject;
typedef Handle<ModuleEnvironmentObject*> HandleModuleEnvironmentObject;

class ImportEntryObject : public NativeObject
{
  public:
    enum
    {
        ModuleRequestSlot = 0,
        ImportNameSlot,
        LocalNameSlot,
        LineNumberSlot,
        ColumnNumberSlot,
        SlotCount
    };

    static const Class class_;
    static JSObject* initClass(JSContext* cx, HandleObject obj);
    static bool isInstance(HandleValue value);
    static ImportEntryObject* create(JSContext* cx,
                                     HandleAtom moduleRequest,
                                     HandleAtom importName,
                                     HandleAtom localName,
                                     uint32_t lineNumber,
                                     uint32_t columnNumber);
    JSAtom* moduleRequest() const;
    JSAtom* importName() const;
    JSAtom* localName() const;
    uint32_t lineNumber() const;
    uint32_t columnNumber() const;
};

typedef Rooted<ImportEntryObject*> RootedImportEntryObject;
typedef Handle<ImportEntryObject*> HandleImportEntryObject;

class ExportEntryObject : public NativeObject
{
  public:
    enum
    {
        ExportNameSlot = 0,
        ModuleRequestSlot,
        ImportNameSlot,
        LocalNameSlot,
        LineNumberSlot,
        ColumnNumberSlot,
        SlotCount
    };

    static const Class class_;
    static JSObject* initClass(JSContext* cx, HandleObject obj);
    static bool isInstance(HandleValue value);
    static ExportEntryObject* create(JSContext* cx,
                                     HandleAtom maybeExportName,
                                     HandleAtom maybeModuleRequest,
                                     HandleAtom maybeImportName,
                                     HandleAtom maybeLocalName,
                                     uint32_t lineNumber,
                                     uint32_t columnNumber);
    JSAtom* exportName() const;
    JSAtom* moduleRequest() const;
    JSAtom* importName() const;
    JSAtom* localName() const;
    uint32_t lineNumber() const;
    uint32_t columnNumber() const;
};

typedef Rooted<ExportEntryObject*> RootedExportEntryObject;
typedef Handle<ExportEntryObject*> HandleExportEntryObject;

class RequestedModuleObject : public NativeObject
{
  public:
    enum
    {
        ModuleSpecifierSlot = 0,
        LineNumberSlot,
        ColumnNumberSlot,
        SlotCount
    };

    static const Class class_;
    static JSObject* initClass(JSContext* cx, HandleObject obj);
    static bool isInstance(HandleValue value);
    static RequestedModuleObject* create(JSContext* cx,
                                         HandleAtom moduleSpecifier,
                                         uint32_t lineNumber,
                                         uint32_t columnNumber);
    JSAtom* moduleSpecifier() const;
    uint32_t lineNumber() const;
    uint32_t columnNumber() const;
};

typedef Rooted<RequestedModuleObject*> RootedRequestedModuleObject;
typedef Handle<RequestedModuleObject*> HandleRequestedModuleObject;

class IndirectBindingMap
{
  public:
    void trace(JSTracer* trc);

    bool put(JSContext* cx, HandleId name,
             HandleModuleEnvironmentObject environment, HandleId localName);

    size_t count() const {
        return map_ ? map_->count() : 0;
    }

    bool has(jsid name) const {
        return map_ ? map_->has(name) : false;
    }

    bool lookup(jsid name, ModuleEnvironmentObject** envOut, Shape** shapeOut) const;

    template <typename Func>
    void forEachExportedName(Func func) const {
        if (!map_)
            return;

        for (auto r = map_->all(); !r.empty(); r.popFront())
            func(r.front().key());
    }

  private:
    struct Binding
    {
        Binding(ModuleEnvironmentObject* environment, Shape* shape);
        HeapPtr<ModuleEnvironmentObject*> environment;
        HeapPtr<Shape*> shape;
    };

    typedef HashMap<jsid, Binding, DefaultHasher<jsid>, ZoneAllocPolicy> Map;

    mozilla::Maybe<Map> map_;
};

class ModuleNamespaceObject : public ProxyObject
{
  public:
    enum ModuleNamespaceSlot
    {
        ExportsSlot = 0,
        BindingsSlot
    };

    static bool isInstance(HandleValue value);
    static ModuleNamespaceObject* create(JSContext* cx, HandleModuleObject module,
                                         HandleObject exports,
                                         UniquePtr<IndirectBindingMap> bindings);

    ModuleObject& module();
    JSObject& exports();
    IndirectBindingMap& bindings();

    bool addBinding(JSContext* cx, HandleAtom exportedName, HandleModuleObject targetModule,
                    HandleAtom localName);

  private:
    struct ProxyHandler : public BaseProxyHandler
    {
        ProxyHandler();

        bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                      MutableHandle<PropertyDescriptor> desc) const override;
        bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                            Handle<PropertyDescriptor> desc,
                            ObjectOpResult& result) const override;
        bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                             AutoIdVector& props) const override;
        bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                     ObjectOpResult& result) const override;
        bool getPrototype(JSContext* cx, HandleObject proxy,
                          MutableHandleObject protop) const override;
        bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                          ObjectOpResult& result) const override;
        bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                    MutableHandleObject protop) const override;
        bool setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                   bool* succeeded) const override;

        bool preventExtensions(JSContext* cx, HandleObject proxy,
                               ObjectOpResult& result) const override;
        bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;
        bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override;
        bool get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                 HandleId id, MutableHandleValue vp) const override;
        bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                 HandleValue receiver, ObjectOpResult& result) const override;

        void trace(JSTracer* trc, JSObject* proxy) const override;
        void finalize(JSFreeOp* fop, JSObject* proxy) const override;

        static const char family;
    };

  public:
    static const ProxyHandler proxyHandler;
};

typedef Rooted<ModuleNamespaceObject*> RootedModuleNamespaceObject;
typedef Handle<ModuleNamespaceObject*> HandleModuleNamespaceObject;

struct FunctionDeclaration
{
    FunctionDeclaration(HandleAtom name, HandleFunction fun);
    void trace(JSTracer* trc);

    HeapPtr<JSAtom*> name;
    HeapPtr<JSFunction*> fun;
};

using FunctionDeclarationVector = GCVector<FunctionDeclaration, 0, ZoneAllocPolicy>;

// Possible values for ModuleStatus are defined in SelfHostingDefines.h.
using ModuleStatus = int32_t;

class ModuleObject : public NativeObject
{
  public:
    enum ModuleSlot
    {
        ScriptSlot = 0,
        EnvironmentSlot,
        NamespaceSlot,
        StatusSlot,
        EvaluationErrorSlot,
        HostDefinedSlot,
        RequestedModulesSlot,
        ImportEntriesSlot,
        LocalExportEntriesSlot,
        IndirectExportEntriesSlot,
        StarExportEntriesSlot,
        ImportBindingsSlot,
        FunctionDeclarationsSlot,
        DFSIndexSlot,
        DFSAncestorIndexSlot,
        SlotCount
    };

    static_assert(EnvironmentSlot == MODULE_OBJECT_ENVIRONMENT_SLOT,
                  "EnvironmentSlot must match self-hosting define");
    static_assert(StatusSlot == MODULE_OBJECT_STATUS_SLOT,
                  "StatusSlot must match self-hosting define");
    static_assert(EvaluationErrorSlot == MODULE_OBJECT_EVALUATION_ERROR_SLOT,
                  "EvaluationErrorSlot must match self-hosting define");
    static_assert(DFSIndexSlot == MODULE_OBJECT_DFS_INDEX_SLOT,
                  "DFSIndexSlot must match self-hosting define");
    static_assert(DFSAncestorIndexSlot == MODULE_OBJECT_DFS_ANCESTOR_INDEX_SLOT,
                  "DFSAncestorIndexSlot must match self-hosting define");

    static const Class class_;

    static bool isInstance(HandleValue value);

    static ModuleObject* create(JSContext* cx);
    void init(HandleScript script);
    void setInitialEnvironment(Handle<ModuleEnvironmentObject*> initialEnvironment);
    void initImportExportData(HandleArrayObject requestedModules,
                              HandleArrayObject importEntries,
                              HandleArrayObject localExportEntries,
                              HandleArrayObject indiretExportEntries,
                              HandleArrayObject starExportEntries);
    static bool Freeze(JSContext* cx, HandleModuleObject self);
#ifdef DEBUG
    static bool AssertFrozen(JSContext* cx, HandleModuleObject self);
#endif
    void fixEnvironmentsAfterCompartmentMerge();

    JSScript* script() const;
    Scope* enclosingScope() const;
    ModuleEnvironmentObject& initialEnvironment() const;
    ModuleEnvironmentObject* environment() const;
    ModuleNamespaceObject* namespace_();
    ModuleStatus status() const;
    bool hadEvaluationError() const;
    Value evaluationError() const;
    Value hostDefinedField() const;
    ArrayObject& requestedModules() const;
    ArrayObject& importEntries() const;
    ArrayObject& localExportEntries() const;
    ArrayObject& indirectExportEntries() const;
    ArrayObject& starExportEntries() const;
    IndirectBindingMap& importBindings();

    static bool Instantiate(JSContext* cx, HandleModuleObject self);
    static bool Evaluate(JSContext* cx, HandleModuleObject self);

    void setHostDefinedField(const JS::Value& value);

    // For BytecodeEmitter.
    bool noteFunctionDeclaration(JSContext* cx, HandleAtom name, HandleFunction fun);

    // For intrinsic_InstantiateModuleFunctionDeclarations.
    static bool instantiateFunctionDeclarations(JSContext* cx, HandleModuleObject self);

    // For intrinsic_ExecuteModule.
    static bool execute(JSContext* cx, HandleModuleObject self, MutableHandleValue rval);

    // For intrinsic_NewModuleNamespace.
    static ModuleNamespaceObject* createNamespace(JSContext* cx, HandleModuleObject self,
                                                  HandleObject exports);

  private:
    static const ClassOps classOps_;

    static void trace(JSTracer* trc, JSObject* obj);
    static void finalize(js::FreeOp* fop, JSObject* obj);

    bool hasScript() const;
    bool hasImportBindings() const;
    FunctionDeclarationVector* functionDeclarations();
};

// Process a module's parse tree to collate the import and export data used when
// creating a ModuleObject.
class MOZ_STACK_CLASS ModuleBuilder
{
  public:
    explicit ModuleBuilder(JSContext* cx, HandleModuleObject module,
                           const frontend::TokenStreamAnyChars& tokenStream);
    bool init();

    bool processImport(frontend::ParseNode* pn);
    bool processExport(frontend::ParseNode* pn);
    bool processExportFrom(frontend::ParseNode* pn);

    bool hasExportedName(JSAtom* name) const;

    using ExportEntryVector = GCVector<ExportEntryObject*>;
    const ExportEntryVector& localExportEntries() const {
        return localExportEntries_;
    }

    bool buildTables();
    bool initModule();

  private:
    using AtomVector = GCVector<JSAtom*>;
    using ImportEntryVector = GCVector<ImportEntryObject*>;
    using RequestedModuleVector = GCVector<RequestedModuleObject*>;
    using AtomSet = JS::GCHashSet<JSAtom*>;
    using RootedAtomVector = JS::Rooted<AtomVector>;
    using RootedImportEntryVector = JS::Rooted<ImportEntryVector>;
    using RootedExportEntryVector = JS::Rooted<ExportEntryVector>;
    using RootedRequestedModuleVector = JS::Rooted<RequestedModuleVector>;
    using RootedAtomSet = JS::Rooted<AtomSet>;

    JSContext* cx_;
    RootedModuleObject module_;
    const frontend::TokenStreamAnyChars& tokenStream_;
    RootedAtomSet requestedModuleSpecifiers_;
    RootedRequestedModuleVector requestedModules_;
    RootedAtomVector importedBoundNames_;
    RootedImportEntryVector importEntries_;
    RootedExportEntryVector exportEntries_;
    RootedExportEntryVector localExportEntries_;
    RootedExportEntryVector indirectExportEntries_;
    RootedExportEntryVector starExportEntries_;

    ImportEntryObject* importEntryFor(JSAtom* localName) const;

    bool processExportBinding(frontend::ParseNode* pn);
    bool processExportArrayBinding(frontend::ParseNode* pn);
    bool processExportObjectBinding(frontend::ParseNode* pn);

    bool appendExportEntry(HandleAtom exportName, HandleAtom localName,
                           frontend::ParseNode* node = nullptr);
    bool appendExportFromEntry(HandleAtom exportName, HandleAtom moduleRequest,
                               HandleAtom importName, frontend::ParseNode* node);

    bool maybeAppendRequestedModule(HandleAtom specifier, frontend::ParseNode* node);

    template <typename T>
    ArrayObject* createArray(const JS::Rooted<GCVector<T>>& vector);
};

} // namespace js

template<>
inline bool
JSObject::is<js::ModuleNamespaceObject>() const
{
    return js::IsDerivedProxyObject(this, &js::ModuleNamespaceObject::proxyHandler);
}

#endif /* builtin_ModuleObject_h */
