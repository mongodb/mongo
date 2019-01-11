/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
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

#ifndef wasm_module_h
#define wasm_module_h

#include "js/TypeDecls.h"
#include "threading/ConditionVariable.h"
#include "threading/Mutex.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmTable.h"
#include "wasm/WasmValidate.h"

namespace js {
namespace wasm {

struct CompileArgs;

// LinkData contains all the metadata necessary to patch all the locations
// that depend on the absolute address of a ModuleSegment.
//
// LinkData is built incrementally by ModuleGenerator and then stored immutably
// in Module. LinkData is distinct from Metadata in that LinkData is owned and
// destroyed by the Module since it is not needed after instantiation; Metadata
// is needed at runtime.

struct LinkDataTierCacheablePod
{
    uint32_t interruptOffset;
    uint32_t outOfBoundsOffset;
    uint32_t unalignedAccessOffset;
    uint32_t trapOffset;

    LinkDataTierCacheablePod() { mozilla::PodZero(this); }
};

struct LinkDataTier : LinkDataTierCacheablePod
{
    const Tier tier;

    explicit LinkDataTier(Tier tier) : tier(tier) {}

    LinkDataTierCacheablePod& pod() { return *this; }
    const LinkDataTierCacheablePod& pod() const { return *this; }

    struct InternalLink {
        uint32_t patchAtOffset;
        uint32_t targetOffset;
#ifdef JS_CODELABEL_LINKMODE
        uint32_t mode;
#endif
    };
    typedef Vector<InternalLink, 0, SystemAllocPolicy> InternalLinkVector;

    struct SymbolicLinkArray : EnumeratedArray<SymbolicAddress, SymbolicAddress::Limit, Uint32Vector> {
        WASM_DECLARE_SERIALIZABLE(SymbolicLinkArray)
    };

    InternalLinkVector  internalLinks;
    SymbolicLinkArray   symbolicLinks;

    WASM_DECLARE_SERIALIZABLE(LinkData)
};

typedef UniquePtr<LinkDataTier> UniqueLinkDataTier;

class LinkData
{
    UniqueLinkDataTier         linkData1_; // Always present
    mutable UniqueLinkDataTier linkData2_; // Access only if hasTier2() is true

  public:
    LinkData() {}
    explicit LinkData(UniqueLinkDataTier linkData) : linkData1_(Move(linkData)) {}

    void setTier2(UniqueLinkDataTier linkData) const;
    const LinkDataTier& linkData(Tier tier) const;

    WASM_DECLARE_SERIALIZABLE(LinkData)
};

// Contains the locked tiering state of a Module: whether there is an active
// background tier-2 compilation in progress and, if so, the list of listeners
// waiting for the tier-2 compilation to complete.

struct Tiering
{
    typedef Vector<RefPtr<JS::WasmModuleListener>, 0, SystemAllocPolicy> ListenerVector;

    Tiering() : active(false) {}
    ~Tiering() { MOZ_ASSERT(listeners.empty()); MOZ_ASSERT(!active); }

    ListenerVector listeners;
    bool active;
};

typedef ExclusiveWaitableData<Tiering> ExclusiveTiering;

// Module represents a compiled wasm module and primarily provides two
// operations: instantiation and serialization. A Module can be instantiated any
// number of times to produce new Instance objects. A Module can be serialized
// any number of times such that the serialized bytes can be deserialized later
// to produce a new, equivalent Module.
//
// Fully linked-and-instantiated code (represented by Code and its owned
// ModuleSegment) can be shared between instances, provided none of those
// instances are being debugged. If patchable code is needed then each instance
// must have its own Code. Module eagerly creates a new Code and gives it to the
// first instance; it then instantiates new Code objects from a copy of the
// unlinked code that it keeps around for that purpose.

class Module : public JS::WasmModule
{
    const Assumptions       assumptions_;
    const SharedCode        code_;
    const UniqueConstBytes  unlinkedCodeForDebugging_;
    const LinkData          linkData_;
    const ImportVector      imports_;
    const ExportVector      exports_;
    const DataSegmentVector dataSegments_;
    const ElemSegmentVector elemSegments_;
    const SharedBytes       bytecode_;
    ExclusiveTiering        tiering_;

    // `codeIsBusy_` is set to false initially and then to true when `code_` is
    // already being used for an instance and can't be shared because it may be
    // patched by the debugger. Subsequent instances must then create copies
    // by linking the `unlinkedCodeForDebugging_`.

    mutable Atomic<bool>    codeIsBusy_;

    bool instantiateFunctions(JSContext* cx, Handle<FunctionVector> funcImports) const;
    bool instantiateMemory(JSContext* cx, MutableHandleWasmMemoryObject memory) const;
    bool instantiateTable(JSContext* cx,
                          MutableHandleWasmTableObject table,
                          SharedTableVector* tables) const;
    bool initSegments(JSContext* cx,
                      HandleWasmInstanceObject instance,
                      Handle<FunctionVector> funcImports,
                      HandleWasmMemoryObject memory,
                      const ValVector& globalImports) const;

    class Tier2GeneratorTaskImpl;
    void notifyCompilationListeners();

  public:
    Module(Assumptions&& assumptions,
           const Code& code,
           UniqueConstBytes unlinkedCodeForDebugging,
           LinkData&& linkData,
           ImportVector&& imports,
           ExportVector&& exports,
           DataSegmentVector&& dataSegments,
           ElemSegmentVector&& elemSegments,
           const ShareableBytes& bytecode)
      : assumptions_(Move(assumptions)),
        code_(&code),
        unlinkedCodeForDebugging_(Move(unlinkedCodeForDebugging)),
        linkData_(Move(linkData)),
        imports_(Move(imports)),
        exports_(Move(exports)),
        dataSegments_(Move(dataSegments)),
        elemSegments_(Move(elemSegments)),
        bytecode_(&bytecode),
        tiering_(mutexid::WasmModuleTieringLock),
        codeIsBusy_(false)
    {
        MOZ_ASSERT_IF(metadata().debugEnabled, unlinkedCodeForDebugging_);
    }
    ~Module() override { /* Note: can be called on any thread */ }

    const Code& code() const { return *code_; }
    const ModuleSegment& moduleSegment(Tier t) const { return code_->segment(t); }
    const Metadata& metadata() const { return code_->metadata(); }
    const MetadataTier& metadata(Tier t) const { return code_->metadata(t); }
    const LinkData& linkData() const { return linkData_; }
    const LinkDataTier& linkData(Tier t) const { return linkData_.linkData(t); }
    const ImportVector& imports() const { return imports_; }
    const ExportVector& exports() const { return exports_; }
    const ShareableBytes& bytecode() const { return *bytecode_; }
    uint32_t codeLength(Tier t) const { return code_->segment(t).length(); }

    // Instantiate this module with the given imports:

    bool instantiate(JSContext* cx,
                     Handle<FunctionVector> funcImports,
                     HandleWasmTableObject tableImport,
                     HandleWasmMemoryObject memoryImport,
                     const ValVector& globalImports,
                     HandleObject instanceProto,
                     MutableHandleWasmInstanceObject instanceObj) const;

    // Tier-2 compilation may be initiated after the Module is constructed at
    // most once, ideally before any client can attempt to serialize the Module.
    // When tier-2 compilation completes, ModuleGenerator calls finishTier2()
    // from a helper thread, passing tier-variant data which will be installed
    // and made visible.

    void startTier2(const CompileArgs& args);
    bool finishTier2(UniqueLinkDataTier linkData2, UniqueCodeTier tier2, ModuleEnvironment* env2);
    void blockOnTier2Complete() const;

    // JS API and JS::WasmModule implementation:

    size_t bytecodeSerializedSize() const override;
    void bytecodeSerialize(uint8_t* bytecodeBegin, size_t bytecodeSize) const override;
    bool compilationComplete() const override;
    bool notifyWhenCompilationComplete(JS::WasmModuleListener* listener) override;
    size_t compiledSerializedSize() const override;
    void compiledSerialize(uint8_t* compiledBegin, size_t compiledSize) const override;

    static bool assumptionsMatch(const Assumptions& current, const uint8_t* compiledBegin,
                                 size_t remain);
    static RefPtr<Module> deserialize(const uint8_t* bytecodeBegin, size_t bytecodeSize,
                                      const uint8_t* compiledBegin, size_t compiledSize,
                                      Metadata* maybeMetadata = nullptr);
    JSObject* createObject(JSContext* cx) override;

    // about:memory reporting:

    void addSizeOfMisc(MallocSizeOf mallocSizeOf,
                       Metadata::SeenSet* seenMetadata,
                       ShareableBytes::SeenSet* seenBytes,
                       Code::SeenSet* seenCode,
                       size_t* code, size_t* data) const;

    // Generated code analysis support:

    bool extractCode(JSContext* cx, Tier tier, MutableHandleValue vp) const;
};

typedef RefPtr<Module> SharedModule;

// JS API implementations:

bool
CompiledModuleAssumptionsMatch(PRFileDesc* compiled, JS::BuildIdCharVector&& buildId);

SharedModule
DeserializeModule(PRFileDesc* bytecode, PRFileDesc* maybeCompiled, JS::BuildIdCharVector&& buildId,
                  UniqueChars filename, unsigned line, unsigned column);

} // namespace wasm
} // namespace js

#endif // wasm_module_h
