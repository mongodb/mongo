/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_ModuleObject_h
#define builtin_ModuleObject_h

#include "jsapi.h"
#include "jsatom.h"

#include "gc/Zone.h"

#include "js/TraceableVector.h"

#include "vm/NativeObject.h"
#include "vm/ProxyObject.h"

namespace js {

class ModuleEnvironmentObject;
class ModuleObject;

namespace frontend {
class ParseNode;
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
        SlotCount
    };

    static const Class class_;
    static JSObject* initClass(JSContext* cx, HandleObject obj);
    static bool isInstance(HandleValue value);
    static ImportEntryObject* create(JSContext* cx,
                                     HandleAtom moduleRequest,
                                     HandleAtom importName,
                                     HandleAtom localName);
    JSAtom* moduleRequest();
    JSAtom* importName();
    JSAtom* localName();
};

class ExportEntryObject : public NativeObject
{
  public:
    enum
    {
        ExportNameSlot = 0,
        ModuleRequestSlot,
        ImportNameSlot,
        LocalNameSlot,
        SlotCount
    };

    static const Class class_;
    static JSObject* initClass(JSContext* cx, HandleObject obj);
    static bool isInstance(HandleValue value);
    static ExportEntryObject* create(JSContext* cx,
                                     HandleAtom maybeExportName,
                                     HandleAtom maybeModuleRequest,
                                     HandleAtom maybeImportName,
                                     HandleAtom maybeLocalName);
    JSAtom* exportName();
    JSAtom* moduleRequest();
    JSAtom* importName();
    JSAtom* localName();
};

typedef Rooted<ExportEntryObject*> RootedExportEntryObject;
typedef Handle<ExportEntryObject*> HandleExportEntryObject;

class IndirectBindingMap
{
  public:
    explicit IndirectBindingMap(Zone* zone);
    bool init();

    void trace(JSTracer* trc);

    bool putNew(JSContext* cx, HandleId name,
                HandleModuleEnvironmentObject environment, HandleId localName);

    size_t count() const {
        return map_.count();
    }

    bool has(jsid name) const {
        return map_.has(name);
    }

    bool lookup(jsid name, ModuleEnvironmentObject** envOut, Shape** shapeOut) const;

    template <typename Func>
    void forEachExportedName(Func func) const {
        for (auto r = map_.all(); !r.empty(); r.popFront())
            func(r.front().key());
    }

  private:
    struct Binding
    {
        Binding(ModuleEnvironmentObject* environment, Shape* shape);
        RelocatablePtr<ModuleEnvironmentObject*> environment;
        RelocatablePtrShape shape;
    };

    typedef HashMap<jsid, Binding, JsidHasher, ZoneAllocPolicy> Map;

    Map map_;
};

class ModuleNamespaceObject : public ProxyObject
{
  public:
    static bool isInstance(HandleValue value);
    static ModuleNamespaceObject* create(JSContext* cx, HandleModuleObject module);

    ModuleObject& module();
    ArrayObject& exports();
    IndirectBindingMap& bindings();

    bool addBinding(JSContext* cx, HandleAtom exportedName, HandleModuleObject targetModule,
                    HandleAtom localName);

  private:
    struct ProxyHandler : public BaseProxyHandler
    {
        enum
        {
            EnumerateFunctionSlot = 0
        };

        ProxyHandler();

        JS::Value getEnumerateFunction(HandleObject proxy) const;

        bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                      MutableHandle<JSPropertyDescriptor> desc) const override;
        bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                            Handle<JSPropertyDescriptor> desc,
                            ObjectOpResult& result) const override;
        bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                             AutoIdVector& props) const override;
        bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                     ObjectOpResult& result) const override;
        bool enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp) const override;
        bool getPrototype(JSContext* cx, HandleObject proxy,
                          MutableHandleObject protop) const override;
        bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                          ObjectOpResult& result) const override;
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

    RelocatablePtrAtom name;
    RelocatablePtrFunction fun;
};

using FunctionDeclarationVector = TraceableVector<FunctionDeclaration, 0, ZoneAllocPolicy>;

class ModuleObject : public NativeObject
{
  public:
    enum
    {
        ScriptSlot = 0,
        StaticScopeSlot,
        InitialEnvironmentSlot,
        EnvironmentSlot,
        NamespaceSlot,
        EvaluatedSlot,
        RequestedModulesSlot,
        ImportEntriesSlot,
        LocalExportEntriesSlot,
        IndirectExportEntriesSlot,
        StarExportEntriesSlot,
        ImportBindingsSlot,
        NamespaceExportsSlot,
        NamespaceBindingsSlot,
        FunctionDeclarationsSlot,
        SlotCount
    };

    static const Class class_;

    static bool isInstance(HandleValue value);

    static ModuleObject* create(ExclusiveContext* cx, HandleObject enclosingStaticScope);
    void init(HandleScript script);
    void setInitialEnvironment(Handle<ModuleEnvironmentObject*> initialEnvironment);
    void initImportExportData(HandleArrayObject requestedModules,
                              HandleArrayObject importEntries,
                              HandleArrayObject localExportEntries,
                              HandleArrayObject indiretExportEntries,
                              HandleArrayObject starExportEntries);

    JSScript* script() const;
    JSObject* enclosingStaticScope() const;
    ModuleEnvironmentObject& initialEnvironment() const;
    ModuleEnvironmentObject* environment() const;
    ModuleNamespaceObject* namespace_();
    bool evaluated() const;
    ArrayObject& requestedModules() const;
    ArrayObject& importEntries() const;
    ArrayObject& localExportEntries() const;
    ArrayObject& indirectExportEntries() const;
    ArrayObject& starExportEntries() const;
    IndirectBindingMap& importBindings();
    ArrayObject* namespaceExports();
    IndirectBindingMap* namespaceBindings();

    void createEnvironment();

    bool noteFunctionDeclaration(ExclusiveContext* cx, HandleAtom name, HandleFunction fun);
    static bool instantiateFunctionDeclarations(JSContext* cx, HandleModuleObject self);

    void setEvaluated();
    static bool evaluate(JSContext* cx, HandleModuleObject self, MutableHandleValue rval);

    static ModuleNamespaceObject* createNamespace(JSContext* cx, HandleModuleObject self,
                                                  HandleArrayObject exports);

  private:
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
    explicit ModuleBuilder(JSContext* cx, HandleModuleObject module);

    bool buildAndInit(frontend::ParseNode* pn);

  private:
    using AtomVector = TraceableVector<JSAtom*>;
    using RootedAtomVector = JS::Rooted<AtomVector>;
    using ImportEntryVector = TraceableVector<ImportEntryObject*>;
    using RootedImportEntryVector = JS::Rooted<ImportEntryVector>;
    using ExportEntryVector = TraceableVector<ExportEntryObject*> ;
    using RootedExportEntryVector = JS::Rooted<ExportEntryVector> ;

    JSContext* cx_;
    RootedModuleObject module_;
    RootedAtomVector requestedModules_;
    RootedAtomVector importedBoundNames_;
    RootedImportEntryVector importEntries_;
    RootedExportEntryVector exportEntries_;
    RootedExportEntryVector localExportEntries_;
    RootedExportEntryVector indirectExportEntries_;
    RootedExportEntryVector starExportEntries_;

    bool processImport(frontend::ParseNode* pn);
    bool processExport(frontend::ParseNode* pn);
    bool processExportFrom(frontend::ParseNode* pn);

    ImportEntryObject* importEntryFor(JSAtom* localName);

    bool appendExportEntry(HandleAtom exportName, HandleAtom localName);
    bool appendExportFromEntry(HandleAtom exportName, HandleAtom moduleRequest,
                               HandleAtom importName);

    bool maybeAppendRequestedModule(HandleAtom module);

    bool appendLocalExportEntry(HandleExportEntryObject exp);

    template <typename T>
    ArrayObject* createArray(const TraceableVector<T>& vector);
};

bool InitModuleClasses(JSContext* cx, HandleObject obj);

} // namespace js

template<>
inline bool
JSObject::is<js::ModuleNamespaceObject>() const
{
    return js::IsDerivedProxyObject(this, &js::ModuleNamespaceObject::proxyHandler);
}

#endif /* builtin_ModuleObject_h */
