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

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"

#include "gc/Barrier.h"
#include "gc/Zone.h"
#include "js/Stack.h"  // JS::NativeStackLimit
#include "js/TypeDecls.h"
#include "vm/SharedMem.h"
#include "wasm/WasmExprType.h"   // for ResultType
#include "wasm/WasmLog.h"        // for PrintCallback
#include "wasm/WasmShareable.h"  // for SeenSet
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmValue.h"

namespace js {

class SharedArrayRawBuffer;
class WasmBreakpointSite;

class WasmStructObject;
class WasmArrayObject;

namespace gc {
class StoreBuffer;
}  // namespace gc

namespace wasm {

using mozilla::Atomic;

class FuncImport;
struct FuncImportInstanceData;
class GlobalDesc;
struct TableDesc;
struct TableInstanceData;
struct TagDesc;
struct TagInstanceData;
struct TypeDefInstanceData;
class WasmFrameIter;

// Instance represents a wasm instance and provides all the support for runtime
// execution of code in the instance. Instances share various immutable data
// structures with the Module from which they were instantiated and other
// instances instantiated from the same Module. However, an Instance has no
// direct reference to its source Module which allows a Module to be destroyed
// while it still has live Instances.
//
// The instance's code may be shared among multiple instances.
//
// An Instance is also known as a 'TlsData'. They used to be separate objects,
// but have now been unified. Extant references to 'TlsData' will be cleaned
// up over time.
class alignas(16) Instance {
  // NOTE: The first fields of Instance are reserved for commonly accessed data
  // from the JIT, such that they have as small an offset as possible. See the
  // next note for the end of this region.

  // Pointer to the base of the default memory (or null if there is none).
  uint8_t* memoryBase_;

  // Bounds check limit in bytes (or zero if there is no memory).  This is
  // 64-bits on 64-bit systems so as to allow for heap lengths up to and beyond
  // 4GB, and 32-bits on 32-bit systems, where heaps are limited to 2GB.
  //
  // See "Linear memory addresses and bounds checking" in WasmMemory.cpp.
  uintptr_t boundsCheckLimit_;

  // Null or a pointer to a per-process builtin thunk that will invoke the Debug
  // Trap Handler.
  void* debugTrapHandler_;

  // The containing JS::Realm.
  JS::Realm* realm_;

  // The containing JSContext.
  JSContext* cx_;

  // The pending exception that was found during stack unwinding after a throw.
  //
  //   - Only non-null while unwinding the control stack from a wasm-exit stub.
  //     until the nearest enclosing Wasm try-catch or try-delegate block.
  //   - Set by wasm::HandleThrow, unset by Instance::consumePendingException.
  //   - If the unwind target is a `try-delegate`, it is unset by the delegated
  //     try-catch block or function body block.
  GCPtr<JSObject*> pendingException_;
  // The tag object of the pending exception.
  GCPtr<JSObject*> pendingExceptionTag_;

  // Usually equal to cx->stackLimitForJitCode(JS::StackForUntrustedScript),
  // but can be racily set to trigger immediate trap as an opportunity to
  // CheckForInterrupt without an additional branch.
  Atomic<JS::NativeStackLimit, mozilla::Relaxed> stackLimit_;

  // Set to 1 when wasm should call CheckForInterrupt.
  Atomic<uint32_t, mozilla::Relaxed> interrupt_;

  // The address of the realm()->zone()->needsIncrementalBarrier(). This is
  // specific to this instance and not a process wide field, and so it cannot
  // be linked into code.
  const JS::shadow::Zone::BarrierState* addressOfNeedsIncrementalBarrier_;

 public:
  // NOTE: All fields commonly accessed by the JIT must be above this method,
  // and this method adapted for the last field present. This method is used
  // to assert that we can use compact offsets on x86(-64) for these fields.
  // We cannot have the assertion here, due to C++ 'offsetof' rules.
  static constexpr size_t offsetOfLastCommonJitField() {
    return offsetof(Instance, addressOfNeedsIncrementalBarrier_);
  }

 private:
  // When compiling with tiering, the jumpTable has one entry for each
  // baseline-compiled function.
  void** jumpTable_;

  // General scratch storage for the baseline compiler, which can't always use
  // the stack for this.
  uint32_t baselineScratch_[2];

  // The class_ of WasmValueBox, this is a per-process value. We could patch
  // this into code, but the only use-sites are register restricted and cannot
  // easily use a symbolic address.
  const JSClass* valueBoxClass_;

  // Address of the JitRuntime's arguments rectifier trampoline
  void* jsJitArgsRectifier_;

  // Address of the JitRuntime's exception handler trampoline
  void* jsJitExceptionHandler_;

  // Address of the JitRuntime's object prebarrier trampoline
  void* preBarrierCode_;

  // Address of the store buffer for this instance
  gc::StoreBuffer* storeBuffer_;

  // Weak pointer to WasmInstanceObject that owns this instance
  WeakHeapPtr<WasmInstanceObject*> object_;

  // The wasm::Code for this instance
  const SharedCode code_;

  // The memory for this instance, if any
  const GCPtr<WasmMemoryObject*> memory_;

  // The tables for this instance, if any
  const SharedTableVector tables_;

  // Passive data segments for use with bulk memory instructions
  DataSegmentVector passiveDataSegments_;

  // Passive elem segments for use with bulk memory instructions
  ElemSegmentVector passiveElemSegments_;

  // The wasm::DebugState for this instance, if any
  const UniqueDebugState maybeDebug_;

  // If debugging, this is a per-funcIndex bit table denoting whether debugging
  // is currently enabled for the function within the instance.  The flag is set
  // if any breakpoint or function entry or exit point needs to be visited.  It
  // is OK to conservatively set this flag, but there is very significant
  // overhead to taking a breakpoint trap, so managing it precisely is
  // worthwhile.
  uint32_t* debugFilter_;

  // The exclusive maximum index of a global that has been initialized so far.
  uint32_t maxInitializedGlobalsIndexPlus1_;

  // Pointer that should be freed (due to padding before the Instance).
  void* allocatedBase_;

  // The data must be the last field.  Globals for the module start here
  // and are inline in this structure.  16-byte alignment is required for SIMD
  // data.
  MOZ_ALIGNED_DECL(16, char data_);

  // Internal helpers:
  TypeDefInstanceData* typeDefInstanceData(uint32_t typeIndex) const;
  const void* addressOfGlobalCell(const GlobalDesc& globalDesc) const;
  FuncImportInstanceData& funcImportInstanceData(const FuncImport& fi);
  TableInstanceData& tableInstanceData(uint32_t tableIndex) const;
  TagInstanceData& tagInstanceData(uint32_t tagIndex) const;

  // Only WasmInstanceObject can call the private trace function.
  friend class js::WasmInstanceObject;
  void tracePrivate(JSTracer* trc);

  bool callImport(JSContext* cx, uint32_t funcImportIndex, unsigned argc,
                  uint64_t* argv);

  Instance(JSContext* cx, Handle<WasmInstanceObject*> object,
           const SharedCode& code, Handle<WasmMemoryObject*> memory,
           SharedTableVector&& tables, UniqueDebugState maybeDebug);
  ~Instance();

 public:
  static Instance* create(JSContext* cx, Handle<WasmInstanceObject*> object,
                          const SharedCode& code, uint32_t instanceDataLength,
                          Handle<WasmMemoryObject*> memory,
                          SharedTableVector&& tables,
                          UniqueDebugState maybeDebug);
  static void destroy(Instance* instance);

  bool init(JSContext* cx, const JSObjectVector& funcImports,
            const ValVector& globalImportValues,
            const WasmGlobalObjectVector& globalObjs,
            const WasmTagObjectVector& tagObjs,
            const DataSegmentVector& dataSegments,
            const ElemSegmentVector& elemSegments);

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

  static constexpr size_t offsetOfMemoryBase() {
    return offsetof(Instance, memoryBase_);
  }
  static constexpr size_t offsetOfBoundsCheckLimit() {
    return offsetof(Instance, boundsCheckLimit_);
  }
  static constexpr size_t offsetOfDebugTrapHandler() {
    return offsetof(Instance, debugTrapHandler_);
  }

  static constexpr size_t offsetOfRealm() { return offsetof(Instance, realm_); }
  static constexpr size_t offsetOfCx() { return offsetof(Instance, cx_); }
  static constexpr size_t offsetOfValueBoxClass() {
    return offsetof(Instance, valueBoxClass_);
  }
  static constexpr size_t offsetOfPendingException() {
    return offsetof(Instance, pendingException_);
  }
  static constexpr size_t offsetOfPendingExceptionTag() {
    return offsetof(Instance, pendingExceptionTag_);
  }
  static constexpr size_t offsetOfStackLimit() {
    return offsetof(Instance, stackLimit_);
  }
  static constexpr size_t offsetOfInterrupt() {
    return offsetof(Instance, interrupt_);
  }
  static constexpr size_t offsetOfAddressOfNeedsIncrementalBarrier() {
    return offsetof(Instance, addressOfNeedsIncrementalBarrier_);
  }
  static constexpr size_t offsetOfJumpTable() {
    return offsetof(Instance, jumpTable_);
  }
  static constexpr size_t offsetOfBaselineScratch() {
    return offsetof(Instance, baselineScratch_);
  }
  static constexpr size_t sizeOfBaselineScratch() {
    return sizeof(baselineScratch_);
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
  static constexpr size_t offsetOfDebugFilter() {
    return offsetof(Instance, debugFilter_);
  }
  static constexpr size_t offsetOfData() { return offsetof(Instance, data_); }
  static constexpr size_t offsetInData(size_t offset) {
    return offsetOfData() + offset;
  }

  JSContext* cx() const { return cx_; }
  void* debugTrapHandler() const { return debugTrapHandler_; }
  void setDebugTrapHandler(void* newHandler) { debugTrapHandler_ = newHandler; }
  JS::Realm* realm() const { return realm_; }
  bool debugEnabled() const { return !!maybeDebug_; }
  DebugState& debug() { return *maybeDebug_; }
  uint8_t* data() const { return (uint8_t*)&data_; }
  const SharedTableVector& tables() const { return tables_; }
  SharedMem<uint8_t*> memoryBase() const;
  WasmMemoryObject* memory() const;
  size_t memoryMappedSize() const;
  SharedArrayRawBuffer* sharedMemoryBuffer() const;  // never null
  bool memoryAccessInGuardRegion(const uint8_t* addr, unsigned numBytes) const;

  // Methods to set, test and clear the interrupt fields. Both interrupt
  // fields are Relaxed and so no consistency/ordering can be assumed.

  void setInterrupt();
  bool isInterrupted() const;
  void resetInterrupt(JSContext* cx);

  bool debugFilter(uint32_t funcIndex) const;
  void setDebugFilter(uint32_t funcIndex, bool value);

  const Code& code() const { return *code_; }
  inline const CodeTier& code(Tier t) const;
  inline uint8_t* codeBase(Tier t) const;
  inline const MetadataTier& metadata(Tier t) const;
  inline const Metadata& metadata() const;
  inline bool isAsmJS() const;

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

  // Exception handling support

  void setPendingException(HandleAnyRef exn);

  // Constant expression support

  void constantGlobalGet(uint32_t globalIndex, MutableHandleVal result);
  [[nodiscard]] bool constantRefFunc(uint32_t funcIndex,
                                     MutableHandleFuncRef result);
  WasmStructObject* constantStructNewDefault(JSContext* cx, uint32_t typeIndex);
  WasmArrayObject* constantArrayNewDefault(JSContext* cx, uint32_t typeIndex,
                                           uint32_t numElements);

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
  void destroyBreakpointSite(JS::GCContext* gcx, uint32_t offset);

  // about:memory reporting:

  void addSizeOfMisc(MallocSizeOf mallocSizeOf, SeenSet<Metadata>* seenMetadata,
                     SeenSet<Code>* seenCode, SeenSet<Table>* seenTables,
                     size_t* code, size_t* data) const;

  // Wasm disassembly support

  void disassembleExport(JSContext* cx, uint32_t funcIndex, Tier tier,
                         PrintCallback printString) const;

 public:
  // Functions to be called directly from wasm code.
  static int32_t callImport_general(Instance*, int32_t, int32_t, uint64_t*);
  static uint32_t memoryGrow_m32(Instance* instance, uint32_t delta);
  static uint64_t memoryGrow_m64(Instance* instance, uint64_t delta);
  static uint32_t memorySize_m32(Instance* instance);
  static uint64_t memorySize_m64(Instance* instance);
  static int32_t memCopy_m32(Instance* instance, uint32_t dstByteOffset,
                             uint32_t srcByteOffset, uint32_t len,
                             uint8_t* memBase);
  static int32_t memCopyShared_m32(Instance* instance, uint32_t dstByteOffset,
                                   uint32_t srcByteOffset, uint32_t len,
                                   uint8_t* memBase);
  static int32_t memCopy_m64(Instance* instance, uint64_t dstByteOffset,
                             uint64_t srcByteOffset, uint64_t len,
                             uint8_t* memBase);
  static int32_t memCopyShared_m64(Instance* instance, uint64_t dstByteOffset,
                                   uint64_t srcByteOffset, uint64_t len,
                                   uint8_t* memBase);
  static int32_t memFill_m32(Instance* instance, uint32_t byteOffset,
                             uint32_t value, uint32_t len, uint8_t* memBase);
  static int32_t memFillShared_m32(Instance* instance, uint32_t byteOffset,
                                   uint32_t value, uint32_t len,
                                   uint8_t* memBase);
  static int32_t memFill_m64(Instance* instance, uint64_t byteOffset,
                             uint32_t value, uint64_t len, uint8_t* memBase);
  static int32_t memFillShared_m64(Instance* instance, uint64_t byteOffset,
                                   uint32_t value, uint64_t len,
                                   uint8_t* memBase);
  static int32_t memInit_m32(Instance* instance, uint32_t dstOffset,
                             uint32_t srcOffset, uint32_t len,
                             uint32_t segIndex);
  static int32_t memInit_m64(Instance* instance, uint64_t dstOffset,
                             uint32_t srcOffset, uint32_t len,
                             uint32_t segIndex);
  static int32_t dataDrop(Instance* instance, uint32_t segIndex);
  static int32_t tableCopy(Instance* instance, uint32_t dstOffset,
                           uint32_t srcOffset, uint32_t len,
                           uint32_t dstTableIndex, uint32_t srcTableIndex);
  static int32_t tableFill(Instance* instance, uint32_t start, void* value,
                           uint32_t len, uint32_t tableIndex);
  static int32_t memDiscard_m32(Instance* instance, uint32_t byteOffset,
                                uint32_t byteLen, uint8_t* memBase);
  static int32_t memDiscardShared_m32(Instance* instance, uint32_t byteOffset,
                                      uint32_t byteLen, uint8_t* memBase);
  static int32_t memDiscard_m64(Instance* instance, uint64_t byteOffset,
                                uint64_t byteLen, uint8_t* memBase);
  static int32_t memDiscardShared_m64(Instance* instance, uint64_t byteOffset,
                                      uint64_t byteLen, uint8_t* memBase);
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
  static int32_t elemDrop(Instance* instance, uint32_t segIndex);
  static int32_t wait_i32_m32(Instance* instance, uint32_t byteOffset,
                              int32_t value, int64_t timeout);
  static int32_t wait_i32_m64(Instance* instance, uint64_t byteOffset,
                              int32_t value, int64_t timeout);
  static int32_t wait_i64_m32(Instance* instance, uint32_t byteOffset,
                              int64_t value, int64_t timeout);
  static int32_t wait_i64_m64(Instance* instance, uint64_t byteOffset,
                              int64_t value, int64_t timeout);
  static int32_t wake_m32(Instance* instance, uint32_t byteOffset,
                          int32_t count);
  static int32_t wake_m64(Instance* instance, uint64_t byteOffset,
                          int32_t count);
  static void* refFunc(Instance* instance, uint32_t funcIndex);
  static void postBarrier(Instance* instance, gc::Cell** location);
  static void postBarrierPrecise(Instance* instance, JSObject** location,
                                 JSObject* prev);
  static void postBarrierPreciseWithOffset(Instance* instance, JSObject** base,
                                           uint32_t offset, JSObject* prev);
  static void* exceptionNew(Instance* instance, JSObject* tag);
  static int32_t throwException(Instance* instance, JSObject* exn);
  static void* structNew(Instance* instance, TypeDefInstanceData* typeDefData);
  static void* structNewUninit(Instance* instance,
                               TypeDefInstanceData* typeDefData);
  static void* arrayNew(Instance* instance, uint32_t numElements,
                        TypeDefInstanceData* typeDefData);
  static void* arrayNewUninit(Instance* instance, uint32_t numElements,
                              TypeDefInstanceData* typeDefData);
  static void* arrayNewData(Instance* instance, uint32_t segByteOffset,
                            uint32_t numElements,
                            TypeDefInstanceData* typeDefData,
                            uint32_t segIndex);
  static void* arrayNewElem(Instance* instance, uint32_t segElemIndex,
                            uint32_t numElements,
                            TypeDefInstanceData* typeDefData,
                            uint32_t segIndex);
  static int32_t arrayCopy(Instance* instance, void* dstArray,
                           uint32_t dstIndex, void* srcArray, uint32_t srcIndex,
                           uint32_t numElements, uint32_t elementSize);
  static int32_t refTest(Instance* instance, void* refPtr,
                         const wasm::TypeDef* typeDef);
  static int32_t intrI8VecMul(Instance* instance, uint32_t dest, uint32_t src1,
                              uint32_t src2, uint32_t len, uint8_t* memBase);
};

bool ResultsToJSValue(JSContext* cx, ResultType type, void* registerResultLoc,
                      Maybe<char*> stackResultsLoc, MutableHandleValue rval,
                      CoercionLevel level = CoercionLevel::Spec);

// Report an error to `cx` and mark it as a 'trap' so that it cannot be caught
// by wasm exception handlers.
void ReportTrapError(JSContext* cx, unsigned errorNumber);

// Instance is not a GC thing itself but contains GC thing pointers. Ensure they
// are traced appropriately.
void TraceInstanceEdge(JSTracer* trc, Instance* instance, const char* name);

}  // namespace wasm
}  // namespace js

#endif  // wasm_instance_h
