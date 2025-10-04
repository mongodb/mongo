/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#include "js/WasmModule.h"
#include "js/BuildId.h"

#include "wasm/WasmCode.h"
#include "wasm/WasmException.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTable.h"

namespace JS {
class OptimizedEncodingListener;
}

namespace js {
namespace wasm {

struct CompileArgs;

// In the context of wasm, the OptimizedEncodingListener specifically is
// listening for the completion of tier-2.

using Tier2Listener = RefPtr<JS::OptimizedEncodingListener>;

// A struct containing the typed, imported values that are harvested from the
// import object and passed to Module::instantiate(). This struct must be
// stored in a (Persistent)Rooted, not in the heap due to its use of TraceRoot()
// and complete lack of barriers.

struct ImportValues {
  JSObjectVector funcs;
  WasmTableObjectVector tables;
  WasmMemoryObjectVector memories;
  WasmTagObjectVector tagObjs;
  WasmGlobalObjectVector globalObjs;
  ValVector globalValues;

  ImportValues() {}

  void trace(JSTracer* trc) {
    funcs.trace(trc);
    tables.trace(trc);
    memories.trace(trc);
    tagObjs.trace(trc);
    globalObjs.trace(trc);
    globalValues.trace(trc);
  }
};

// Module represents a compiled wasm module and primarily provides three
// operations: instantiation, tiered compilation, serialization. A Module can be
// instantiated any number of times to produce new Instance objects. A Module
// can have a single tier-2 task initiated to augment a Module's code with a
// higher tier. A Module can have its optimized code serialized at any point
// where the LinkData is also available, which is primarily (1) at the end of
// module generation, (2) at the end of tier-2 compilation.
//
// Fully linked-and-instantiated code (represented by SharedCode and its owned
// ModuleSegment) can be shared between instances.

class Module : public JS::WasmModule {
  const SharedCode code_;
  const ImportVector imports_;
  const ExportVector exports_;
  const DataSegmentVector dataSegments_;
  const ModuleElemSegmentVector elemSegments_;
  const CustomSectionVector customSections_;

  // This field is only meaningful when code_->metadata().debugEnabled.

  const SharedBytes debugBytecode_;

  // This field is set during tier-2 compilation and cleared on success or
  // failure. These happen on different threads and are serialized by the
  // control flow of helper tasks.

  mutable Tier2Listener tier2Listener_;

  // This flag is used for logging (and testing) purposes to indicate
  // whether the module was deserialized (from a cache).

  const bool loggingDeserialized_;

  // This flag is only used for testing purposes and is cleared on success or
  // failure. The field is racily polled from various threads.

  mutable Atomic<bool> testingTier2Active_;

  // Cached malloc allocation size for GC memory tracking.

  size_t gcMallocBytesExcludingCode_;

  bool instantiateFunctions(JSContext* cx,
                            const JSObjectVector& funcImports) const;
  bool instantiateMemories(
      JSContext* cx, const WasmMemoryObjectVector& memoryImports,
      MutableHandle<WasmMemoryObjectVector> memoryObjs) const;
  bool instantiateTags(JSContext* cx, WasmTagObjectVector& tagObjs) const;
  bool instantiateImportedTable(JSContext* cx, const TableDesc& td,
                                Handle<WasmTableObject*> table,
                                WasmTableObjectVector* tableObjs,
                                SharedTableVector* tables) const;
  bool instantiateLocalTable(JSContext* cx, const TableDesc& td,
                             WasmTableObjectVector* tableObjs,
                             SharedTableVector* tables) const;
  bool instantiateTables(JSContext* cx,
                         const WasmTableObjectVector& tableImports,
                         MutableHandle<WasmTableObjectVector> tableObjs,
                         SharedTableVector* tables) const;
  bool instantiateGlobals(JSContext* cx, const ValVector& globalImportValues,
                          WasmGlobalObjectVector& globalObjs) const;

  class Tier2GeneratorTaskImpl;

 public:
  Module(const Code& code, ImportVector&& imports, ExportVector&& exports,
         DataSegmentVector&& dataSegments,
         ModuleElemSegmentVector&& elemSegments,
         CustomSectionVector&& customSections,
         const ShareableBytes* debugBytecode = nullptr,
         bool loggingDeserialized = false)
      : code_(&code),
        imports_(std::move(imports)),
        exports_(std::move(exports)),
        dataSegments_(std::move(dataSegments)),
        elemSegments_(std::move(elemSegments)),
        customSections_(std::move(customSections)),
        debugBytecode_(debugBytecode),
        loggingDeserialized_(loggingDeserialized),
        testingTier2Active_(false) {
    initGCMallocBytesExcludingCode();
  }
  ~Module() override;

  const Code& code() const { return *code_; }
  const ModuleSegment& moduleSegment(Tier t) const { return code_->segment(t); }
  const Metadata& metadata() const { return code_->metadata(); }
  const MetadataTier& metadata(Tier t) const { return code_->metadata(t); }
  const ImportVector& imports() const { return imports_; }
  const ExportVector& exports() const { return exports_; }
  const CustomSectionVector& customSections() const { return customSections_; }
  const Bytes& debugBytecode() const { return debugBytecode_->bytes; }
  uint32_t codeLength(Tier t) const { return code_->segment(t).length(); }

  // Instantiate this module with the given imports:

  bool instantiate(JSContext* cx, ImportValues& imports,
                   HandleObject instanceProto,
                   MutableHandle<WasmInstanceObject*> instanceObj) const;

  // Tier-2 compilation may be initiated after the Module is constructed at
  // most once. When tier-2 compilation completes, ModuleGenerator calls
  // finishTier2() from a helper thread, passing tier-variant data which will
  // be installed and made visible.

  void startTier2(const CompileArgs& args, const ShareableBytes& bytecode,
                  JS::OptimizedEncodingListener* listener);
  bool finishTier2(const LinkData& linkData2, UniqueCodeTier code2) const;

  void testingBlockOnTier2Complete() const;
  bool testingTier2Active() const { return testingTier2Active_; }

  // Code caching support.

  [[nodiscard]] bool serialize(const LinkData& linkData, Bytes* bytes) const;
  static RefPtr<Module> deserialize(const uint8_t* begin, size_t size);
  bool loggingDeserialized() const { return loggingDeserialized_; }

  // JS API and JS::WasmModule implementation:

  JSObject* createObject(JSContext* cx) const override;
  JSObject* createObjectForAsmJS(JSContext* cx) const override;

  // about:memory reporting:

  void addSizeOfMisc(MallocSizeOf mallocSizeOf, Metadata::SeenSet* seenMetadata,
                     Code::SeenSet* seenCode, size_t* code, size_t* data) const;

  // GC malloc memory tracking:

  void initGCMallocBytesExcludingCode();
  size_t gcMallocBytesExcludingCode() const {
    return gcMallocBytesExcludingCode_;
  }

  // Generated code analysis support:

  bool extractCode(JSContext* cx, Tier tier, MutableHandleValue vp) const;

  WASM_DECLARE_FRIEND_SERIALIZE_ARGS(Module, const wasm::LinkData& linkData);
};

using MutableModule = RefPtr<Module>;
using SharedModule = RefPtr<const Module>;

// JS API implementations:

[[nodiscard]] bool GetOptimizedEncodingBuildId(JS::BuildIdCharVector* buildId);

}  // namespace wasm
}  // namespace js

#endif  // wasm_module_h
