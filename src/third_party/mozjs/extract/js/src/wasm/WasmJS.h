/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#include "mozilla/HashTable.h"  // DefaultHasher
#include "mozilla/Maybe.h"      // mozilla::Maybe

#include <stdint.h>  // int32_t, int64_t, uint32_t

#include "gc/Barrier.h"        // HeapPtr
#include "gc/ZoneAllocator.h"  // ZoneAllocPolicy
#include "js/AllocPolicy.h"    // SystemAllocPolicy
#include "js/Class.h"          // JSClassOps, ClassSpec
#include "js/GCHashTable.h"    // GCHashMap, GCHashSet
#include "js/GCVector.h"       // GCVector
#include "js/PropertySpec.h"   // JSPropertySpec, JSFunctionSpec
#include "js/RootingAPI.h"     // StableCellHasher
#include "js/SweepingAPI.h"    // JS::WeakCache
#include "js/TypeDecls.h"  // HandleValue, HandleObject, MutableHandleObject, MutableHandleFunction
#include "js/Vector.h"  // JS::Vector
#include "js/WasmFeatures.h"
#include "vm/JSFunction.h"    // JSFunction
#include "vm/NativeObject.h"  // NativeObject
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmException.h"
#include "wasm/WasmExprType.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

// WASM testing is completely disabled using the defined value below.
// You need to change this value and re-compile the code-base to re-enable WASM testing.
#define DISABLE_WASM_TESTING 1

#ifdef DISABLE_WASM_TESTING
#  define WASM_HAS_SUPPORT(cx) false
#else
#  define WASM_HAS_SUPPORT(cx) wasm::HasSupport(cx)
#endif

class JSObject;
class JSTracer;
struct JSContext;

namespace JS {
class CallArgs;
class Value;
}  // namespace JS

namespace js {

class ArrayBufferObject;
class ArrayBufferObjectMaybeShared;
class JSStringBuilder;
class TypedArrayObject;
class WasmFunctionScope;
class WasmInstanceScope;
class WasmSharedArrayRawBuffer;

namespace wasm {

struct ImportValues;

// Compiles the given binary wasm module given the ArrayBufferObject
// and links the module's imports with the given import object.

[[nodiscard]] bool Eval(JSContext* cx, Handle<TypedArrayObject*> code,
                        HandleObject importObj,
                        MutableHandle<WasmInstanceObject*> instanceObj);

// Extracts the various imports from the given import object into the given
// ImportValues structure while checking the imports against the given module.
// The resulting structure can be passed to WasmModule::instantiate.

struct ImportValues;
[[nodiscard]] bool GetImports(JSContext* cx, const Module& module,
                              HandleObject importObj, ImportValues* imports);

// For testing cross-process (de)serialization, this pair of functions are
// responsible for, in the child process, compiling the given wasm bytecode
// to a wasm::Module that is serialized into the given byte array, and, in
// the parent process, deserializing the given byte array into a
// WebAssembly.Module object.

[[nodiscard]] bool CompileAndSerialize(JSContext* cx,
                                       const ShareableBytes& bytecode,
                                       Bytes* serialized);

[[nodiscard]] bool DeserializeModule(JSContext* cx, const Bytes& serialized,
                                     MutableHandleObject module);

// A WebAssembly "Exported Function" is the spec name for the JS function
// objects created to wrap wasm functions. This predicate returns false
// for asm.js functions which are semantically just normal JS functions
// (even if they are implemented via wasm under the hood). The accessor
// functions for extracting the instance and func-index of a wasm function
// can be used for both wasm and asm.js, however.

bool IsWasmExportedFunction(JSFunction* fun);

Instance& ExportedFunctionToInstance(JSFunction* fun);
WasmInstanceObject* ExportedFunctionToInstanceObject(JSFunction* fun);
uint32_t ExportedFunctionToFuncIndex(JSFunction* fun);

bool IsSharedWasmMemoryObject(JSObject* obj);

}  // namespace wasm

// The class of WebAssembly.Module. Each WasmModuleObject owns a
// wasm::Module. These objects are used both as content-facing JS objects and as
// internal implementation details of asm.js.

class WasmModuleObject : public NativeObject {
  static const unsigned MODULE_SLOT = 0;
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static bool imports(JSContext* cx, unsigned argc, Value* vp);
  static bool exports(JSContext* cx, unsigned argc, Value* vp);
  static bool customSections(JSContext* cx, unsigned argc, Value* vp);

 public:
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmModuleObject* create(JSContext* cx, const wasm::Module& module,
                                  HandleObject proto);
  const wasm::Module& module() const;
};

// The class of WebAssembly.Global.  This wraps a storage location, and there is
// a per-agent one-to-one relationship between the WasmGlobalObject and the
// storage location (the Cell) it wraps: if a module re-exports an imported
// global, the imported and exported WasmGlobalObjects are the same, and if a
// module exports a global twice, the two exported WasmGlobalObjects are the
// same.

class WasmGlobalObject : public NativeObject {
  static const unsigned MUTABLE_SLOT = 0;
  static const unsigned VAL_SLOT = 1;

  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static void trace(JSTracer* trc, JSObject* obj);

  static bool typeImpl(JSContext* cx, const CallArgs& args);
  static bool type(JSContext* cx, unsigned argc, Value* vp);

  static bool valueGetterImpl(JSContext* cx, const CallArgs& args);
  static bool valueGetter(JSContext* cx, unsigned argc, Value* vp);
  static bool valueSetterImpl(JSContext* cx, const CallArgs& args);
  static bool valueSetter(JSContext* cx, unsigned argc, Value* vp);

  wasm::GCPtrVal& mutableVal();

 public:
  static const unsigned RESERVED_SLOTS = 2;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmGlobalObject* create(JSContext* cx, wasm::HandleVal value,
                                  bool isMutable, HandleObject proto);
  bool isNewborn() { return getReservedSlot(VAL_SLOT).isUndefined(); }

  bool isMutable() const;
  wasm::ValType type() const;
  const wasm::GCPtrVal& val() const;
  void setVal(wasm::HandleVal value);
  void* addressOfCell() const;
};

// The class of WebAssembly.Instance. Each WasmInstanceObject owns a
// wasm::Instance. These objects are used both as content-facing JS objects and
// as internal implementation details of asm.js.

class WasmInstanceObject : public NativeObject {
  static const unsigned INSTANCE_SLOT = 0;
  static const unsigned EXPORTS_OBJ_SLOT = 1;
  static const unsigned EXPORTS_SLOT = 2;
  static const unsigned SCOPES_SLOT = 3;
  static const unsigned INSTANCE_SCOPE_SLOT = 4;
  static const unsigned GLOBALS_SLOT = 5;

  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static bool exportsGetterImpl(JSContext* cx, const CallArgs& args);
  static bool exportsGetter(JSContext* cx, unsigned argc, Value* vp);
  bool isNewborn() const;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static void trace(JSTracer* trc, JSObject* obj);

  // ExportMap maps from function index to exported function object.
  // This allows the instance to lazily create exported function
  // objects on demand (instead up-front for all table elements) while
  // correctly preserving observable function object identity.
  using ExportMap = GCHashMap<uint32_t, HeapPtr<JSFunction*>,
                              DefaultHasher<uint32_t>, CellAllocPolicy>;
  ExportMap& exports() const;

  // See the definition inside WasmJS.cpp.
  class UnspecifiedScopeMap;
  UnspecifiedScopeMap& scopes() const;

 public:
  static const unsigned RESERVED_SLOTS = 6;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmInstanceObject* create(
      JSContext* cx, const RefPtr<const wasm::Code>& code,
      const wasm::DataSegmentVector& dataSegments,
      const wasm::ModuleElemSegmentVector& elemSegments,
      uint32_t instanceDataLength, Handle<WasmMemoryObjectVector> memories,
      Vector<RefPtr<wasm::Table>, 0, SystemAllocPolicy>&& tables,
      const JSObjectVector& funcImports, const wasm::GlobalDescVector& globals,
      const wasm::ValVector& globalImportValues,
      const WasmGlobalObjectVector& globalObjs,
      const WasmTagObjectVector& tagObjs, HandleObject proto,
      UniquePtr<wasm::DebugState> maybeDebug);
  void initExportsObj(JSObject& exportsObj);

  wasm::Instance& instance() const;
  JSObject& exportsObj() const;

  [[nodiscard]] static bool getExportedFunction(
      JSContext* cx, Handle<WasmInstanceObject*> instanceObj,
      uint32_t funcIndex, MutableHandleFunction fun);

  const wasm::CodeRange& getExportedFunctionCodeRange(JSFunction* fun,
                                                      wasm::Tier tier);

  static WasmInstanceScope* getScope(JSContext* cx,
                                     Handle<WasmInstanceObject*> instanceObj);
  static WasmFunctionScope* getFunctionScope(
      JSContext* cx, Handle<WasmInstanceObject*> instanceObj,
      uint32_t funcIndex);

  using GlobalObjectVector =
      GCVector<HeapPtr<WasmGlobalObject*>, 0, CellAllocPolicy>;
  GlobalObjectVector& indirectGlobals() const;
};

// The class of WebAssembly.Memory. A WasmMemoryObject references an ArrayBuffer
// or SharedArrayBuffer object which owns the actual memory.

class WasmMemoryObject : public NativeObject {
  static const unsigned BUFFER_SLOT = 0;
  static const unsigned OBSERVERS_SLOT = 1;
  static const unsigned ISHUGE_SLOT = 2;
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static bool bufferGetterImpl(JSContext* cx, const CallArgs& args);
  static bool bufferGetter(JSContext* cx, unsigned argc, Value* vp);
  static bool typeImpl(JSContext* cx, const CallArgs& args);
  static bool type(JSContext* cx, unsigned argc, Value* vp);
  static bool growImpl(JSContext* cx, const CallArgs& args);
  static bool grow(JSContext* cx, unsigned argc, Value* vp);
  static bool discardImpl(JSContext* cx, const CallArgs& args);
  static bool discard(JSContext* cx, unsigned argc, Value* vp);
  static uint64_t growShared(Handle<WasmMemoryObject*> memory, uint64_t delta);

  using InstanceSet = JS::WeakCache<GCHashSet<
      WeakHeapPtr<WasmInstanceObject*>,
      StableCellHasher<WeakHeapPtr<WasmInstanceObject*>>, CellAllocPolicy>>;
  bool hasObservers() const;
  InstanceSet& observers() const;
  InstanceSet* getOrCreateObservers(JSContext* cx);

 public:
  static const unsigned RESERVED_SLOTS = 3;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec memoryControlMethods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmMemoryObject* create(JSContext* cx,
                                  Handle<ArrayBufferObjectMaybeShared*> buffer,
                                  bool isHuge, HandleObject proto);

  // `buffer()` returns the current buffer object always.  If the buffer
  // represents shared memory then `buffer().byteLength()` never changes, and
  // in particular it may be a smaller value than that returned from
  // `volatileMemoryLength()` below.
  //
  // Generally, you do not want to call `buffer().byteLength()`, but to call
  // `volatileMemoryLength()`, instead.
  ArrayBufferObjectMaybeShared& buffer() const;

  // The current length of the memory in bytes. In the case of shared memory,
  // the length can change at any time.  Also note that this will acquire a lock
  // for shared memory, so do not call this from a signal handler.
  size_t volatileMemoryLength() const;

  // The current length of the memory in pages. See the comment for
  // `volatileMemoryLength` for details on why this is 'volatile'.
  wasm::Pages volatilePages() const;

  // The maximum length of the memory in pages. This is not 'volatile' in
  // contrast to the current length, as it cannot change for shared memories.
  wasm::Pages clampedMaxPages() const;
  mozilla::Maybe<wasm::Pages> sourceMaxPages() const;

  wasm::IndexType indexType() const;
  bool isShared() const;
  bool isHuge() const;
  bool movingGrowable() const;
  size_t boundsCheckLimit() const;

  // If isShared() is true then obtain the underlying buffer object.
  WasmSharedArrayRawBuffer* sharedArrayRawBuffer() const;

  bool addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance);
  static uint64_t grow(Handle<WasmMemoryObject*> memory, uint64_t delta,
                       JSContext* cx);
  static void discard(Handle<WasmMemoryObject*> memory, uint64_t byteOffset,
                      uint64_t len, JSContext* cx);
};

// The class of WebAssembly.Table. A WasmTableObject holds a refcount on a
// wasm::Table, allowing a Table to be shared between multiple Instances
// (eventually between multiple threads).

class WasmTableObject : public NativeObject {
  static const unsigned TABLE_SLOT = 0;
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  bool isNewborn() const;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static void trace(JSTracer* trc, JSObject* obj);
  static bool lengthGetterImpl(JSContext* cx, const CallArgs& args);
  static bool lengthGetter(JSContext* cx, unsigned argc, Value* vp);
  static bool typeImpl(JSContext* cx, const CallArgs& args);
  static bool type(JSContext* cx, unsigned argc, Value* vp);
  static bool getImpl(JSContext* cx, const CallArgs& args);
  static bool get(JSContext* cx, unsigned argc, Value* vp);
  static bool setImpl(JSContext* cx, const CallArgs& args);
  static bool set(JSContext* cx, unsigned argc, Value* vp);
  static bool growImpl(JSContext* cx, const CallArgs& args);
  static bool grow(JSContext* cx, unsigned argc, Value* vp);

 public:
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  // Note that, after creation, a WasmTableObject's table() is not initialized
  // and must be initialized before use.

  static WasmTableObject* create(JSContext* cx, uint32_t initialLength,
                                 mozilla::Maybe<uint32_t> maximumLength,
                                 wasm::RefType tableType, HandleObject proto);
  wasm::Table& table() const;

  // Perform the standard `ToWebAssemblyValue` coercion on `value` and fill the
  // range [index, index + length) in the table. Callers are required to ensure
  // the range is within bounds. Returns false if the coercion failed.
  bool fillRange(JSContext* cx, uint32_t index, uint32_t length,
                 HandleValue value) const;
};

// The class of WebAssembly.Tag. This class is used to track exception tag
// types for exports and imports.

class WasmTagObject : public NativeObject {
  static const unsigned TYPE_SLOT = 0;

  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static bool typeImpl(JSContext* cx, const CallArgs& args);
  static bool type(JSContext* cx, unsigned argc, Value* vp);

 public:
  static const unsigned RESERVED_SLOTS = 1;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmTagObject* create(JSContext* cx,
                               const wasm::SharedTagType& tagType,
                               HandleObject proto);

  const wasm::TagType* tagType() const;
  const wasm::ValTypeVector& valueTypes() const;
  wasm::ResultType resultType() const;
};

// The class of WebAssembly.Exception. This class is used for
// representing exceptions thrown from Wasm in JS. (it is also used as
// the internal representation for exceptions in Wasm)

class WasmExceptionObject : public NativeObject {
  static const unsigned TAG_SLOT = 0;
  static const unsigned TYPE_SLOT = 1;
  static const unsigned DATA_SLOT = 2;
  static const unsigned STACK_SLOT = 3;

  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  // Named isMethod instead of is to avoid name conflict.
  static bool isMethod(JSContext* cx, unsigned argc, Value* vp);
  static bool isImpl(JSContext* cx, const CallArgs& args);
  static bool getArg(JSContext* cx, unsigned argc, Value* vp);
  static bool getArgImpl(JSContext* cx, const CallArgs& args);
  static bool getStack(JSContext* cx, unsigned argc, Value* vp);
  static bool getStack_impl(JSContext* cx, const CallArgs& args);

  uint8_t* typedMem() const;
  [[nodiscard]] bool loadArg(JSContext* cx, size_t offset, wasm::ValType type,
                             MutableHandleValue vp) const;
  [[nodiscard]] bool initArg(JSContext* cx, size_t offset, wasm::ValType type,
                             HandleValue value);

  void initRefArg(size_t offset, wasm::AnyRef ref);
  wasm::AnyRef loadRefArg(size_t offset) const;

 public:
  static const unsigned RESERVED_SLOTS = 4;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSFunctionSpec static_methods[];
  static bool construct(JSContext*, unsigned, Value*);

  static WasmExceptionObject* create(JSContext* cx, Handle<WasmTagObject*> tag,
                                     HandleObject stack, HandleObject proto);
  static WasmExceptionObject* wrapJSValue(JSContext* cx, HandleValue value);
  bool isNewborn() const;

  JSObject* stack() const;
  const wasm::TagType* tagType() const;
  WasmTagObject& tag() const;

  bool isWrappedJSValue() const;
  Value wrappedJSValue() const;

  static size_t offsetOfData() {
    return NativeObject::getFixedSlotOffset(DATA_SLOT);
  }
};

// The class of the WebAssembly global namespace object.

class WasmNamespaceObject : public NativeObject {
 public:
  static const JSClass class_;
  static const unsigned JS_VALUE_TAG_SLOT = 0;
  static const unsigned RESERVED_SLOTS = 1;

  WasmTagObject* wrappedJSValueTag() const {
    return &getReservedSlot(JS_VALUE_TAG_SLOT)
                .toObjectOrNull()
                ->as<WasmTagObject>();
  }
  void setWrappedJSValueTag(WasmTagObject* tag) {
    return setReservedSlot(JS_VALUE_TAG_SLOT, ObjectValue(*tag));
  }

  static WasmNamespaceObject* getOrCreate(JSContext* cx);

 private:
  static const ClassSpec classSpec_;
};

extern const JSClass WasmFunctionClass;

bool IsWasmSuspendingObject(JSObject* obj);

#ifdef ENABLE_WASM_JSPI

class WasmSuspendingObject : public NativeObject {
 public:
  static const ClassSpec classSpec_;
  static const JSClass class_;
  static const JSClass& protoClass_;
  static const unsigned WRAPPED_FN_SLOT = 0;
  static const unsigned RESERVED_SLOTS = 1;
  static bool construct(JSContext*, unsigned, Value*);

  JSObject* wrappedFunction() const {
    return getReservedSlot(WRAPPED_FN_SLOT).toObjectOrNull();
  }
  void setWrappedFunction(HandleObject fn) {
    return setReservedSlot(WRAPPED_FN_SLOT, ObjectValue(*fn));
  }
};

JSObject* MaybeUnwrapSuspendingObject(JSObject* wrapper);
#endif

}  // namespace js

#endif  // wasm_js_h
