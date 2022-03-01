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

#ifndef wasm_instance_h
#define wasm_instance_h

#include "gc/Barrier.h"
#include "gc/Zone.h"
#include "vm/SharedMem.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmFrameIter.h"  // js::wasm::WasmFrameIter
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

class Instance {
  JS::Realm* const realm_;
  WeakHeapPtrWasmInstanceObject object_;
  void* jsJitArgsRectifier_;
  void* jsJitExceptionHandler_;
  void* preBarrierCode_;
  const SharedCode code_;
  const UniqueTlsData tlsData_;
  const GCPtrWasmMemoryObject memory_;
  const SharedExceptionTagVector exceptionTags_;
  const SharedTableVector tables_;
  DataSegmentVector passiveDataSegments_;
  ElemSegmentVector passiveElemSegments_;
  const UniqueDebugState maybeDebug_;
#ifdef ENABLE_WASM_GC
  bool hasGcTypes_;
#endif

  // Internal helpers:
  const void** addressOfTypeId(const TypeIdDesc& typeId) const;
  FuncImportTls& funcImportTls(const FuncImport& fi);
  TableTls& tableTls(const TableDesc& td) const;

  // Only WasmInstanceObject can call the private trace function.
  friend class js::WasmInstanceObject;
  void tracePrivate(JSTracer* trc);

  bool callImport(JSContext* cx, uint32_t funcImportIndex, unsigned argc,
                  uint64_t* argv);

 public:
  Instance(JSContext* cx, HandleWasmInstanceObject object, SharedCode code,
           UniqueTlsData tlsData, HandleWasmMemoryObject memory,
           SharedExceptionTagVector&& exceptionTags, SharedTableVector&& tables,
           UniqueDebugState maybeDebug);
  ~Instance();
  bool init(JSContext* cx, const JSFunctionVector& funcImports,
            const ValVector& globalImportValues,
            const WasmGlobalObjectVector& globalObjs,
            const DataSegmentVector& dataSegments,
            const ElemSegmentVector& elemSegments);
  void trace(JSTracer* trc);

  // Trace any GC roots on the stack, for the frame associated with |wfi|,
  // whose next instruction to execute is |nextPC|.
  //
  // For consistency checking of StackMap sizes in debug builds, this also
  // takes |highestByteVisitedInPrevFrame|, which is the address of the
  // highest byte scanned in the frame below this one on the stack, and in
  // turn it returns the address of the highest byte scanned in this frame.
  uintptr_t traceFrame(JSTracer* trc, const wasm::WasmFrameIter& wfi,
                       uint8_t* nextPC,
                       uintptr_t highestByteVisitedInPrevFrame);

  JS::Realm* realm() const { return realm_; }
  const Code& code() const { return *code_; }
  const CodeTier& code(Tier t) const { return code_->codeTier(t); }
  bool debugEnabled() const { return !!maybeDebug_; }
  DebugState& debug() { return *maybeDebug_; }
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
  SharedArrayRawBuffer* sharedMemoryBuffer() const;  // never null
  bool memoryAccessInGuardRegion(const uint8_t* addr, unsigned numBytes) const;
  const SharedExceptionTagVector& exceptionTags() const {
    return exceptionTags_;
  }

  static constexpr size_t offsetOfJSJitArgsRectifier() {
    return offsetof(Instance, jsJitArgsRectifier_);
  }
  static constexpr size_t offsetOfJSJitExceptionHandler() {
    return offsetof(Instance, jsJitExceptionHandler_);
  }
  static constexpr size_t offsetOfPreBarrierCode() {
    return offsetof(Instance, preBarrierCode_);
  }

  // This method returns a pointer to the GC object that owns this Instance.
  // Instances may be reached via weak edges (e.g., Realm::instances_)
  // so this perform a read-barrier on the returned object unless the barrier
  // is explicitly waived.

  WasmInstanceObject* object() const;
  WasmInstanceObject* objectUnbarriered() const;

  // Execute the given export given the JS call arguments, storing the return
  // value in args.rval.

  [[nodiscard]] bool callExport(JSContext* cx, uint32_t funcIndex,
                                CallArgs args,
                                CoercionLevel level = CoercionLevel::Spec);

  // Return the name associated with a given function index, or generate one
  // if none was given by the module.

  JSAtom* getFuncDisplayAtom(JSContext* cx, uint32_t funcIndex) const;
  void ensureProfilingLabels(bool profilingEnabled) const;

  // Called by Wasm(Memory|Table)Object when a moving resize occurs:

  void onMovingGrowMemory();
  void onMovingGrowTable(const Table* theTable);

  // Called to apply a single ElemSegment at a given offset, assuming
  // that all bounds validation has already been performed.

  [[nodiscard]] bool initElems(uint32_t tableIndex, const ElemSegment& seg,
                               uint32_t dstOffset, uint32_t srcOffset,
                               uint32_t len);

  // Debugger support:

  JSString* createDisplayURL(JSContext* cx);
  WasmBreakpointSite* getOrCreateBreakpointSite(JSContext* cx, uint32_t offset);
  void destroyBreakpointSite(JSFreeOp* fop, uint32_t offset);

  // about:memory reporting:

  void addSizeOfMisc(MallocSizeOf mallocSizeOf, Metadata::SeenSet* seenMetadata,
                     Code::SeenSet* seenCode, Table::SeenSet* seenTables,
                     size_t* code, size_t* data) const;

  // Wasm disassembly support

  void disassembleExport(JSContext* cx, uint32_t funcIndex, Tier tier,
                         PrintCallback printString) const;

 public:
  // Functions to be called directly from wasm code.
  static int32_t callImport_general(Instance*, int32_t, int32_t, uint64_t*);
  static uint32_t memoryGrow_i32(Instance* instance, uint32_t delta);
  static uint32_t memorySize_i32(Instance* instance);
  static int32_t wait_i32(Instance* instance, uint32_t byteOffset,
                          int32_t value, int64_t timeout);
  static int32_t wait_i64(Instance* instance, uint32_t byteOffset,
                          int64_t value, int64_t timeout);
  static int32_t wake(Instance* instance, uint32_t byteOffset, int32_t count);
  static int32_t memCopy32(Instance* instance, uint32_t dstByteOffset,
                           uint32_t srcByteOffset, uint32_t len,
                           uint8_t* memBase);
  static int32_t memCopyShared32(Instance* instance, uint32_t dstByteOffset,
                                 uint32_t srcByteOffset, uint32_t len,
                                 uint8_t* memBase);
  static int32_t dataDrop(Instance* instance, uint32_t segIndex);
  static int32_t memFill32(Instance* instance, uint32_t byteOffset,
                           uint32_t value, uint32_t len, uint8_t* memBase);
  static int32_t memFillShared32(Instance* instance, uint32_t byteOffset,
                                 uint32_t value, uint32_t len,
                                 uint8_t* memBase);
  static int32_t memInit32(Instance* instance, uint32_t dstOffset,
                           uint32_t srcOffset, uint32_t len, uint32_t segIndex);
  static int32_t tableCopy(Instance* instance, uint32_t dstOffset,
                           uint32_t srcOffset, uint32_t len,
                           uint32_t dstTableIndex, uint32_t srcTableIndex);
  static int32_t elemDrop(Instance* instance, uint32_t segIndex);
  static int32_t tableFill(Instance* instance, uint32_t start, void* value,
                           uint32_t len, uint32_t tableIndex);
  static void* tableGet(Instance* instance, uint32_t index,
                        uint32_t tableIndex);
  static uint32_t tableGrow(Instance* instance, void* initValue, uint32_t delta,
                            uint32_t tableIndex);
  static int32_t tableSet(Instance* instance, uint32_t index, void* value,
                          uint32_t tableIndex);
  static uint32_t tableSize(Instance* instance, uint32_t tableIndex);
  static int32_t tableInit(Instance* instance, uint32_t dstOffset,
                           uint32_t srcOffset, uint32_t len, uint32_t segIndex,
                           uint32_t tableIndex);
  static void* refFunc(Instance* instance, uint32_t funcIndex);
  static void preBarrierFiltering(Instance* instance, gc::Cell** location);
  static void postBarrier(Instance* instance, gc::Cell** location);
  static void postBarrierFiltering(Instance* instance, gc::Cell** location);
  static void* structNew(Instance* instance, void* structDescr);
#ifdef ENABLE_WASM_EXCEPTIONS
  static void* exceptionNew(Instance* instance, uint32_t exnIndex,
                            uint32_t nbytes);
  static void* throwException(Instance* instance, JSObject* exn);
  static uint32_t getLocalExceptionIndex(Instance* instance, JSObject* exn);
  static int32_t pushRefIntoExn(Instance* instance, JSObject* exn,
                                JSObject* ref);
#endif
  static void* arrayNew(Instance* instance, uint32_t length, void* arrayDescr);
  static int32_t refTest(Instance* instance, void* refPtr, void* rttPtr);
  static void* rttSub(Instance* instance, void* rttPtr);
};

using UniqueInstance = UniquePtr<Instance>;

bool ResultsToJSValue(JSContext* cx, ResultType type, void* registerResultLoc,
                      Maybe<char*> stackResultsLoc, MutableHandleValue rval,
                      CoercionLevel level = CoercionLevel::Spec);

}  // namespace wasm
}  // namespace js

#endif  // wasm_instance_h
