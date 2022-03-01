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

#include "wasm/WasmModule.h"

#include <chrono>

#include "jit/JitOptions.h"
#include "js/BuildId.h"                 // JS::BuildIdCharVector
#include "js/experimental/TypedData.h"  // JS_NewUint8Array
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "threading/LockGuard.h"
#include "vm/HelperThreadState.h"  // Tier2GeneratorTask
#include "vm/PlainObject.h"        // js::PlainObject
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmUtility.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSAtom-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

class Module::Tier2GeneratorTaskImpl : public Tier2GeneratorTask {
  SharedCompileArgs compileArgs_;
  SharedBytes bytecode_;
  SharedModule module_;
  Atomic<bool> cancelled_;

 public:
  Tier2GeneratorTaskImpl(const CompileArgs& compileArgs,
                         const ShareableBytes& bytecode, Module& module)
      : compileArgs_(&compileArgs),
        bytecode_(&bytecode),
        module_(&module),
        cancelled_(false) {}

  ~Tier2GeneratorTaskImpl() override {
    module_->tier2Listener_ = nullptr;
    module_->testingTier2Active_ = false;
  }

  void cancel() override { cancelled_ = true; }

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override {
    {
      AutoUnlockHelperThreadState unlock(locked);
      CompileTier2(*compileArgs_, bytecode_->bytes, *module_, &cancelled_);
    }

    // During shutdown the main thread will wait for any ongoing (cancelled)
    // tier-2 generation to shut down normally.  To do so, it waits on the
    // HelperThreadState's condition variable for the count of finished
    // generators to rise.
    HelperThreadState().incWasmTier2GeneratorsFinished(locked);

    // The task is finished, release it.
    js_delete(this);
  }

  ThreadType threadType() override {
    return ThreadType::THREAD_TYPE_WASM_GENERATOR_TIER2;
  }
};

Module::~Module() {
  // Note: Modules can be destroyed on any thread.
  MOZ_ASSERT(!tier2Listener_);
  MOZ_ASSERT(!testingTier2Active_);
}

void Module::startTier2(const CompileArgs& args, const ShareableBytes& bytecode,
                        JS::OptimizedEncodingListener* listener) {
  MOZ_ASSERT(!testingTier2Active_);

  auto task = MakeUnique<Tier2GeneratorTaskImpl>(args, bytecode, *this);
  if (!task) {
    return;
  }

  // These will be cleared asynchronously by ~Tier2GeneratorTaskImpl() if not
  // sooner by finishTier2().
  tier2Listener_ = listener;
  testingTier2Active_ = true;

  StartOffThreadWasmTier2Generator(std::move(task));
}

bool Module::finishTier2(const LinkData& linkData2,
                         UniqueCodeTier code2) const {
  MOZ_ASSERT(code().bestTier() == Tier::Baseline &&
             code2->tier() == Tier::Optimized);

  // Install the data in the data structures. They will not be visible
  // until commitTier2().

  if (!code().setTier2(std::move(code2), linkData2)) {
    return false;
  }

  // Before we can make tier-2 live, we need to compile tier2 versions of any
  // extant tier1 lazy stubs (otherwise, tiering would break the assumption
  // that any extant exported wasm function has had a lazy entry stub already
  // compiled for it).
  //
  // Also see doc block for stubs in WasmJS.cpp.
  {
    // We need to prevent new tier1 stubs generation until we've committed
    // the newer tier2 stubs, otherwise we might not generate one tier2
    // stub that has been generated for tier1 before we committed.

    const MetadataTier& metadataTier1 = metadata(Tier::Baseline);

    auto stubs1 = code().codeTier(Tier::Baseline).lazyStubs().lock();
    auto stubs2 = code().codeTier(Tier::Optimized).lazyStubs().lock();

    MOZ_ASSERT(stubs2->empty());

    Uint32Vector funcExportIndices;
    for (size_t i = 0; i < metadataTier1.funcExports.length(); i++) {
      const FuncExport& fe = metadataTier1.funcExports[i];
      if (fe.hasEagerStubs()) {
        continue;
      }
      if (!stubs1->hasStub(fe.funcIndex())) {
        continue;
      }
      if (!funcExportIndices.emplaceBack(i)) {
        return false;
      }
    }

    const CodeTier& tier2 = code().codeTier(Tier::Optimized);

    Maybe<size_t> stub2Index;
    if (!stubs2->createTier2(funcExportIndices, tier2, &stub2Index)) {
      return false;
    }

    // Now that we can't fail or otherwise abort tier2, make it live.

    MOZ_ASSERT(!code().hasTier2());
    code().commitTier2();

    stubs2->setJitEntries(stub2Index, code());
  }

  // And we update the jump vectors with pointers to tier-2 functions and eager
  // stubs.  Callers will continue to invoke tier-1 code until, suddenly, they
  // will invoke tier-2 code.  This is benign.

  uint8_t* base = code().segment(Tier::Optimized).base();
  for (const CodeRange& cr : metadata(Tier::Optimized).codeRanges) {
    // These are racy writes that we just want to be visible, atomically,
    // eventually.  All hardware we care about will do this right.  But
    // we depend on the compiler not splitting the stores hidden inside the
    // set*Entry functions.
    if (cr.isFunction()) {
      code().setTieringEntry(cr.funcIndex(), base + cr.funcTierEntry());
    } else if (cr.isJitEntry()) {
      code().setJitEntry(cr.funcIndex(), base + cr.begin());
    }
  }

  // Tier-2 is done; let everyone know. Mark tier-2 active for testing
  // purposes so that wasmHasTier2CompilationCompleted() only returns true
  // after tier-2 has been fully cached.

  if (tier2Listener_) {
    serialize(linkData2, *tier2Listener_);
    tier2Listener_ = nullptr;
  }
  testingTier2Active_ = false;

  return true;
}

void Module::testingBlockOnTier2Complete() const {
  while (testingTier2Active_) {
    ThisThread::SleepMilliseconds(1);
  }
}

/* virtual */
size_t Module::serializedSize(const LinkData& linkData) const {
  JS::BuildIdCharVector buildId;
  {
    AutoEnterOOMUnsafeRegion oom;
    if (!GetOptimizedEncodingBuildId(&buildId)) {
      oom.crash("getting build id");
    }
  }

  return SerializedPodVectorSize(buildId) + linkData.serializedSize() +
         SerializedVectorSize(imports_) + SerializedVectorSize(exports_) +
         SerializedVectorSize(dataSegments_) +
         SerializedVectorSize(elemSegments_) +
         SerializedVectorSize(customSections_) + code_->serializedSize();
}

/* virtual */
void Module::serialize(const LinkData& linkData, uint8_t* begin,
                       size_t size) const {
  MOZ_RELEASE_ASSERT(!metadata().debugEnabled);
  MOZ_RELEASE_ASSERT(code_->hasTier(Tier::Serialized));

  JS::BuildIdCharVector buildId;
  {
    AutoEnterOOMUnsafeRegion oom;
    if (!GetOptimizedEncodingBuildId(&buildId)) {
      oom.crash("getting build id");
    }
  }

  uint8_t* cursor = begin;
  cursor = SerializePodVector(cursor, buildId);
  cursor = linkData.serialize(cursor);
  cursor = SerializeVector(cursor, imports_);
  cursor = SerializeVector(cursor, exports_);
  cursor = SerializeVector(cursor, dataSegments_);
  cursor = SerializeVector(cursor, elemSegments_);
  cursor = SerializeVector(cursor, customSections_);
  cursor = code_->serialize(cursor, linkData);
  MOZ_RELEASE_ASSERT(cursor == begin + size);
}

/* static */
MutableModule Module::deserialize(const uint8_t* begin, size_t size,
                                  Metadata* maybeMetadata) {
  MutableMetadata metadata(maybeMetadata);
  if (!metadata) {
    metadata = js_new<Metadata>();
    if (!metadata) {
      return nullptr;
    }
  }

  const uint8_t* cursor = begin;

  JS::BuildIdCharVector currentBuildId;
  if (!GetOptimizedEncodingBuildId(&currentBuildId)) {
    return nullptr;
  }

  JS::BuildIdCharVector deserializedBuildId;
  cursor = DeserializePodVector(cursor, &deserializedBuildId);
  if (!cursor) {
    return nullptr;
  }

  MOZ_RELEASE_ASSERT(EqualContainers(currentBuildId, deserializedBuildId));

  LinkData linkData(Tier::Serialized);
  cursor = linkData.deserialize(cursor);
  if (!cursor) {
    return nullptr;
  }

  ImportVector imports;
  cursor = DeserializeVector(cursor, &imports);
  if (!cursor) {
    return nullptr;
  }

  ExportVector exports;
  cursor = DeserializeVector(cursor, &exports);
  if (!cursor) {
    return nullptr;
  }

  DataSegmentVector dataSegments;
  cursor = DeserializeVector(cursor, &dataSegments);
  if (!cursor) {
    return nullptr;
  }

  ElemSegmentVector elemSegments;
  cursor = DeserializeVector(cursor, &elemSegments);
  if (!cursor) {
    return nullptr;
  }

  CustomSectionVector customSections;
  cursor = DeserializeVector(cursor, &customSections);
  if (!cursor) {
    return nullptr;
  }

  SharedCode code;
  cursor = Code::deserialize(cursor, linkData, *metadata, &code);
  if (!cursor) {
    return nullptr;
  }

  MOZ_RELEASE_ASSERT(cursor == begin + size);
  MOZ_RELEASE_ASSERT(!!maybeMetadata == code->metadata().isAsmJS());

  if (metadata->nameCustomSectionIndex) {
    metadata->namePayload =
        customSections[*metadata->nameCustomSectionIndex].payload;
  } else {
    MOZ_RELEASE_ASSERT(!metadata->moduleName);
    MOZ_RELEASE_ASSERT(metadata->funcNames.empty());
  }

  return js_new<Module>(*code, std::move(imports), std::move(exports),
                        std::move(dataSegments), std::move(elemSegments),
                        std::move(customSections), nullptr, nullptr, nullptr,
                        /* loggingDeserialized = */ true);
}

void Module::serialize(const LinkData& linkData,
                       JS::OptimizedEncodingListener& listener) const {
  auto bytes = MakeUnique<JS::OptimizedEncodingBytes>();
  if (!bytes || !bytes->resize(serializedSize(linkData))) {
    return;
  }

  serialize(linkData, bytes->begin(), bytes->length());

  listener.storeOptimizedEncoding(std::move(bytes));
}

/* virtual */
JSObject* Module::createObject(JSContext* cx) const {
  if (!GlobalObject::ensureConstructor(cx, cx->global(), JSProto_WebAssembly)) {
    return nullptr;
  }

  RootedObject proto(
      cx, &cx->global()->getPrototype(JSProto_WasmModule).toObject());
  return WasmModuleObject::create(cx, *this, proto);
}

/* virtual */
JSObject* Module::createObjectForAsmJS(JSContext* cx) const {
  // Use nullptr to get the default object prototype. These objects are never
  // exposed to script for asm.js.
  return WasmModuleObject::create(cx, *this, nullptr);
}

bool wasm::GetOptimizedEncodingBuildId(JS::BuildIdCharVector* buildId) {
  // From a JS API perspective, the "build id" covers everything that can
  // cause machine code to become invalid, so include both the actual build-id
  // and cpu-id.

  if (!GetBuildId || !GetBuildId(buildId)) {
    return false;
  }

  uint32_t cpu = ObservedCPUFeatures();

  if (!buildId->reserve(buildId->length() +
                        12 /* "()" + 8 nibbles + "m[+-]" */)) {
    return false;
  }

  buildId->infallibleAppend('(');
  while (cpu) {
    buildId->infallibleAppend('0' + (cpu & 0xf));
    cpu >>= 4;
  }
  buildId->infallibleAppend(')');

  buildId->infallibleAppend('m');
  buildId->infallibleAppend(wasm::IsHugeMemoryEnabled() ? '+' : '-');

  return true;
}

/* virtual */
void Module::addSizeOfMisc(MallocSizeOf mallocSizeOf,
                           Metadata::SeenSet* seenMetadata,
                           Code::SeenSet* seenCode, size_t* code,
                           size_t* data) const {
  code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenMetadata, seenCode, code,
                                data);
  *data += mallocSizeOf(this) +
           SizeOfVectorExcludingThis(imports_, mallocSizeOf) +
           SizeOfVectorExcludingThis(exports_, mallocSizeOf) +
           SizeOfVectorExcludingThis(dataSegments_, mallocSizeOf) +
           SizeOfVectorExcludingThis(elemSegments_, mallocSizeOf) +
           SizeOfVectorExcludingThis(customSections_, mallocSizeOf);

  if (debugUnlinkedCode_) {
    *data += debugUnlinkedCode_->sizeOfExcludingThis(mallocSizeOf);
  }
}

void Module::initGCMallocBytesExcludingCode() {
  // The size doesn't have to be exact so use the serialization framework to
  // calculate a value.
  gcMallocBytesExcludingCode_ = sizeof(*this) + SerializedVectorSize(imports_) +
                                SerializedVectorSize(exports_) +
                                SerializedVectorSize(dataSegments_) +
                                SerializedVectorSize(elemSegments_) +
                                SerializedVectorSize(customSections_);
}

// Extracting machine code as JS object. The result has the "code" property, as
// a Uint8Array, and the "segments" property as array objects. The objects
// contain offsets in the "code" array and basic information about a code
// segment/function body.
bool Module::extractCode(JSContext* cx, Tier tier,
                         MutableHandleValue vp) const {
  RootedPlainObject result(cx, NewBuiltinClassInstance<PlainObject>(cx));
  if (!result) {
    return false;
  }

  // This function is only used for testing purposes so we can simply
  // block on tiered compilation to complete.
  testingBlockOnTier2Complete();

  if (!code_->hasTier(tier)) {
    vp.setNull();
    return true;
  }

  const ModuleSegment& moduleSegment = code_->segment(tier);
  RootedObject code(cx, JS_NewUint8Array(cx, moduleSegment.length()));
  if (!code) {
    return false;
  }

  memcpy(code->as<TypedArrayObject>().dataPointerUnshared(),
         moduleSegment.base(), moduleSegment.length());

  RootedValue value(cx, ObjectValue(*code));
  if (!JS_DefineProperty(cx, result, "code", value, JSPROP_ENUMERATE)) {
    return false;
  }

  RootedObject segments(cx, NewDenseEmptyArray(cx));
  if (!segments) {
    return false;
  }

  for (const CodeRange& p : metadata(tier).codeRanges) {
    RootedObject segment(cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr));
    if (!segment) {
      return false;
    }

    value.setNumber((uint32_t)p.begin());
    if (!JS_DefineProperty(cx, segment, "begin", value, JSPROP_ENUMERATE)) {
      return false;
    }

    value.setNumber((uint32_t)p.end());
    if (!JS_DefineProperty(cx, segment, "end", value, JSPROP_ENUMERATE)) {
      return false;
    }

    value.setNumber((uint32_t)p.kind());
    if (!JS_DefineProperty(cx, segment, "kind", value, JSPROP_ENUMERATE)) {
      return false;
    }

    if (p.isFunction()) {
      value.setNumber((uint32_t)p.funcIndex());
      if (!JS_DefineProperty(cx, segment, "funcIndex", value,
                             JSPROP_ENUMERATE)) {
        return false;
      }

      value.setNumber((uint32_t)p.funcUncheckedCallEntry());
      if (!JS_DefineProperty(cx, segment, "funcBodyBegin", value,
                             JSPROP_ENUMERATE)) {
        return false;
      }

      value.setNumber((uint32_t)p.end());
      if (!JS_DefineProperty(cx, segment, "funcBodyEnd", value,
                             JSPROP_ENUMERATE)) {
        return false;
      }
    }

    if (!NewbornArrayPush(cx, segments, ObjectValue(*segment))) {
      return false;
    }
  }

  value.setObject(*segments);
  if (!JS_DefineProperty(cx, result, "segments", value, JSPROP_ENUMERATE)) {
    return false;
  }

  vp.setObject(*result);
  return true;
}

#ifdef DEBUG
static bool AllSegmentsArePassive(const DataSegmentVector& vec) {
  for (const DataSegment* seg : vec) {
    if (seg->active()) {
      return false;
    }
  }
  return true;
}
#endif

bool Module::initSegments(JSContext* cx, HandleWasmInstanceObject instanceObj,
                          HandleWasmMemoryObject memoryObj,
                          const ValVector& globalImportValues) const {
  MOZ_ASSERT_IF(!memoryObj, AllSegmentsArePassive(dataSegments_));

  Instance& instance = instanceObj->instance();
  const SharedTableVector& tables = instance.tables();

  // Write data/elem segments into memories/tables.

  for (const ElemSegment* seg : elemSegments_) {
    if (seg->active()) {
      RootedVal offsetVal(cx);
      if (!seg->offset().evaluate(cx, globalImportValues, instanceObj,
                                  &offsetVal)) {
        return false;  // OOM
      }
      uint32_t offset = offsetVal.get().i32();
      uint32_t count = seg->length();

      uint32_t tableLength = tables[seg->tableIndex]->length();
      if (offset > tableLength || tableLength - offset < count) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }

      if (!instance.initElems(seg->tableIndex, *seg, offset, 0, count)) {
        return false;  // OOM
      }
    }
  }

  if (memoryObj) {
    size_t memoryLength = memoryObj->volatileMemoryLength();
    uint8_t* memoryBase =
        memoryObj->buffer().dataPointerEither().unwrap(/* memcpy */);

    for (const DataSegment* seg : dataSegments_) {
      if (!seg->active()) {
        continue;
      }

      RootedVal offsetVal(cx);
      if (!seg->offset().evaluate(cx, globalImportValues, instanceObj,
                                  &offsetVal)) {
        return false;  // OOM
      }
      uint32_t offset = offsetVal.get().i32();
      uint32_t count = seg->bytes.length();

      if (offset > memoryLength || memoryLength - offset < count) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }
      memcpy(memoryBase + offset, seg->bytes.begin(), count);
    }
  }

  return true;
}

static const Import& FindImportFunction(const ImportVector& imports,
                                        uint32_t funcImportIndex) {
  for (const Import& import : imports) {
    if (import.kind != DefinitionKind::Function) {
      continue;
    }
    if (funcImportIndex == 0) {
      return import;
    }
    funcImportIndex--;
  }
  MOZ_CRASH("ran out of imports");
}

bool Module::instantiateFunctions(JSContext* cx,
                                  const JSFunctionVector& funcImports) const {
#ifdef DEBUG
  for (auto t : code().tiers()) {
    MOZ_ASSERT(funcImports.length() == metadata(t).funcImports.length());
  }
#endif

  if (metadata().isAsmJS()) {
    return true;
  }

  Tier tier = code().stableTier();

  for (size_t i = 0; i < metadata(tier).funcImports.length(); i++) {
    JSFunction* f = funcImports[i];
    if (!IsWasmExportedFunction(f)) {
      continue;
    }

    uint32_t funcIndex = ExportedFunctionToFuncIndex(f);
    Instance& instance = ExportedFunctionToInstance(f);
    Tier otherTier = instance.code().stableTier();

    const FuncExport& funcExport =
        instance.metadata(otherTier).lookupFuncExport(funcIndex);

    if (funcExport.funcType() != metadata(tier).funcImports[i].funcType()) {
      const Import& import = FindImportFunction(imports_, i);
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_BAD_IMPORT_SIG, import.module.get(),
                               import.field.get());
      return false;
    }
  }

  return true;
}

template <typename T>
static bool CheckLimits(JSContext* cx, T declaredMin,
                        const Maybe<T>& declaredMax, T defaultMax,
                        T actualLength, const Maybe<T>& actualMax, bool isAsmJS,
                        const char* kind) {
  if (isAsmJS) {
    MOZ_ASSERT(actualLength >= declaredMin);
    MOZ_ASSERT(!declaredMax);
    MOZ_ASSERT(actualLength == actualMax.value());
    return true;
  }

  if (actualLength < declaredMin ||
      actualLength > declaredMax.valueOr(defaultMax)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_IMP_SIZE, kind);
    return false;
  }

  if ((actualMax && declaredMax && *actualMax > *declaredMax) ||
      (!actualMax && declaredMax)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_IMP_MAX, kind);
    return false;
  }

  return true;
}

static bool CheckSharing(JSContext* cx, bool declaredShared, bool isShared) {
  if (isShared &&
      !cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_NO_SHMEM_LINK);
    return false;
  }

  if (declaredShared && !isShared) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_IMP_SHARED_REQD);
    return false;
  }

  if (!declaredShared && isShared) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_IMP_SHARED_BANNED);
    return false;
  }

  return true;
}

// asm.js module instantiation supplies its own buffer, but for wasm, create and
// initialize the buffer if one is requested. Either way, the buffer is wrapped
// in a WebAssembly.Memory object which is what the Instance stores.
bool Module::instantiateMemory(JSContext* cx,
                               MutableHandleWasmMemoryObject memory) const {
  if (!metadata().usesMemory()) {
    MOZ_ASSERT(!memory);
    MOZ_ASSERT(AllSegmentsArePassive(dataSegments_));
    return true;
  }

  MemoryDesc desc = *metadata().memory;
  MOZ_ASSERT(desc.kind == MemoryKind::Memory32);

  if (memory) {
    MOZ_ASSERT_IF(metadata().isAsmJS(), memory->buffer().isPreparedForAsmJS());
    MOZ_ASSERT_IF(!metadata().isAsmJS(), memory->buffer().isWasm());

    if (!CheckLimits(cx, desc.initialPages(), desc.maximumPages(),
                     /* defaultMax */ MaxMemory32Pages(),
                     /* actualLength */
                     memory->volatilePages(), memory->maxPages(),
                     metadata().isAsmJS(), "Memory")) {
      return false;
    }

    if (!CheckSharing(cx, desc.isShared(), memory->isShared())) {
      return false;
    }
  } else {
    MOZ_ASSERT(!metadata().isAsmJS());

    if (desc.initialPages() > MaxMemory32Pages()) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_MEM_IMP_LIMIT);
      return false;
    }

    RootedArrayBufferObjectMaybeShared buffer(cx);
    if (!CreateWasmBuffer32(cx, desc, &buffer)) {
      return false;
    }

    RootedObject proto(
        cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
    memory.set(WasmMemoryObject::create(cx, buffer, proto));
    if (!memory) {
      return false;
    }
  }

  MOZ_RELEASE_ASSERT(memory->isHuge() == metadata().omitsBoundsChecks);

  return true;
}

#ifdef ENABLE_WASM_EXCEPTIONS
bool Module::instantiateImportedException(
    JSContext* cx, Handle<WasmExceptionObject*> exnObj,
    WasmExceptionObjectVector& exnObjs, SharedExceptionTagVector* tags) const {
  MOZ_ASSERT(exnObj);
  // The check whether the EventDesc signature matches the exnObj value types
  // is done by js::wasm::GetImports().

  // Collects the exception tag from the imported exception.
  ExceptionTag& tag = exnObj->tag();

  if (!tags->append(&tag)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool Module::instantiateLocalException(JSContext* cx, const EventDesc& ed,
                                       WasmExceptionObjectVector& exnObjs,
                                       SharedExceptionTagVector* tags,
                                       uint32_t exnIndex) const {
  SharedExceptionTag tag;
  // Extend exnObjs in anticipation of an exported exception object.
  if (exnObjs.length() <= exnIndex && !exnObjs.resize(exnIndex + 1)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (ed.isExport) {
    // If the exception description is exported, create an export exception
    // object for it.
    RootedObject proto(
        cx, &cx->global()->getPrototype(JSProto_WasmException).toObject());
    RootedWasmExceptionObject exnObj(
        cx, WasmExceptionObject::create(cx, ed.type, proto));
    if (!exnObj) {
      return false;
    }
    // Take the exception tag that was created inside the WasmExceptionObject.
    tag = &exnObj->tag();
    // Save the new export exception object.
    exnObjs[exnIndex] = exnObj;
  } else {
    // Create a new tag for every non exported exception.
    tag = SharedExceptionTag(cx->new_<ExceptionTag>());
    if (!tag) {
      return false;
    }
    // The exnObj is null if the exception is neither exported nor imported.
  }
  // Collect a tag for every exception.
  if (!tags->emplaceBack(tag)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool Module::instantiateExceptions(JSContext* cx,
                                   WasmExceptionObjectVector& exnObjs,
                                   SharedExceptionTagVector* tags) const {
  uint32_t exnIndex = 0;
  for (const EventDesc& ed : metadata().events) {
    if (exnIndex < exnObjs.length()) {
      Rooted<WasmExceptionObject*> exnObj(cx, exnObjs[exnIndex]);
      if (!instantiateImportedException(cx, exnObj, exnObjs, tags)) {
        return false;
      }
    } else {
      if (!instantiateLocalException(cx, ed, exnObjs, tags, exnIndex)) {
        return false;
      }
    }
    exnIndex++;
  }
  return true;
}
#endif

bool Module::instantiateImportedTable(JSContext* cx, const TableDesc& td,
                                      Handle<WasmTableObject*> tableObj,
                                      WasmTableObjectVector* tableObjs,
                                      SharedTableVector* tables) const {
  MOZ_ASSERT(tableObj);
  MOZ_ASSERT(!metadata().isAsmJS());

  Table& table = tableObj->table();
  if (!CheckLimits(cx, td.initialLength, td.maximumLength,
                   /* declaredMin */ MaxTableLimitField,
                   /* actualLength */ table.length(), table.maximum(),
                   metadata().isAsmJS(), "Table")) {
    return false;
  }

  if (!tables->append(&table)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!tableObjs->append(tableObj)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool Module::instantiateLocalTable(JSContext* cx, const TableDesc& td,
                                   WasmTableObjectVector* tableObjs,
                                   SharedTableVector* tables) const {
  if (td.initialLength > MaxTableLength) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_TABLE_IMP_LIMIT);
    return false;
  }

  SharedTable table;
  Rooted<WasmTableObject*> tableObj(cx);
  if (td.importedOrExported) {
    RootedObject proto(
        cx, &cx->global()->getPrototype(JSProto_WasmTable).toObject());
    tableObj.set(WasmTableObject::create(cx, td.initialLength, td.maximumLength,
                                         td.elemType, proto));
    if (!tableObj) {
      return false;
    }
    table = &tableObj->table();
  } else {
    table = Table::create(cx, td, /* HandleWasmTableObject = */ nullptr);
    if (!table) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Note, appending a null pointer for non-exported local tables.
  if (!tableObjs->append(tableObj.get())) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!tables->emplaceBack(table)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool Module::instantiateTables(JSContext* cx,
                               const WasmTableObjectVector& tableImports,
                               MutableHandle<WasmTableObjectVector> tableObjs,
                               SharedTableVector* tables) const {
  uint32_t tableIndex = 0;
  for (const TableDesc& td : metadata().tables) {
    if (tableIndex < tableImports.length()) {
      Rooted<WasmTableObject*> tableObj(cx, tableImports[tableIndex]);
      if (!instantiateImportedTable(cx, td, tableObj, &tableObjs.get(),
                                    tables)) {
        return false;
      }
    } else {
      if (!instantiateLocalTable(cx, td, &tableObjs.get(), tables)) {
        return false;
      }
    }
    tableIndex++;
  }
  return true;
}

static bool EnsureExportedGlobalObject(JSContext* cx,
                                       const ValVector& globalImportValues,
                                       size_t globalIndex,
                                       const GlobalDesc& global,
                                       WasmGlobalObjectVector& globalObjs) {
  if (globalIndex < globalObjs.length() && globalObjs[globalIndex]) {
    return true;
  }

  RootedVal val(cx);
  if (global.kind() == GlobalKind::Import) {
    // If this is an import, then this must be a constant global that was
    // provided without a global object. We must initialize it with the
    // provided value while we still can differentiate this case.
    MOZ_ASSERT(!global.isMutable());
    val.set(Val(globalImportValues[globalIndex]));
  } else {
    // If this is not an import, then the initial value will be set by
    // Instance::init() for indirect globals or else by CreateExportObject().
    // In either case, we initialize with a default value here.
    val.set(Val(global.type()));
  }

  RootedObject proto(
      cx, &cx->global()->getPrototype(JSProto_WasmGlobal).toObject());
  RootedWasmGlobalObject go(
      cx, WasmGlobalObject::create(cx, val, global.isMutable(), proto));
  if (!go) {
    return false;
  }

  if (globalObjs.length() <= globalIndex &&
      !globalObjs.resize(globalIndex + 1)) {
    ReportOutOfMemory(cx);
    return false;
  }

  globalObjs[globalIndex] = go;
  return true;
}

bool Module::instantiateGlobals(JSContext* cx,
                                const ValVector& globalImportValues,
                                WasmGlobalObjectVector& globalObjs) const {
  // If there are exported globals that aren't in globalObjs because they
  // originate in this module or because they were immutable imports that came
  // in as primitive values then we must create cells in the globalObjs for
  // them here, as WasmInstanceObject::create() and CreateExportObject() will
  // need the cells to exist.

  const GlobalDescVector& globals = metadata().globals;

  for (const Export& exp : exports_) {
    if (exp.kind() != DefinitionKind::Global) {
      continue;
    }
    unsigned globalIndex = exp.globalIndex();
    const GlobalDesc& global = globals[globalIndex];
    if (!EnsureExportedGlobalObject(cx, globalImportValues, globalIndex, global,
                                    globalObjs)) {
      return false;
    }
  }

  // Imported globals that are not re-exported may also have received only a
  // primitive value; these globals are always immutable.  Assert that we do
  // not need to create any additional Global objects for such imports.

#ifdef DEBUG
  size_t numGlobalImports = 0;
  for (const Import& import : imports_) {
    if (import.kind != DefinitionKind::Global) {
      continue;
    }
    size_t globalIndex = numGlobalImports++;
    const GlobalDesc& global = globals[globalIndex];
    MOZ_ASSERT(global.importIndex() == globalIndex);
    MOZ_ASSERT_IF(global.isIndirect(),
                  globalIndex < globalObjs.length() || globalObjs[globalIndex]);
  }
  MOZ_ASSERT_IF(!metadata().isAsmJS(),
                numGlobalImports == globals.length() ||
                    !globals[numGlobalImports].isImport());
#endif
  return true;
}

SharedCode Module::getDebugEnabledCode() const {
  MOZ_ASSERT(metadata().debugEnabled);
  MOZ_ASSERT(debugUnlinkedCode_);
  MOZ_ASSERT(debugLinkData_);

  // The first time through, use the pre-linked code in the module but
  // mark it as having been claimed. Subsequently, instantiate the copy of the
  // code bytes that we keep around for debugging instead, because the
  // debugger may patch the pre-linked code at any time.
  if (debugCodeClaimed_.compareExchange(false, true)) {
    return code_;
  }

  Tier tier = Tier::Baseline;
  auto segment =
      ModuleSegment::create(tier, *debugUnlinkedCode_, *debugLinkData_);
  if (!segment) {
    return nullptr;
  }

  UniqueMetadataTier metadataTier = js::MakeUnique<MetadataTier>(tier);
  if (!metadataTier || !metadataTier->clone(metadata(tier))) {
    return nullptr;
  }

  auto codeTier =
      js::MakeUnique<CodeTier>(std::move(metadataTier), std::move(segment));
  if (!codeTier) {
    return nullptr;
  }

  JumpTables jumpTables;
  if (!jumpTables.init(CompileMode::Once, codeTier->segment(),
                       metadata(tier).codeRanges)) {
    return nullptr;
  }

  MutableCode debugCode =
      js_new<Code>(std::move(codeTier), metadata(), std::move(jumpTables));
  if (!debugCode || !debugCode->initialize(*debugLinkData_)) {
    return nullptr;
  }

  return debugCode;
}

static bool GetFunctionExport(JSContext* cx,
                              HandleWasmInstanceObject instanceObj,
                              const JSFunctionVector& funcImports,
                              uint32_t funcIndex, MutableHandleFunction func) {
  if (funcIndex < funcImports.length() &&
      IsWasmExportedFunction(funcImports[funcIndex])) {
    func.set(funcImports[funcIndex]);
    return true;
  }

  return instanceObj->getExportedFunction(cx, instanceObj, funcIndex, func);
}

static bool GetGlobalExport(JSContext* cx, HandleWasmInstanceObject instanceObj,
                            const JSFunctionVector& funcImports,
                            const GlobalDesc& global, uint32_t globalIndex,
                            const ValVector& globalImportValues,
                            const WasmGlobalObjectVector& globalObjs,
                            MutableHandleValue val) {
  // A global object for this index is guaranteed to exist by
  // instantiateGlobals.
  RootedWasmGlobalObject globalObj(cx, globalObjs[globalIndex]);
  val.setObject(*globalObj);

  // We are responsible to set the initial value of the global object here if
  // it's not imported or indirect. Imported global objects have their initial
  // value set by their defining module, or are set by
  // EnsureExportedGlobalObject when a constant value is provided as an import.
  // Indirect exported globals that are not imported, are initialized in
  // Instance::init.
  if (global.isIndirect() || global.isImport()) {
    return true;
  }

  // This must be an exported immutable global defined in this module. The
  // instance either has compiled the value into the code or has its own copy
  // in its global data area. Either way, we must initialize the global object
  // with the same initial value.
  MOZ_ASSERT(!global.isMutable());
  MOZ_ASSERT(!global.isImport());
  RootedVal globalVal(cx);
  MOZ_RELEASE_ASSERT(!global.isImport());
  const InitExpr& init = global.initExpr();
  if (!init.evaluate(cx, globalImportValues, instanceObj, &globalVal)) {
    return false;
  }
  globalObj->val() = globalVal;
  return true;
}

static bool CreateExportObject(JSContext* cx,
                               HandleWasmInstanceObject instanceObj,
                               const JSFunctionVector& funcImports,
                               const WasmTableObjectVector& tableObjs,
                               HandleWasmMemoryObject memoryObj,
                               const WasmExceptionObjectVector& exceptionObjs,
                               const ValVector& globalImportValues,
                               const WasmGlobalObjectVector& globalObjs,
                               const ExportVector& exports) {
  const Instance& instance = instanceObj->instance();
  const Metadata& metadata = instance.metadata();
  const GlobalDescVector& globals = metadata.globals;

  if (metadata.isAsmJS() && exports.length() == 1 &&
      strlen(exports[0].fieldName()) == 0) {
    RootedFunction func(cx);
    if (!GetFunctionExport(cx, instanceObj, funcImports, exports[0].funcIndex(),
                           &func)) {
      return false;
    }
    instanceObj->initExportsObj(*func.get());
    return true;
  }

  RootedObject exportObj(cx);
  uint8_t propertyAttr = JSPROP_ENUMERATE;

  if (metadata.isAsmJS()) {
    exportObj = NewBuiltinClassInstance<PlainObject>(cx);
  } else {
    exportObj = NewObjectWithGivenProto<PlainObject>(cx, nullptr);
    propertyAttr |= JSPROP_READONLY | JSPROP_PERMANENT;
  }
  if (!exportObj) {
    return false;
  }

  for (const Export& exp : exports) {
    JSAtom* atom =
        AtomizeUTF8Chars(cx, exp.fieldName(), strlen(exp.fieldName()));
    if (!atom) {
      return false;
    }

    RootedId id(cx, AtomToId(atom));
    RootedValue val(cx);
    switch (exp.kind()) {
      case DefinitionKind::Function: {
        RootedFunction func(cx);
        if (!GetFunctionExport(cx, instanceObj, funcImports, exp.funcIndex(),
                               &func)) {
          return false;
        }
        val = ObjectValue(*func);
        break;
      }
      case DefinitionKind::Table: {
        val = ObjectValue(*tableObjs[exp.tableIndex()]);
        break;
      }
      case DefinitionKind::Memory: {
        val = ObjectValue(*memoryObj);
        break;
      }
      case DefinitionKind::Global: {
        const GlobalDesc& global = globals[exp.globalIndex()];
        if (!GetGlobalExport(cx, instanceObj, funcImports, global,
                             exp.globalIndex(), globalImportValues, globalObjs,
                             &val)) {
          return false;
        }
        break;
      }
#ifdef ENABLE_WASM_EXCEPTIONS
      case DefinitionKind::Event: {
        val = ObjectValue(*exceptionObjs[exp.eventIndex()]);
        break;
      }
#endif
    }

    if (!JS_DefinePropertyById(cx, exportObj, id, val, propertyAttr)) {
      return false;
    }
  }

  if (!metadata.isAsmJS()) {
    if (!PreventExtensions(cx, exportObj)) {
      return false;
    }
  }

  instanceObj->initExportsObj(*exportObj);
  return true;
}

bool Module::instantiate(JSContext* cx, ImportValues& imports,
                         HandleObject instanceProto,
                         MutableHandleWasmInstanceObject instance) const {
  MOZ_RELEASE_ASSERT(cx->wasm().haveSignalHandlers);

  if (!instantiateFunctions(cx, imports.funcs)) {
    return false;
  }

  RootedWasmMemoryObject memory(cx, imports.memory);
  if (!instantiateMemory(cx, &memory)) {
    return false;
  }

  // Note that the following will extend imports.exceptionObjs with wrappers for
  // the local (non-imported) exceptions of the module.
  // The resulting vector is sparse, i.e., it will be null in slots that contain
  // exceptions that are neither exported or imported.
  // On the contrary, all the slots of exceptionTags will be filled with
  // unique tags.

  SharedExceptionTagVector tags;
#ifdef ENABLE_WASM_EXCEPTIONS
  if (!instantiateExceptions(cx, imports.exceptionObjs, &tags)) {
    return false;
  }
#endif

  // Note that tableObjs is sparse: it will be null in slots that contain
  // tables that are neither exported nor imported.

  Rooted<WasmTableObjectVector> tableObjs(cx);
  SharedTableVector tables;
  if (!instantiateTables(cx, imports.tables, &tableObjs, &tables)) {
    return false;
  }

  if (!instantiateGlobals(cx, imports.globalValues, imports.globalObjs)) {
    return false;
  }

  UniqueTlsData tlsData = CreateTlsData(metadata().globalDataLength);
  if (!tlsData) {
    ReportOutOfMemory(cx);
    return false;
  }

  SharedCode code;
  UniqueDebugState maybeDebug;
  if (metadata().debugEnabled) {
    code = getDebugEnabledCode();
    if (!code) {
      ReportOutOfMemory(cx);
      return false;
    }

    maybeDebug = cx->make_unique<DebugState>(*code, *this);
    if (!maybeDebug) {
      return false;
    }
  } else {
    code = code_;
  }

  instance.set(WasmInstanceObject::create(
      cx, code, dataSegments_, elemSegments_, std::move(tlsData), memory,
      std::move(tags), std::move(tables), imports.funcs, metadata().globals,
      imports.globalValues, imports.globalObjs, instanceProto,
      std::move(maybeDebug)));
  if (!instance) {
    return false;
  }

  if (!CreateExportObject(cx, instance, imports.funcs, tableObjs.get(), memory,
                          imports.exceptionObjs, imports.globalValues,
                          imports.globalObjs, exports_)) {
    return false;
  }

  // Register the instance with the Realm so that it can find out about global
  // events like profiling being enabled in the realm. Registration does not
  // require a fully-initialized instance and must precede initSegments as the
  // final pre-requisite for a live instance.

  if (!cx->realm()->wasm.registerInstance(cx, instance)) {
    return false;
  }

  // Perform initialization as the final step after the instance is fully
  // constructed since this can make the instance live to content (even if the
  // start function fails).

  if (!initSegments(cx, instance, memory, imports.globalValues)) {
    return false;
  }

  // Now that the instance is fully live and initialized, the start function.
  // Note that failure may cause instantiation to throw, but the instance may
  // still be live via edges created by initSegments or the start function.

  if (metadata().startFuncIndex) {
    FixedInvokeArgs<0> args(cx);
    if (!instance->instance().callExport(cx, *metadata().startFuncIndex,
                                         args)) {
      return false;
    }
  }

  JSUseCounter useCounter =
      metadata().isAsmJS() ? JSUseCounter::ASMJS : JSUseCounter::WASM;
  cx->runtime()->setUseCounter(instance, useCounter);

  if (metadata().usesDuplicateImports) {
    cx->runtime()->setUseCounter(instance,
                                 JSUseCounter::WASM_DUPLICATE_IMPORTS);
  }

  if (cx->options().testWasmAwaitTier2()) {
    testingBlockOnTier2Complete();
  }

  return true;
}
