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

using mozilla::Maybe;

namespace JS {
class OptimizedEncodingListener;
}

namespace js {
namespace wasm {

struct CompileArgs;

// In the context of wasm, the OptimizedEncodingListener specifically is
// listening for the completion of complete tier-2.

using CompleteTier2Listener = RefPtr<JS::OptimizedEncodingListener>;

// Report tier-2 compilation results off-thread.  If `maybeFuncIndex` is
// `Some`, this report is for a partial tier-2 compilation of the specified
// function.  Otherwise it's for a complete tier-2 compilation.

void ReportTier2ResultsOffThread(bool cancelled, bool success,
                                 Maybe<uint32_t> maybeFuncIndex,
                                 const ScriptedCaller& scriptedCaller,
                                 const UniqueChars& error,
                                 const UniqueCharsVector& warnings);

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
// Fully linked-and-instantiated code (represented by SharedCode) can be shared
// between instances.

class Module : public JS::WasmModule {
  // This has the same lifetime end as Module itself -- it can be dropped when
  // Module itself is dropped.
  const SharedModuleMetadata moduleMeta_;

  // This contains all compilation artifacts for the module.
  const SharedCode code_;

  // This field is set during complete tier-2 compilation and cleared on
  // success or failure. These happen on different threads and are serialized
  // by the control flow of helper tasks.

  mutable CompleteTier2Listener completeTier2Listener_;

  // This flag is used for logging (and testing) purposes to indicate
  // whether the module was deserialized (from a cache).

  const bool loggingDeserialized_;

  // This flag is only used for testing purposes and is cleared on success or
  // failure. The field is racily polled from various threads.

  mutable mozilla::Atomic<bool> testingTier2Active_;

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

  class CompleteTier2GeneratorTaskImpl;

 public:
  class PartialTier2CompileTaskImpl;

  Module(const ModuleMetadata& moduleMeta, const Code& code,
         bool loggingDeserialized = false)
      : moduleMeta_(&moduleMeta),
        code_(&code),
        loggingDeserialized_(loggingDeserialized),
        testingTier2Active_(false) {
    initGCMallocBytesExcludingCode();
  }
  ~Module() override;

  const Code& code() const { return *code_; }
  const ModuleMetadata& moduleMeta() const { return *moduleMeta_; }
  const CodeMetadata& codeMeta() const { return code_->codeMeta(); }
  const CodeTailMetadata& codeTailMeta() const { return code_->codeTailMeta(); }
  const CodeMetadataForAsmJS* codeMetaForAsmJS() const {
    return code_->codeMetaForAsmJS();
  }
  const BytecodeSource& debugBytecode() const {
    return codeTailMeta().debugBytecode.source();
  }
  uint32_t tier1CodeMemoryUsed() const { return code_->tier1CodeMemoryUsed(); }

  // Instantiate this module with the given imports:

  bool instantiate(JSContext* cx, ImportValues& imports,
                   HandleObject instanceProto,
                   MutableHandle<WasmInstanceObject*> instanceObj) const;

  // Tier-2 compilation may be initiated after the Module is constructed at
  // most once. When tier-2 compilation completes, ModuleGenerator calls
  // finishTier2() from a helper thread, passing tier-variant data which will
  // be installed and made visible.

  void startTier2(const ShareableBytes* codeSection,
                  JS::OptimizedEncodingListener* listener);
  bool finishTier2(UniqueCodeBlock tier2CodeBlock, UniqueLinkData tier2LinkData,
                   const CompileAndLinkStats& tier2Stats) const;

  void testingBlockOnTier2Complete() const;
  bool testingTier2Active() const { return testingTier2Active_; }

  // Code caching support.

  bool canSerialize() const;
  [[nodiscard]] bool serialize(Bytes* bytes) const;
  static RefPtr<Module> deserialize(const uint8_t* begin, size_t size);
  bool loggingDeserialized() const { return loggingDeserialized_; }

  // JS API and JS::WasmModule implementation:

  JSObject* createObject(JSContext* cx) const override;
  JSObject* createObjectForAsmJS(JSContext* cx) const override;

  // about:memory reporting:

  void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf,
                     CodeMetadata::SeenSet* seenCodeMeta,
                     CodeMetadataForAsmJS::SeenSet* seenCodeMetaForAsmJS,
                     Code::SeenSet* seenCode, size_t* code, size_t* data) const;

  // GC malloc memory tracking:

  void initGCMallocBytesExcludingCode();
  size_t gcMallocBytesExcludingCode() const {
    return gcMallocBytesExcludingCode_;
  }

  // Generated code analysis support:

  bool extractCode(JSContext* cx, Tier tier, MutableHandleValue vp) const;

  WASM_DECLARE_FRIEND_SERIALIZE(Module);
};

using MutableModule = RefPtr<Module>;
using SharedModule = RefPtr<const Module>;

// JS API implementations:

[[nodiscard]] bool GetOptimizedEncodingBuildId(JS::BuildIdCharVector* buildId);

}  // namespace wasm
}  // namespace js

#endif  // wasm_module_h
