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

#include "wasm/WasmInstance.h"

#include "jit/AtomicOperations.h"
#include "jit/BaselineJIT.h"
#include "jit/InlinableNatives.h"
#include "jit/JitCommon.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmModule.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using mozilla::BitwiseCast;

class SigIdSet
{
    typedef HashMap<const Sig*, uint32_t, SigHashPolicy, SystemAllocPolicy> Map;
    Map map_;

  public:
    ~SigIdSet() {
        MOZ_ASSERT_IF(!JSRuntime::hasLiveRuntimes(), !map_.initialized() || map_.empty());
    }

    bool ensureInitialized(JSContext* cx) {
        if (!map_.initialized() && !map_.init()) {
            ReportOutOfMemory(cx);
            return false;
        }

        return true;
    }

    bool allocateSigId(JSContext* cx, const Sig& sig, const void** sigId) {
        Map::AddPtr p = map_.lookupForAdd(sig);
        if (p) {
            MOZ_ASSERT(p->value() > 0);
            p->value()++;
            *sigId = p->key();
            return true;
        }

        UniquePtr<Sig> clone = MakeUnique<Sig>();
        if (!clone || !clone->clone(sig) || !map_.add(p, clone.get(), 1)) {
            ReportOutOfMemory(cx);
            return false;
        }

        *sigId = clone.release();
        MOZ_ASSERT(!(uintptr_t(*sigId) & SigIdDesc::ImmediateBit));
        return true;
    }

    void deallocateSigId(const Sig& sig, const void* sigId) {
        Map::Ptr p = map_.lookup(sig);
        MOZ_RELEASE_ASSERT(p && p->key() == sigId && p->value() > 0);

        p->value()--;
        if (!p->value()) {
            js_delete(p->key());
            map_.remove(p);
        }
    }
};

ExclusiveData<SigIdSet>* sigIdSet = nullptr;

bool
js::wasm::InitInstanceStaticData()
{
    MOZ_ASSERT(!sigIdSet);
    sigIdSet = js_new<ExclusiveData<SigIdSet>>(mutexid::WasmSigIdSet);
    return sigIdSet != nullptr;
}

void
js::wasm::ShutDownInstanceStaticData()
{
    MOZ_ASSERT(sigIdSet);
    js_delete(sigIdSet);
    sigIdSet = nullptr;
}

const void**
Instance::addressOfSigId(const SigIdDesc& sigId) const
{
    return (const void**)(globalData() + sigId.globalDataOffset());
}

FuncImportTls&
Instance::funcImportTls(const FuncImport& fi)
{
    return *(FuncImportTls*)(globalData() + fi.tlsDataOffset());
}

TableTls&
Instance::tableTls(const TableDesc& td) const
{
    return *(TableTls*)(globalData() + td.globalDataOffset);
}

bool
Instance::callImport(JSContext* cx, uint32_t funcImportIndex, unsigned argc, const uint64_t* argv,
                     MutableHandleValue rval)
{
    Tier tier = code().bestTier();

    const FuncImport& fi = metadata(tier).funcImports[funcImportIndex];

    InvokeArgs args(cx);
    if (!args.init(cx, argc))
        return false;

    if (fi.sig().hasI64ArgOrRet()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_I64_TYPE);
        return false;
    }

    MOZ_ASSERT(fi.sig().args().length() == argc);
    for (size_t i = 0; i < argc; i++) {
        switch (fi.sig().args()[i]) {
          case ValType::I32:
            args[i].set(Int32Value(*(int32_t*)&argv[i]));
            break;
          case ValType::F32:
            args[i].set(JS::CanonicalizedDoubleValue(*(float*)&argv[i]));
            break;
          case ValType::F64:
            args[i].set(JS::CanonicalizedDoubleValue(*(double*)&argv[i]));
            break;
          case ValType::I64:
          case ValType::I8x16:
          case ValType::I16x8:
          case ValType::I32x4:
          case ValType::F32x4:
          case ValType::B8x16:
          case ValType::B16x8:
          case ValType::B32x4:
            MOZ_CRASH("unhandled type in callImport");
        }
    }

    FuncImportTls& import = funcImportTls(fi);
    RootedFunction importFun(cx, &import.obj->as<JSFunction>());
    RootedValue fval(cx, ObjectValue(*import.obj));
    RootedValue thisv(cx, UndefinedValue());
    if (!Call(cx, fval, thisv, args, rval))
        return false;

    // The import may already have become optimized.
    for (auto t : code().tiers()) {
        void* jitExitCode = codeBase(t) + fi.jitExitCodeOffset();
        if (import.code == jitExitCode)
            return true;
    }

    void* jitExitCode = codeBase(tier) + fi.jitExitCodeOffset();

    // Test if the function is JIT compiled.
    if (!importFun->hasScript())
        return true;

    JSScript* script = importFun->nonLazyScript();
    if (!script->hasBaselineScript()) {
        MOZ_ASSERT(!script->hasIonScript());
        return true;
    }

    // Don't enable jit entry when we have a pending ion builder.
    // Take the interpreter path which will link it and enable
    // the fast path on the next call.
    if (script->baselineScript()->hasPendingIonBuilder())
        return true;

    // Ensure the argument types are included in the argument TypeSets stored in
    // the TypeScript. This is necessary for Ion, because the import will use
    // the skip-arg-checks entry point.
    //
    // Note that the TypeScript is never discarded while the script has a
    // BaselineScript, so if those checks hold now they must hold at least until
    // the BaselineScript is discarded and when that happens the import is
    // patched back.
    if (!TypeScript::ThisTypes(script)->hasType(TypeSet::UndefinedType()))
        return true;

    const ValTypeVector& importArgs = fi.sig().args();

    size_t numKnownArgs = Min(importArgs.length(), importFun->nargs());
    for (uint32_t i = 0; i < numKnownArgs; i++) {
        TypeSet::Type type = TypeSet::UnknownType();
        switch (importArgs[i]) {
          case ValType::I32:   type = TypeSet::Int32Type(); break;
          case ValType::F32:   type = TypeSet::DoubleType(); break;
          case ValType::F64:   type = TypeSet::DoubleType(); break;
          case ValType::I64:   MOZ_CRASH("NYI");
          case ValType::I8x16: MOZ_CRASH("NYI");
          case ValType::I16x8: MOZ_CRASH("NYI");
          case ValType::I32x4: MOZ_CRASH("NYI");
          case ValType::F32x4: MOZ_CRASH("NYI");
          case ValType::B8x16: MOZ_CRASH("NYI");
          case ValType::B16x8: MOZ_CRASH("NYI");
          case ValType::B32x4: MOZ_CRASH("NYI");
        }
        if (!TypeScript::ArgTypes(script, i)->hasType(type))
            return true;
    }

    // These arguments will be filled with undefined at runtime by the
    // arguments rectifier: check that the imported function can handle
    // undefined there.
    for (uint32_t i = importArgs.length(); i < importFun->nargs(); i++) {
        if (!TypeScript::ArgTypes(script, i)->hasType(TypeSet::UndefinedType()))
            return true;
    }

    // Let's optimize it!
    if (!script->baselineScript()->addDependentWasmImport(cx, *this, funcImportIndex))
        return false;

    import.code = jitExitCode;
    import.baselineScript = script->baselineScript();
    return true;
}

/* static */ int32_t
Instance::callImport_void(Instance* instance, int32_t funcImportIndex, int32_t argc, uint64_t* argv)
{
    JSContext* cx = TlsContext.get();
    RootedValue rval(cx);
    return instance->callImport(cx, funcImportIndex, argc, argv, &rval);
}

/* static */ int32_t
Instance::callImport_i32(Instance* instance, int32_t funcImportIndex, int32_t argc, uint64_t* argv)
{
    JSContext* cx = TlsContext.get();
    RootedValue rval(cx);
    if (!instance->callImport(cx, funcImportIndex, argc, argv, &rval))
        return false;

    return ToInt32(cx, rval, (int32_t*)argv);
}

/* static */ int32_t
Instance::callImport_i64(Instance* instance, int32_t funcImportIndex, int32_t argc, uint64_t* argv)
{
    JSContext* cx = TlsContext.get();
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_I64_TYPE);
    return false;
}

/* static */ int32_t
Instance::callImport_f64(Instance* instance, int32_t funcImportIndex, int32_t argc, uint64_t* argv)
{
    JSContext* cx = TlsContext.get();
    RootedValue rval(cx);
    if (!instance->callImport(cx, funcImportIndex, argc, argv, &rval))
        return false;

    return ToNumber(cx, rval, (double*)argv);
}

/* static */ uint32_t
Instance::growMemory_i32(Instance* instance, uint32_t delta)
{
    MOZ_ASSERT(!instance->isAsmJS());

    JSContext* cx = TlsContext.get();
    RootedWasmMemoryObject memory(cx, instance->memory_);

    uint32_t ret = WasmMemoryObject::grow(memory, delta, cx);

    // If there has been a moving grow, this Instance should have been notified.
    MOZ_RELEASE_ASSERT(instance->tlsData()->memoryBase ==
                       instance->memory_->buffer().dataPointerEither());

    return ret;
}

/* static */ uint32_t
Instance::currentMemory_i32(Instance* instance)
{
    uint32_t byteLength = instance->memory()->volatileMemoryLength();
    MOZ_ASSERT(byteLength % wasm::PageSize == 0);
    return byteLength / wasm::PageSize;
}

template<typename T>
static int32_t
PerformWait(Instance* instance, uint32_t byteOffset, T value, int64_t timeout_ns)
{
    JSContext* cx = TlsContext.get();

    if (byteOffset & (sizeof(T) - 1)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_UNALIGNED_ACCESS);
        return -1;
    }

    if (byteOffset + sizeof(T) > instance->memory()->volatileMemoryLength()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_OUT_OF_BOUNDS);
        return -1;
    }

    mozilla::Maybe<mozilla::TimeDuration> timeout;
    if (timeout_ns >= 0)
        timeout = mozilla::Some(mozilla::TimeDuration::FromMicroseconds(timeout_ns / 1000));

    switch (atomics_wait_impl(cx, instance->sharedMemoryBuffer(), byteOffset, value, timeout)) {
      case FutexThread::WaitResult::OK:       return 0;
      case FutexThread::WaitResult::NotEqual: return 1;
      case FutexThread::WaitResult::TimedOut: return 2;
      case FutexThread::WaitResult::Error:    return -1;
      default: MOZ_CRASH();
    }
}

/* static */ int32_t
Instance::wait_i32(Instance* instance, uint32_t byteOffset, int32_t value, int64_t timeout_ns)
{
    return PerformWait<int32_t>(instance, byteOffset, value, timeout_ns);
}

/* static */ int32_t
Instance::wait_i64(Instance* instance, uint32_t byteOffset, int64_t value, int64_t timeout_ns)
{
    return PerformWait<int64_t>(instance, byteOffset, value, timeout_ns);
}

/* static */ int32_t
Instance::wake(Instance* instance, uint32_t byteOffset, int32_t count)
{
    JSContext* cx = TlsContext.get();

    // The alignment guard is not in the wasm spec as of 2017-11-02, but is
    // considered likely to appear, as 4-byte alignment is required for WAKE by
    // the spec's validation algorithm.

    if (byteOffset & 3) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_UNALIGNED_ACCESS);
        return -1;
    }

    if (byteOffset >= instance->memory()->volatileMemoryLength()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_OUT_OF_BOUNDS);
        return -1;
    }

    int64_t woken = atomics_wake_impl(instance->sharedMemoryBuffer(), byteOffset, int64_t(count));

    if (woken > INT32_MAX) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_WAKE_OVERFLOW);
        return -1;
    }

    return int32_t(woken);
}

Instance::Instance(JSContext* cx,
                   Handle<WasmInstanceObject*> object,
                   SharedCode code,
                   UniqueDebugState debug,
                   UniqueTlsData tlsDataIn,
                   HandleWasmMemoryObject memory,
                   SharedTableVector&& tables,
                   Handle<FunctionVector> funcImports,
                   const ValVector& globalImports)
  : compartment_(cx->compartment()),
    object_(object),
    code_(code),
    debug_(Move(debug)),
    tlsData_(Move(tlsDataIn)),
    memory_(memory),
    tables_(Move(tables)),
    enterFrameTrapsEnabled_(false)
{
#ifdef DEBUG
    for (auto t : code_->tiers())
        MOZ_ASSERT(funcImports.length() == metadata(t).funcImports.length());
#endif
    MOZ_ASSERT(tables_.length() == metadata().tables.length());

    tlsData()->memoryBase = memory ? memory->buffer().dataPointerEither().unwrap() : nullptr;
#ifndef WASM_HUGE_MEMORY
    tlsData()->boundsCheckLimit = memory ? memory->buffer().wasmBoundsCheckLimit() : 0;
#endif
    tlsData()->instance = this;
    tlsData()->cx = cx;
    tlsData()->stackLimit = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
    tlsData()->jumpTable = code_->tieringJumpTable();

    Tier callerTier = code_->bestTier();

    for (size_t i = 0; i < metadata(callerTier).funcImports.length(); i++) {
        HandleFunction f = funcImports[i];
        const FuncImport& fi = metadata(callerTier).funcImports[i];
        FuncImportTls& import = funcImportTls(fi);
        if (!isAsmJS() && IsExportedWasmFunction(f)) {
            WasmInstanceObject* calleeInstanceObj = ExportedFunctionToInstanceObject(f);
            Instance& calleeInstance = calleeInstanceObj->instance();
            Tier calleeTier = calleeInstance.code().bestTier();
            const CodeRange& codeRange = calleeInstanceObj->getExportedFunctionCodeRange(f, calleeTier);
            import.tls = calleeInstance.tlsData();
            import.code = calleeInstance.codeBase(calleeTier) + codeRange.funcNormalEntry();
            import.baselineScript = nullptr;
            import.obj = calleeInstanceObj;
        } else if (void* thunk = MaybeGetBuiltinThunk(f, fi.sig())) {
            import.tls = tlsData();
            import.code = thunk;
            import.baselineScript = nullptr;
            import.obj = f;
        } else {
            import.tls = tlsData();
            import.code = codeBase(callerTier) + fi.interpExitCodeOffset();
            import.baselineScript = nullptr;
            import.obj = f;
        }
    }

    for (size_t i = 0; i < tables_.length(); i++) {
        const TableDesc& td = metadata().tables[i];
        TableTls& table = tableTls(td);
        table.length = tables_[i]->length();
        table.base = tables_[i]->base();
    }

    for (size_t i = 0; i < metadata().globals.length(); i++) {
        const GlobalDesc& global = metadata().globals[i];
        if (global.isConstant())
            continue;

        uint8_t* globalAddr = globalData() + global.offset();
        switch (global.kind()) {
          case GlobalKind::Import: {
            globalImports[global.importIndex()].writePayload(globalAddr);
            break;
          }
          case GlobalKind::Variable: {
            const InitExpr& init = global.initExpr();
            switch (init.kind()) {
              case InitExpr::Kind::Constant: {
                init.val().writePayload(globalAddr);
                break;
              }
              case InitExpr::Kind::GetGlobal: {
                const GlobalDesc& imported = metadata().globals[init.globalIndex()];
                globalImports[imported.importIndex()].writePayload(globalAddr);
                break;
              }
            }
            break;
          }
          case GlobalKind::Constant: {
            MOZ_CRASH("skipped at the top");
          }
        }
    }
}

bool
Instance::init(JSContext* cx)
{
    if (memory_ && memory_->movingGrowable() && !memory_->addMovingGrowObserver(cx, object_))
        return false;

    for (const SharedTable& table : tables_) {
        if (table->movingGrowable() && !table->addMovingGrowObserver(cx, object_))
            return false;
    }

    if (!metadata().sigIds.empty()) {
        ExclusiveData<SigIdSet>::Guard lockedSigIdSet = sigIdSet->lock();

        if (!lockedSigIdSet->ensureInitialized(cx))
            return false;

        for (const SigWithId& sig : metadata().sigIds) {
            const void* sigId;
            if (!lockedSigIdSet->allocateSigId(cx, sig, &sigId))
                return false;

            *addressOfSigId(sig.id) = sigId;
        }
    }

    JitRuntime* jitRuntime = cx->runtime()->getJitRuntime(cx);
    if (!jitRuntime)
        return false;
    jsJitArgsRectifier_ = jitRuntime->getArgumentsRectifier();
    jsJitExceptionHandler_ = jitRuntime->getExceptionTail();
    return true;
}

Instance::~Instance()
{
    compartment_->wasm.unregisterInstance(*this);

    const FuncImportVector& funcImports = metadata(code().stableTier()).funcImports;

    for (unsigned i = 0; i < funcImports.length(); i++) {
        FuncImportTls& import = funcImportTls(funcImports[i]);
        if (import.baselineScript)
            import.baselineScript->removeDependentWasmImport(*this, i);
    }

    if (!metadata().sigIds.empty()) {
        ExclusiveData<SigIdSet>::Guard lockedSigIdSet = sigIdSet->lock();

        for (const SigWithId& sig : metadata().sigIds) {
            if (const void* sigId = *addressOfSigId(sig.id))
                lockedSigIdSet->deallocateSigId(sig, sigId);
        }
    }
}

size_t
Instance::memoryMappedSize() const
{
    return memory_->buffer().wasmMappedSize();
}

#ifdef JS_SIMULATOR
bool
Instance::memoryAccessInGuardRegion(uint8_t* addr, unsigned numBytes) const
{
    MOZ_ASSERT(numBytes > 0);

    if (!metadata().usesMemory())
        return false;

    uint8_t* base = memoryBase().unwrap(/* comparison */);
    if (addr < base)
        return false;

    size_t lastByteOffset = addr - base + (numBytes - 1);
    return lastByteOffset >= memory()->volatileMemoryLength() && lastByteOffset < memoryMappedSize();
}
#endif

void
Instance::tracePrivate(JSTracer* trc)
{
    // This method is only called from WasmInstanceObject so the only reason why
    // TraceEdge is called is so that the pointer can be updated during a moving
    // GC. TraceWeakEdge may sound better, but it is less efficient given that
    // we know object_ is already marked.
    MOZ_ASSERT(!gc::IsAboutToBeFinalized(&object_));
    TraceEdge(trc, &object_, "wasm instance object");

    // OK to just do one tier here; though the tiers have different funcImports
    // tables, they share the tls object.
    for (const FuncImport& fi : metadata(code().stableTier()).funcImports)
        TraceNullableEdge(trc, &funcImportTls(fi).obj, "wasm import");

    for (const SharedTable& table : tables_)
        table->trace(trc);

    TraceNullableEdge(trc, &memory_, "wasm buffer");
}

void
Instance::trace(JSTracer* trc)
{
    // Technically, instead of having this method, the caller could use
    // Instance::object() to get the owning WasmInstanceObject to mark,
    // but this method is simpler and more efficient. The trace hook of
    // WasmInstanceObject will call Instance::tracePrivate at which point we
    // can mark the rest of the children.
    TraceEdge(trc, &object_, "wasm instance object");
}

WasmMemoryObject*
Instance::memory() const
{
    return memory_;
}

SharedMem<uint8_t*>
Instance::memoryBase() const
{
    MOZ_ASSERT(metadata().usesMemory());
    MOZ_ASSERT(tlsData()->memoryBase == memory_->buffer().dataPointerEither());
    return memory_->buffer().dataPointerEither();
}

SharedArrayRawBuffer*
Instance::sharedMemoryBuffer() const
{
    MOZ_ASSERT(memory_->isShared());
    return memory_->sharedArrayRawBuffer();
}

WasmInstanceObject*
Instance::objectUnbarriered() const
{
    return object_.unbarrieredGet();
}

WasmInstanceObject*
Instance::object() const
{
    return object_;
}

bool
Instance::callExport(JSContext* cx, uint32_t funcIndex, CallArgs args)
{
    // If there has been a moving grow, this Instance should have been notified.
    MOZ_RELEASE_ASSERT(!memory_ || tlsData()->memoryBase == memory_->buffer().dataPointerEither());

    Tier tier = code().bestTier();

    const FuncExport& func = metadata(tier).lookupFuncExport(funcIndex);

    if (func.sig().hasI64ArgOrRet()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_I64_TYPE);
        return false;
    }

    // The calling convention for an external call into wasm is to pass an
    // array of 16-byte values where each value contains either a coerced int32
    // (in the low word), a double value (in the low dword) or a SIMD vector
    // value, with the coercions specified by the wasm signature. The external
    // entry point unpacks this array into the system-ABI-specified registers
    // and stack memory and then calls into the internal entry point. The return
    // value is stored in the first element of the array (which, therefore, must
    // have length >= 1).
    Vector<ExportArg, 8> exportArgs(cx);
    if (!exportArgs.resize(Max<size_t>(1, func.sig().args().length())))
        return false;

    RootedValue v(cx);
    for (unsigned i = 0; i < func.sig().args().length(); ++i) {
        v = i < args.length() ? args[i] : UndefinedValue();
        switch (func.sig().arg(i)) {
          case ValType::I32:
            if (!ToInt32(cx, v, (int32_t*)&exportArgs[i]))
                return false;
            break;
          case ValType::I64:
            MOZ_CRASH("unexpected i64 flowing into callExport");
          case ValType::F32:
            if (!RoundFloat32(cx, v, (float*)&exportArgs[i]))
                return false;
            break;
          case ValType::F64:
            if (!ToNumber(cx, v, (double*)&exportArgs[i]))
                return false;
            break;
          case ValType::I8x16: {
            SimdConstant simd;
            if (!ToSimdConstant<Int8x16>(cx, v, &simd))
                return false;
            memcpy(&exportArgs[i], simd.asInt8x16(), Simd128DataSize);
            break;
          }
          case ValType::I16x8: {
            SimdConstant simd;
            if (!ToSimdConstant<Int16x8>(cx, v, &simd))
                return false;
            memcpy(&exportArgs[i], simd.asInt16x8(), Simd128DataSize);
            break;
          }
          case ValType::I32x4: {
            SimdConstant simd;
            if (!ToSimdConstant<Int32x4>(cx, v, &simd))
                return false;
            memcpy(&exportArgs[i], simd.asInt32x4(), Simd128DataSize);
            break;
          }
          case ValType::F32x4: {
            SimdConstant simd;
            if (!ToSimdConstant<Float32x4>(cx, v, &simd))
                return false;
            memcpy(&exportArgs[i], simd.asFloat32x4(), Simd128DataSize);
            break;
          }
          case ValType::B8x16: {
            SimdConstant simd;
            if (!ToSimdConstant<Bool8x16>(cx, v, &simd))
                return false;
            // Bool8x16 uses the same representation as Int8x16.
            memcpy(&exportArgs[i], simd.asInt8x16(), Simd128DataSize);
            break;
          }
          case ValType::B16x8: {
            SimdConstant simd;
            if (!ToSimdConstant<Bool16x8>(cx, v, &simd))
                return false;
            // Bool16x8 uses the same representation as Int16x8.
            memcpy(&exportArgs[i], simd.asInt16x8(), Simd128DataSize);
            break;
          }
          case ValType::B32x4: {
            SimdConstant simd;
            if (!ToSimdConstant<Bool32x4>(cx, v, &simd))
                return false;
            // Bool32x4 uses the same representation as Int32x4.
            memcpy(&exportArgs[i], simd.asInt32x4(), Simd128DataSize);
            break;
          }
        }
    }

    {
        JitActivation activation(cx);

        void* callee;
        if (func.hasEagerStubs())
            callee = codeBase(tier) + func.eagerInterpEntryOffset();
        else
            callee = code(tier).lazyStubs().lock()->lookupInterpEntry(funcIndex);

        // Call the per-exported-function trampoline created by GenerateEntry.
        auto funcPtr = JS_DATA_TO_FUNC_PTR(ExportFuncPtr, callee);
        if (!CALL_GENERATED_2(funcPtr, exportArgs.begin(), tlsData()))
            return false;
    }

    if (isAsmJS() && args.isConstructing()) {
        // By spec, when a JS function is called as a constructor and this
        // function returns a primary type, which is the case for all asm.js
        // exported functions, the returned value is discarded and an empty
        // object is returned instead.
        PlainObject* obj = NewBuiltinClassInstance<PlainObject>(cx);
        if (!obj)
            return false;
        args.rval().set(ObjectValue(*obj));
        return true;
    }

    void* retAddr = &exportArgs[0];
    JSObject* retObj = nullptr;
    switch (func.sig().ret()) {
      case ExprType::Void:
        args.rval().set(UndefinedValue());
        break;
      case ExprType::I32:
        args.rval().set(Int32Value(*(int32_t*)retAddr));
        break;
      case ExprType::I64:
        MOZ_CRASH("unexpected i64 flowing from callExport");
      case ExprType::F32:
        args.rval().set(NumberValue(*(float*)retAddr));
        break;
      case ExprType::F64:
        args.rval().set(NumberValue(*(double*)retAddr));
        break;
      case ExprType::I8x16:
        retObj = CreateSimd<Int8x16>(cx, (int8_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::I16x8:
        retObj = CreateSimd<Int16x8>(cx, (int16_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::I32x4:
        retObj = CreateSimd<Int32x4>(cx, (int32_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::F32x4:
        retObj = CreateSimd<Float32x4>(cx, (float*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::B8x16:
        retObj = CreateSimd<Bool8x16>(cx, (int8_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::B16x8:
        retObj = CreateSimd<Bool16x8>(cx, (int16_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::B32x4:
        retObj = CreateSimd<Bool32x4>(cx, (int32_t*)retAddr);
        if (!retObj)
            return false;
        break;
      case ExprType::Limit:
        MOZ_CRASH("Limit");
    }

    if (retObj)
        args.rval().set(ObjectValue(*retObj));

    return true;
}

bool
Instance::getFuncName(uint32_t funcIndex, UTF8Bytes* name) const
{
    return metadata().getFuncName(debug_->maybeBytecode(), funcIndex, name);
}

JSAtom*
Instance::getFuncAtom(JSContext* cx, uint32_t funcIndex) const
{
    UTF8Bytes name;
    if (!getFuncName(funcIndex, &name))
        return nullptr;

    return AtomizeUTF8Chars(cx, name.begin(), name.length());
}

void
Instance::ensureProfilingLabels(bool profilingEnabled) const
{
    return code_->ensureProfilingLabels(debug_->maybeBytecode(), profilingEnabled);
}

void
Instance::onMovingGrowMemory(uint8_t* prevMemoryBase)
{
    MOZ_ASSERT(!isAsmJS());
    MOZ_ASSERT(!memory_->isShared());

    ArrayBufferObject& buffer = memory_->buffer().as<ArrayBufferObject>();
    tlsData()->memoryBase = buffer.dataPointer();
#ifndef WASM_HUGE_MEMORY
    tlsData()->boundsCheckLimit = buffer.wasmBoundsCheckLimit();
#endif
}

void
Instance::onMovingGrowTable()
{
    MOZ_ASSERT(!isAsmJS());
    MOZ_ASSERT(tables_.length() == 1);
    TableTls& table = tableTls(metadata().tables[0]);
    table.length = tables_[0]->length();
    table.base = tables_[0]->base();
}

void
Instance::deoptimizeImportExit(uint32_t funcImportIndex)
{
    Tier t = code().bestTier();
    const FuncImport& fi = metadata(t).funcImports[funcImportIndex];
    FuncImportTls& import = funcImportTls(fi);
    import.code = codeBase(t) + fi.interpExitCodeOffset();
    import.baselineScript = nullptr;
}

void
Instance::ensureEnterFrameTrapsState(JSContext* cx, bool enabled)
{
    if (enterFrameTrapsEnabled_ == enabled)
        return;

    debug_->adjustEnterAndLeaveFrameTrapsState(cx, enabled);
    enterFrameTrapsEnabled_ = enabled;
}

void
Instance::addSizeOfMisc(MallocSizeOf mallocSizeOf,
                        Metadata::SeenSet* seenMetadata,
                        ShareableBytes::SeenSet* seenBytes,
                        Code::SeenSet* seenCode,
                        Table::SeenSet* seenTables,
                        size_t* code,
                        size_t* data) const
{
    *data += mallocSizeOf(this);
    *data += mallocSizeOf(tlsData_.get());
    for (const SharedTable& table : tables_)
         *data += table->sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenTables);

    debug_->addSizeOfMisc(mallocSizeOf, seenMetadata, seenBytes, seenCode, code, data);
    code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenMetadata, seenCode, code, data);
}
