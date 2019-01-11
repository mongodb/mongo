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

#ifndef wasm_instance_h
#define wasm_instance_h

#include "gc/Barrier.h"
#include "jit/shared/Assembler-shared.h"
#include "vm/SharedMem.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmTable.h"

namespace js {
namespace wasm {

// Instance represents a wasm instance and provides all the support for runtime
// execution of code in the instance. Instances share various immutable data
// structures with the Module from which they were instantiated and other
// instances instantiated from the same Module. However, an Instance has no
// direct reference to its source Module which allows a Module to be destroyed
// while it still has live Instances.
//
// The instance's code may be shared among multiple instances provided none of
// those instances are being debugged. Instances that are being debugged own
// their code.

class Instance
{
    JSCompartment* const            compartment_;
    ReadBarrieredWasmInstanceObject object_;
    jit::TrampolinePtr              jsJitArgsRectifier_;
    jit::TrampolinePtr              jsJitExceptionHandler_;
    const SharedCode                code_;
    const UniqueDebugState          debug_;
    const UniqueTlsData             tlsData_;
    GCPtrWasmMemoryObject           memory_;
    SharedTableVector               tables_;
    bool                            enterFrameTrapsEnabled_;

    // Internal helpers:
    const void** addressOfSigId(const SigIdDesc& sigId) const;
    FuncImportTls& funcImportTls(const FuncImport& fi);
    TableTls& tableTls(const TableDesc& td) const;

    // Only WasmInstanceObject can call the private trace function.
    friend class js::WasmInstanceObject;
    void tracePrivate(JSTracer* trc);

    bool callImport(JSContext* cx, uint32_t funcImportIndex, unsigned argc, const uint64_t* argv,
                    MutableHandleValue rval);

  public:
    Instance(JSContext* cx,
             HandleWasmInstanceObject object,
             SharedCode code,
             UniqueDebugState debug,
             UniqueTlsData tlsData,
             HandleWasmMemoryObject memory,
             SharedTableVector&& tables,
             Handle<FunctionVector> funcImports,
             const ValVector& globalImports);
    ~Instance();
    bool init(JSContext* cx);
    void trace(JSTracer* trc);

    JSCompartment* compartment() const { return compartment_; }
    const Code& code() const { return *code_; }
    const CodeTier& code(Tier t) const { return code_->codeTier(t); }
    DebugState& debug() { return *debug_; }
    const DebugState& debug() const { return *debug_; }
    const ModuleSegment& moduleSegment(Tier t) const { return code_->segment(t); }
    TlsData* tlsData() const { return tlsData_.get(); }
    uint8_t* globalData() const { return (uint8_t*)&tlsData_->globalArea; }
    uint8_t* codeBase(Tier t) const { return code_->segment(t).base(); }
    const MetadataTier& metadata(Tier t) const { return code_->metadata(t); }
    const Metadata& metadata() const { return code_->metadata(); }
    bool isAsmJS() const { return metadata().isAsmJS(); }
    const SharedTableVector& tables() const { return tables_; }
    SharedMem<uint8_t*> memoryBase() const;
    WasmMemoryObject* memory() const;
    size_t memoryMappedSize() const;
    SharedArrayRawBuffer* sharedMemoryBuffer() const; // never null
#ifdef JS_SIMULATOR
    bool memoryAccessInGuardRegion(uint8_t* addr, unsigned numBytes) const;
#endif

    static constexpr size_t offsetOfJSJitArgsRectifier() {
        return offsetof(Instance, jsJitArgsRectifier_);
    }
    static constexpr size_t offsetOfJSJitExceptionHandler() {
        return offsetof(Instance, jsJitExceptionHandler_);
    }

    // This method returns a pointer to the GC object that owns this Instance.
    // Instances may be reached via weak edges (e.g., Compartment::instances_)
    // so this perform a read-barrier on the returned object unless the barrier
    // is explicitly waived.

    WasmInstanceObject* object() const;
    WasmInstanceObject* objectUnbarriered() const;

    // Execute the given export given the JS call arguments, storing the return
    // value in args.rval.

    MOZ_MUST_USE bool callExport(JSContext* cx, uint32_t funcIndex, CallArgs args);

    // Return the name associated with a given function index, or generate one
    // if none was given by the module.

    bool getFuncName(uint32_t funcIndex, UTF8Bytes* name) const;
    JSAtom* getFuncAtom(JSContext* cx, uint32_t funcIndex) const;
    void ensureProfilingLabels(bool profilingEnabled) const;

    // Initially, calls to imports in wasm code call out through the generic
    // callImport method. If the imported callee gets JIT compiled and the types
    // match up, callImport will patch the code to instead call through a thunk
    // directly into the JIT code. If the JIT code is released, the Instance must
    // be notified so it can go back to the generic callImport.

    void deoptimizeImportExit(uint32_t funcImportIndex);

    // Called by Wasm(Memory|Table)Object when a moving resize occurs:

    void onMovingGrowMemory(uint8_t* prevMemoryBase);
    void onMovingGrowTable();

    // Debug support:

    bool debugEnabled() const { return metadata().debugEnabled; }
    bool enterFrameTrapsEnabled() const { return enterFrameTrapsEnabled_; }
    void ensureEnterFrameTrapsState(JSContext* cx, bool enabled);

    // about:memory reporting:

    void addSizeOfMisc(MallocSizeOf mallocSizeOf,
                       Metadata::SeenSet* seenMetadata,
                       ShareableBytes::SeenSet* seenBytes,
                       Code::SeenSet* seenCode,
                       Table::SeenSet* seenTables,
                       size_t* code,
                       size_t* data) const;

  public:
    // Functions to be called directly from wasm code.
    static int32_t callImport_void(Instance*, int32_t, int32_t, uint64_t*);
    static int32_t callImport_i32(Instance*, int32_t, int32_t, uint64_t*);
    static int32_t callImport_i64(Instance*, int32_t, int32_t, uint64_t*);
    static int32_t callImport_f64(Instance*, int32_t, int32_t, uint64_t*);
    static uint32_t growMemory_i32(Instance* instance, uint32_t delta);
    static uint32_t currentMemory_i32(Instance* instance);
    static int32_t wait_i32(Instance* instance, uint32_t byteOffset, int32_t value, int64_t timeout);
    static int32_t wait_i64(Instance* instance, uint32_t byteOffset, int64_t value, int64_t timeout);
    static int32_t wake(Instance* instance, uint32_t byteOffset, int32_t count);
};

typedef UniquePtr<Instance> UniqueInstance;

bool InitInstanceStaticData();
void ShutDownInstanceStaticData();

} // namespace wasm
} // namespace js

#endif // wasm_instance_h
