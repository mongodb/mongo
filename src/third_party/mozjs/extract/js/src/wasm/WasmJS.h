/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_js_h
#define wasm_js_h

#include "gc/Policy.h"
#include "vm/NativeObject.h"
#include "wasm/WasmTypes.h"

namespace js {

class TypedArrayObject;
class WasmFunctionScope;
class WasmInstanceScope;

namespace wasm {

// Return whether WebAssembly can be compiled on this platform.
// This must be checked and must be true to call any of the top-level wasm
// eval/compile methods.

bool
HasCompilerSupport(JSContext* cx);

// Return whether WebAssembly is enabled on this platform.

bool
HasSupport(JSContext* cx);

// ToWebAssemblyValue and ToJSValue are conversion functions defined in
// the Wasm JS API spec.

bool
ToWebAssemblyValue(JSContext* cx, ValType targetType, HandleValue v, Val* val);

void
ToJSValue(const Val& val, MutableHandleValue v);

// Compiles the given binary wasm module given the ArrayBufferObject
// and links the module's imports with the given import object.

MOZ_MUST_USE bool
Eval(JSContext* cx, Handle<TypedArrayObject*> code, HandleObject importObj,
     MutableHandleWasmInstanceObject instanceObj);

// These accessors can be used to probe JS values for being an exported wasm
// function.

extern bool
IsExportedFunction(JSFunction* fun);

extern bool
IsExportedWasmFunction(JSFunction* fun);

extern bool
IsExportedFunction(const Value& v, MutableHandleFunction f);

extern Instance&
ExportedFunctionToInstance(JSFunction* fun);

extern WasmInstanceObject*
ExportedFunctionToInstanceObject(JSFunction* fun);

extern uint32_t
ExportedFunctionToFuncIndex(JSFunction* fun);

extern bool
IsSharedWasmMemoryObject(JSObject* obj);

} // namespace wasm

// The class of the WebAssembly global namespace object.

extern const Class WebAssemblyClass;

JSObject*
InitWebAssemblyClass(JSContext* cx, HandleObject global);

// The class of WebAssembly.Module. Each WasmModuleObject owns a
// wasm::Module. These objects are used both as content-facing JS objects and as
// internal implementation details of asm.js.

class WasmModuleObject : public NativeObject
{
    static const unsigned MODULE_SLOT = 0;
    static const ClassOps classOps_;
    static void finalize(FreeOp* fop, JSObject* obj);
    static bool imports(JSContext* cx, unsigned argc, Value* vp);
    static bool exports(JSContext* cx, unsigned argc, Value* vp);
    static bool customSections(JSContext* cx, unsigned argc, Value* vp);

  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSFunctionSpec static_methods[];
    static bool construct(JSContext*, unsigned, Value*);

    static WasmModuleObject* create(JSContext* cx,
                                    wasm::Module& module,
                                    HandleObject proto = nullptr);
    wasm::Module& module() const;
};

// The class of WebAssembly.Instance. Each WasmInstanceObject owns a
// wasm::Instance. These objects are used both as content-facing JS objects and
// as internal implementation details of asm.js.

class WasmInstanceObject : public NativeObject
{
    static const unsigned INSTANCE_SLOT = 0;
    static const unsigned EXPORTS_OBJ_SLOT = 1;
    static const unsigned EXPORTS_SLOT = 2;
    static const unsigned SCOPES_SLOT = 3;
    static const unsigned INSTANCE_SCOPE_SLOT = 4;

    static const ClassOps classOps_;
    static bool exportsGetterImpl(JSContext* cx, const CallArgs& args);
    static bool exportsGetter(JSContext* cx, unsigned argc, Value* vp);
    bool isNewborn() const;
    static void finalize(FreeOp* fop, JSObject* obj);
    static void trace(JSTracer* trc, JSObject* obj);

    // ExportMap maps from function index to exported function object.
    // This allows the instance to lazily create exported function
    // objects on demand (instead up-front for all table elements) while
    // correctly preserving observable function object identity.
    using ExportMap = GCHashMap<uint32_t,
                                HeapPtr<JSFunction*>,
                                DefaultHasher<uint32_t>,
                                SystemAllocPolicy>;
    ExportMap& exports() const;

    // WeakScopeMap maps from function index to js::Scope. This maps is weak
    // to avoid holding scope objects alive. The scopes are normally created
    // during debugging.
    using ScopeMap = JS::WeakCache<GCHashMap<uint32_t,
                                             ReadBarriered<WasmFunctionScope*>,
                                             DefaultHasher<uint32_t>,
                                             SystemAllocPolicy>>;
    ScopeMap& scopes() const;

  public:
    static const unsigned RESERVED_SLOTS = 5;
    static const Class class_;
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSFunctionSpec static_methods[];
    static bool construct(JSContext*, unsigned, Value*);

    static WasmInstanceObject* create(JSContext* cx,
                                      RefPtr<const wasm::Code> code,
                                      UniquePtr<wasm::DebugState> debug,
                                      wasm::UniqueTlsData tlsData,
                                      HandleWasmMemoryObject memory,
                                      Vector<RefPtr<wasm::Table>, 0, SystemAllocPolicy>&& tables,
                                      Handle<FunctionVector> funcImports,
                                      const wasm::ValVector& globalImports,
                                      HandleObject proto);
    void initExportsObj(JSObject& exportsObj);

    wasm::Instance& instance() const;
    JSObject& exportsObj() const;

    static bool getExportedFunction(JSContext* cx,
                                    HandleWasmInstanceObject instanceObj,
                                    uint32_t funcIndex,
                                    MutableHandleFunction fun);

    const wasm::CodeRange& getExportedFunctionCodeRange(HandleFunction fun, wasm::Tier tier);

    static WasmInstanceScope* getScope(JSContext* cx, HandleWasmInstanceObject instanceObj);
    static WasmFunctionScope* getFunctionScope(JSContext* cx,
                                               HandleWasmInstanceObject instanceObj,
                                               uint32_t funcIndex);
};

// The class of WebAssembly.Memory. A WasmMemoryObject references an ArrayBuffer
// or SharedArrayBuffer object which owns the actual memory.

class WasmMemoryObject : public NativeObject
{
    static const unsigned BUFFER_SLOT = 0;
    static const unsigned OBSERVERS_SLOT = 1;
    static const ClassOps classOps_;
    static void finalize(FreeOp* fop, JSObject* obj);
    static bool bufferGetterImpl(JSContext* cx, const CallArgs& args);
    static bool bufferGetter(JSContext* cx, unsigned argc, Value* vp);
    static bool growImpl(JSContext* cx, const CallArgs& args);
    static bool grow(JSContext* cx, unsigned argc, Value* vp);
    static uint32_t growShared(HandleWasmMemoryObject memory, uint32_t delta);

    using InstanceSet = JS::WeakCache<GCHashSet<ReadBarrieredWasmInstanceObject,
                                                MovableCellHasher<ReadBarrieredWasmInstanceObject>,
                                                SystemAllocPolicy>>;
    bool hasObservers() const;
    InstanceSet& observers() const;
    InstanceSet* getOrCreateObservers(JSContext* cx);

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const Class class_;
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSFunctionSpec static_methods[];
    static bool construct(JSContext*, unsigned, Value*);

    static WasmMemoryObject* create(JSContext* cx,
                                    Handle<ArrayBufferObjectMaybeShared*> buffer,
                                    HandleObject proto);

    // `buffer()` returns the current buffer object always.  If the buffer
    // represents shared memory then `buffer().byteLength()` never changes, and
    // in particular it may be a smaller value than that returned from
    // `volatileMemoryLength()` below.
    //
    // Generally, you do not want to call `buffer().byteLength()`, but to call
    // `volatileMemoryLength()`, instead.
    ArrayBufferObjectMaybeShared& buffer() const;

    // The current length of the memory.  In the case of shared memory, the
    // length can change at any time.  Also note that this will acquire a lock
    // for shared memory, so do not call this from a signal handler.
    uint32_t volatileMemoryLength() const;

    bool isShared() const;
    bool movingGrowable() const;

    // If isShared() is true then obtain the underlying buffer object.
    SharedArrayRawBuffer* sharedArrayRawBuffer() const;

    bool addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance);
    static uint32_t grow(HandleWasmMemoryObject memory, uint32_t delta, JSContext* cx);
};

// The class of WebAssembly.Table. A WasmTableObject holds a refcount on a
// wasm::Table, allowing a Table to be shared between multiple Instances
// (eventually between multiple threads).

class WasmTableObject : public NativeObject
{
    static const unsigned TABLE_SLOT = 0;
    static const ClassOps classOps_;
    bool isNewborn() const;
    static void finalize(FreeOp* fop, JSObject* obj);
    static void trace(JSTracer* trc, JSObject* obj);
    static bool lengthGetterImpl(JSContext* cx, const CallArgs& args);
    static bool lengthGetter(JSContext* cx, unsigned argc, Value* vp);
    static bool getImpl(JSContext* cx, const CallArgs& args);
    static bool get(JSContext* cx, unsigned argc, Value* vp);
    static bool setImpl(JSContext* cx, const CallArgs& args);
    static bool set(JSContext* cx, unsigned argc, Value* vp);
    static bool growImpl(JSContext* cx, const CallArgs& args);
    static bool grow(JSContext* cx, unsigned argc, Value* vp);

  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSFunctionSpec static_methods[];
    static bool construct(JSContext*, unsigned, Value*);

    // Note that, after creation, a WasmTableObject's table() is not initialized
    // and must be initialized before use.

    static WasmTableObject* create(JSContext* cx, const wasm::Limits& limits);
    wasm::Table& table() const;
};

#if defined(ENABLE_WASM_GLOBAL) && defined(EARLY_BETA_OR_EARLIER)

// The class of WebAssembly.Global.  A WasmGlobalObject holds either the value
// of an immutable wasm global or the cell of a mutable wasm global.

class WasmGlobalObject : public NativeObject
{
    static const unsigned TYPE_SLOT = 0;
    static const unsigned MUTABLE_SLOT = 1;
    static const unsigned VALUE_SLOT = 2;

    static const ClassOps classOps_;

    static bool valueGetterImpl(JSContext* cx, const CallArgs& args);
    static bool valueGetter(JSContext* cx, unsigned argc, Value* vp);
    static bool valueSetterImpl(JSContext* cx, const CallArgs& args);
    static bool valueSetter(JSContext* cx, unsigned argc, Value* vp);

  public:
    static const unsigned RESERVED_SLOTS = 3;
    static const Class class_;
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSFunctionSpec static_methods[];
    static bool construct(JSContext*, unsigned, Value*);

    static WasmGlobalObject* create(JSContext* cx, wasm::ValType type, bool isMutable,
                                    HandleValue value);

    wasm::ValType type() const;
    bool isMutable() const;
    Value value() const;
};

#endif // ENABLE_WASM_GLOBAL && EARLY_BETA_OR_EARLIER

} // namespace js

#endif // wasm_js_h
