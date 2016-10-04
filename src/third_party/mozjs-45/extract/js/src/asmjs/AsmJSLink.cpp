/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
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

#include "asmjs/AsmJSLink.h"

#include "mozilla/PodOperations.h"

#ifdef MOZ_VTUNE
# include "vtune/VTuneWrapper.h"
#endif

#include "jscntxt.h"
#include "jsmath.h"
#include "jsprf.h"
#include "jswrapper.h"

#include "asmjs/AsmJSModule.h"
#include "builtin/AtomicsObject.h"
#include "builtin/SIMD.h"
#include "frontend/BytecodeCompiler.h"
#include "jit/Ion.h"
#include "jit/JitCommon.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "vm/ArrayBufferObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::IsNaN;
using mozilla::PodZero;

static bool
CloneModule(JSContext* cx, MutableHandle<AsmJSModuleObject*> moduleObj)
{
    ScopedJSDeletePtr<AsmJSModule> module;
    if (!moduleObj->module().clone(cx, &module))
        return false;

    AsmJSModuleObject* newModuleObj = AsmJSModuleObject::create(cx, &module);
    if (!newModuleObj)
        return false;

    moduleObj.set(newModuleObj);
    return true;
}

static bool
LinkFail(JSContext* cx, const char* str)
{
    JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING, GetErrorMessage,
                                 nullptr, JSMSG_USE_ASM_LINK_FAIL, str);
    return false;
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, HandlePropertyName field, MutableHandleValue v)
{
    if (!objVal.isObject())
        return LinkFail(cx, "accessing property of non-object");

    RootedObject obj(cx, &objVal.toObject());
    if (IsScriptedProxy(obj))
        return LinkFail(cx, "accessing property of a Proxy");

    Rooted<PropertyDescriptor> desc(cx);
    RootedId id(cx, NameToId(field));
    if (!GetPropertyDescriptor(cx, obj, id, &desc))
        return false;

    if (!desc.object())
        return LinkFail(cx, "property not present on object");

    if (!desc.isDataDescriptor())
        return LinkFail(cx, "property is not a data property");

    v.set(desc.value());
    return true;
}

static bool
HasPureCoercion(JSContext* cx, HandleValue v)
{
    if (IsVectorObject<Int32x4>(v) || IsVectorObject<Float32x4>(v))
        return true;

    // Ideally, we'd reject all non-SIMD non-primitives, but Emscripten has a
    // bug that generates code that passes functions for some imports. To avoid
    // breaking all the code that contains this bug, we make an exception for
    // functions that don't have user-defined valueOf or toString, for their
    // coercions are not observable and coercion via ToNumber/ToInt32
    // definitely produces NaN/0. We should remove this special case later once
    // most apps have been built with newer Emscripten.
    jsid toString = NameToId(cx->names().toString);
    if (v.toObject().is<JSFunction>() &&
        HasObjectValueOf(&v.toObject(), cx) &&
        ClassMethodIsNative(cx, &v.toObject().as<JSFunction>(), &JSFunction::class_, toString, fun_toString))
    {
        return true;
    }

    return false;
}

static bool
ValidateGlobalVariable(JSContext* cx, const AsmJSModule& module, AsmJSModule::Global& global,
                       HandleValue importVal)
{
    void* datum = module.globalData() + global.varGlobalDataOffset();

    switch (global.varInitKind()) {
      case AsmJSModule::Global::InitConstant: {
        Val v = global.varInitVal();
        switch (v.type()) {
          case ValType::I32:
            *(int32_t*)datum = v.i32();
            break;
          case ValType::I64:
            MOZ_CRASH("int64");
          case ValType::F32:
            *(float*)datum = v.f32();
            break;
          case ValType::F64:
            *(double*)datum = v.f64();
            break;
          case ValType::I32x4:
            memcpy(datum, v.i32x4(), Simd128DataSize);
            break;
          case ValType::F32x4:
            memcpy(datum, v.f32x4(), Simd128DataSize);
            break;
        }
        break;
      }

      case AsmJSModule::Global::InitImport: {
        RootedPropertyName field(cx, global.varImportField());
        RootedValue v(cx);
        if (!GetDataProperty(cx, importVal, field, &v))
            return false;

        if (!v.isPrimitive() && !HasPureCoercion(cx, v))
            return LinkFail(cx, "Imported values must be primitives");

        switch (global.varInitImportType()) {
          case ValType::I32:
            if (!ToInt32(cx, v, (int32_t*)datum))
                return false;
            break;
          case ValType::I64:
            MOZ_CRASH("int64");
          case ValType::F32:
            if (!RoundFloat32(cx, v, (float*)datum))
                return false;
            break;
          case ValType::F64:
            if (!ToNumber(cx, v, (double*)datum))
                return false;
            break;
          case ValType::I32x4: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Int32x4>(cx, v, &simdConstant))
                return false;
            memcpy(datum, simdConstant.asInt32x4(), Simd128DataSize);
            break;
          }
          case ValType::F32x4: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Float32x4>(cx, v, &simdConstant))
                return false;
            memcpy(datum, simdConstant.asFloat32x4(), Simd128DataSize);
            break;
          }
        }
        break;
      }
    }

    return true;
}

static bool
ValidateFFI(JSContext* cx, AsmJSModule::Global& global, HandleValue importVal,
            AutoObjectVector* ffis)
{
    RootedPropertyName field(cx, global.ffiField());
    RootedValue v(cx);
    if (!GetDataProperty(cx, importVal, field, &v))
        return false;

    if (!v.isObject() || !v.toObject().is<JSFunction>())
        return LinkFail(cx, "FFI imports must be functions");

    (*ffis)[global.ffiIndex()].set(&v.toObject().as<JSFunction>());
    return true;
}

static bool
ValidateArrayView(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    RootedPropertyName field(cx, global.maybeViewName());
    if (!field)
        return true;

    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, field, &v))
        return false;

    bool tac = IsTypedArrayConstructor(v, global.viewType());
    if (!tac)
        return LinkFail(cx, "bad typed array constructor");

    return true;
}

static bool
ValidateByteLength(JSContext* cx, HandleValue globalVal)
{
    RootedPropertyName field(cx, cx->names().byteLength);
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, field, &v))
        return false;

    if (!v.isObject() || !v.toObject().isBoundFunction())
        return LinkFail(cx, "byteLength must be a bound function object");

    RootedFunction fun(cx, &v.toObject().as<JSFunction>());

    RootedValue boundTarget(cx, ObjectValue(*fun->getBoundFunctionTarget()));
    if (!IsNativeFunction(boundTarget, fun_call))
        return LinkFail(cx, "bound target of byteLength must be Function.prototype.call");

    RootedValue boundThis(cx, fun->getBoundFunctionThis());
    if (!IsNativeFunction(boundThis, ArrayBufferObject::byteLengthGetter))
        return LinkFail(cx, "bound this value must be ArrayBuffer.protototype.byteLength accessor");

    return true;
}

static bool
ValidateMathBuiltinFunction(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Math, &v))
        return false;

    RootedPropertyName field(cx, global.mathName());
    if (!GetDataProperty(cx, v, field, &v))
        return false;

    Native native = nullptr;
    switch (global.mathBuiltinFunction()) {
      case AsmJSMathBuiltin_sin: native = math_sin; break;
      case AsmJSMathBuiltin_cos: native = math_cos; break;
      case AsmJSMathBuiltin_tan: native = math_tan; break;
      case AsmJSMathBuiltin_asin: native = math_asin; break;
      case AsmJSMathBuiltin_acos: native = math_acos; break;
      case AsmJSMathBuiltin_atan: native = math_atan; break;
      case AsmJSMathBuiltin_ceil: native = math_ceil; break;
      case AsmJSMathBuiltin_floor: native = math_floor; break;
      case AsmJSMathBuiltin_exp: native = math_exp; break;
      case AsmJSMathBuiltin_log: native = math_log; break;
      case AsmJSMathBuiltin_pow: native = math_pow; break;
      case AsmJSMathBuiltin_sqrt: native = math_sqrt; break;
      case AsmJSMathBuiltin_min: native = math_min; break;
      case AsmJSMathBuiltin_max: native = math_max; break;
      case AsmJSMathBuiltin_abs: native = math_abs; break;
      case AsmJSMathBuiltin_atan2: native = math_atan2; break;
      case AsmJSMathBuiltin_imul: native = math_imul; break;
      case AsmJSMathBuiltin_clz32: native = math_clz32; break;
      case AsmJSMathBuiltin_fround: native = math_fround; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Math.* builtin function");

    return true;
}

static PropertyName*
SimdTypeToName(JSContext* cx, AsmJSSimdType type)
{
    switch (type) {
      case AsmJSSimdType_int32x4:   return cx->names().int32x4;
      case AsmJSSimdType_float32x4: return cx->names().float32x4;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected SIMD type");
}

static SimdTypeDescr::Type
AsmJSSimdTypeToTypeDescrType(AsmJSSimdType type)
{
    switch (type) {
      case AsmJSSimdType_int32x4: return Int32x4::type;
      case AsmJSSimdType_float32x4: return Float32x4::type;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected AsmJSSimdType");
}

static bool
ValidateSimdType(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal,
                 MutableHandleValue out)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().SIMD, &v))
        return false;

    AsmJSSimdType type;
    if (global.which() == AsmJSModule::Global::SimdCtor)
        type = global.simdCtorType();
    else
        type = global.simdOperationType();

    RootedPropertyName simdTypeName(cx, SimdTypeToName(cx, type));
    if (!GetDataProperty(cx, v, simdTypeName, &v))
        return false;

    if (!v.isObject())
        return LinkFail(cx, "bad SIMD type");

    RootedObject simdDesc(cx, &v.toObject());
    if (!simdDesc->is<SimdTypeDescr>())
        return LinkFail(cx, "bad SIMD type");

    if (AsmJSSimdTypeToTypeDescrType(type) != simdDesc->as<SimdTypeDescr>().type())
        return LinkFail(cx, "bad SIMD type");

    out.set(v);
    return true;
}

static bool
ValidateSimdType(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    RootedValue _(cx);
    return ValidateSimdType(cx, global, globalVal, &_);
}

static bool
ValidateSimdOperation(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    // SIMD operations are loaded from the SIMD type, so the type must have been
    // validated before the operation.
    RootedValue v(cx);
    JS_ALWAYS_TRUE(ValidateSimdType(cx, global, globalVal, &v));

    RootedPropertyName opName(cx, global.simdOperationName());
    if (!GetDataProperty(cx, v, opName, &v))
        return false;

    Native native = nullptr;
    switch (global.simdOperationType()) {
#define SET_NATIVE_INT32X4(op) case AsmJSSimdOperation_##op: native = simd_int32x4_##op; break;
#define SET_NATIVE_FLOAT32X4(op) case AsmJSSimdOperation_##op: native = simd_float32x4_##op; break;
#define FALLTHROUGH(op) case AsmJSSimdOperation_##op:
      case AsmJSSimdType_int32x4:
        switch (global.simdOperation()) {
          FOREACH_INT32X4_SIMD_OP(SET_NATIVE_INT32X4)
          FOREACH_COMMONX4_SIMD_OP(SET_NATIVE_INT32X4)
          FOREACH_FLOAT32X4_SIMD_OP(FALLTHROUGH)
            MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("shouldn't have been validated in the first "
                                                    "place");
        }
        break;
      case AsmJSSimdType_float32x4:
        switch (global.simdOperation()) {
          FOREACH_FLOAT32X4_SIMD_OP(SET_NATIVE_FLOAT32X4)
          FOREACH_COMMONX4_SIMD_OP(SET_NATIVE_FLOAT32X4)
          FOREACH_INT32X4_SIMD_OP(FALLTHROUGH)
             MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("shouldn't have been validated in the first "
                                                     "place");
        }
        break;
#undef FALLTHROUGH
#undef SET_NATIVE_FLOAT32X4
#undef SET_NATIVE_INT32X4
#undef SET_NATIVE
    }
    if (!native || !IsNativeFunction(v, native))
        return LinkFail(cx, "bad SIMD.type.* operation");
    return true;
}

static bool
ValidateAtomicsBuiltinFunction(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Atomics, &v))
        return false;
    RootedPropertyName field(cx, global.atomicsName());
    if (!GetDataProperty(cx, v, field, &v))
        return false;

    Native native = nullptr;
    switch (global.atomicsBuiltinFunction()) {
      case AsmJSAtomicsBuiltin_compareExchange: native = atomics_compareExchange; break;
      case AsmJSAtomicsBuiltin_exchange: native = atomics_exchange; break;
      case AsmJSAtomicsBuiltin_load: native = atomics_load; break;
      case AsmJSAtomicsBuiltin_store: native = atomics_store; break;
      case AsmJSAtomicsBuiltin_fence: native = atomics_fence; break;
      case AsmJSAtomicsBuiltin_add: native = atomics_add; break;
      case AsmJSAtomicsBuiltin_sub: native = atomics_sub; break;
      case AsmJSAtomicsBuiltin_and: native = atomics_and; break;
      case AsmJSAtomicsBuiltin_or: native = atomics_or; break;
      case AsmJSAtomicsBuiltin_xor: native = atomics_xor; break;
      case AsmJSAtomicsBuiltin_isLockFree: native = atomics_isLockFree; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Atomics.* builtin function");

    return true;
}

static bool
ValidateConstant(JSContext* cx, AsmJSModule::Global& global, HandleValue globalVal)
{
    RootedPropertyName field(cx, global.constantName());
    RootedValue v(cx, globalVal);

    if (global.constantKind() == AsmJSModule::Global::MathConstant) {
        if (!GetDataProperty(cx, v, cx->names().Math, &v))
            return false;
    }

    if (!GetDataProperty(cx, v, field, &v))
        return false;

    if (!v.isNumber())
        return LinkFail(cx, "math / global constant value needs to be a number");

    // NaN != NaN
    if (IsNaN(global.constantValue())) {
        if (!IsNaN(v.toNumber()))
            return LinkFail(cx, "global constant value needs to be NaN");
    } else {
        if (v.toNumber() != global.constantValue())
            return LinkFail(cx, "global constant value mismatch");
    }

    return true;
}

static bool
LinkModuleToHeap(JSContext* cx, AsmJSModule& module, Handle<ArrayBufferObjectMaybeShared*> heap)
{
    uint32_t heapLength = heap->byteLength();

    if (!IsValidAsmJSHeapLength(heapLength)) {
        ScopedJSFreePtr<char> msg(
            JS_smprintf("ArrayBuffer byteLength 0x%x is not a valid heap length. The next "
                        "valid length is 0x%x",
                        heapLength,
                        RoundUpToNextValidAsmJSHeapLength(heapLength)));
        return LinkFail(cx, msg.get());
    }

    // This check is sufficient without considering the size of the loaded datum because heap
    // loads and stores start on an aligned boundary and the heap byteLength has larger alignment.
    MOZ_ASSERT((module.minHeapLength() - 1) <= INT32_MAX);
    if (heapLength < module.minHeapLength()) {
        ScopedJSFreePtr<char> msg(
            JS_smprintf("ArrayBuffer byteLength of 0x%x is less than 0x%x (the size implied "
                        "by const heap accesses and/or change-heap minimum-length requirements).",
                        heapLength,
                        module.minHeapLength()));
        return LinkFail(cx, msg.get());
    }

    if (heapLength > module.maxHeapLength()) {
        ScopedJSFreePtr<char> msg(
            JS_smprintf("ArrayBuffer byteLength 0x%x is greater than maximum length of 0x%x",
                        heapLength,
                        module.maxHeapLength()));
        return LinkFail(cx, msg.get());
    }

    // If we've generated the code with signal handlers in mind (for bounds
    // checks on x64 and for interrupt callback requesting on all platforms),
    // we need to be able to use signals at runtime. In particular, a module
    // can have been created using signals and cached, and executed without
    // signals activated.
    if (module.usesSignalHandlersForInterrupt() && !cx->canUseSignalHandlers())
        return LinkFail(cx, "Code generated with signal handlers but signals are deactivated");

    if (heap->is<ArrayBufferObject>()) {
        Rooted<ArrayBufferObject*> abheap(cx, &heap->as<ArrayBufferObject>());
        if (!ArrayBufferObject::prepareForAsmJS(cx, abheap, module.usesSignalHandlersForOOB()))
            return LinkFail(cx, "Unable to prepare ArrayBuffer for asm.js use");
    }

    module.initHeap(heap, cx);
    return true;
}

static bool
DynamicallyLinkModule(JSContext* cx, const CallArgs& args, AsmJSModule& module)
{
    module.setIsDynamicallyLinked(cx->runtime());

    HandleValue globalVal = args.get(0);
    HandleValue importVal = args.get(1);
    HandleValue bufferVal = args.get(2);

    Rooted<ArrayBufferObjectMaybeShared*> heap(cx);
    if (module.hasArrayView()) {
        if (module.isSharedView() && !IsSharedArrayBuffer(bufferVal))
            return LinkFail(cx, "shared views can only be constructed onto SharedArrayBuffer");
        if (!module.isSharedView() && !IsArrayBuffer(bufferVal))
            return LinkFail(cx, "unshared views can only be constructed onto ArrayBuffer");
        heap = &AsAnyArrayBuffer(bufferVal);
        if (!LinkModuleToHeap(cx, module, heap))
            return false;
    }

    AutoObjectVector ffis(cx);
    if (!ffis.resize(module.numFFIs()))
        return false;

    for (unsigned i = 0; i < module.numGlobals(); i++) {
        AsmJSModule::Global& global = module.global(i);
        switch (global.which()) {
          case AsmJSModule::Global::Variable:
            if (!ValidateGlobalVariable(cx, module, global, importVal))
                return false;
            break;
          case AsmJSModule::Global::FFI:
            if (!ValidateFFI(cx, global, importVal, &ffis))
                return false;
            break;
          case AsmJSModule::Global::ArrayView:
          case AsmJSModule::Global::ArrayViewCtor:
            if (!ValidateArrayView(cx, global, globalVal))
                return false;
            break;
          case AsmJSModule::Global::ByteLength:
            if (!ValidateByteLength(cx, globalVal))
                return false;
            break;
          case AsmJSModule::Global::MathBuiltinFunction:
            if (!ValidateMathBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSModule::Global::AtomicsBuiltinFunction:
            if (!ValidateAtomicsBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSModule::Global::Constant:
            if (!ValidateConstant(cx, global, globalVal))
                return false;
            break;
          case AsmJSModule::Global::SimdCtor:
            if (!ValidateSimdType(cx, global, globalVal))
                return false;
            break;
          case AsmJSModule::Global::SimdOperation:
            if (!ValidateSimdOperation(cx, global, globalVal))
                return false;
            break;
        }
    }

    for (unsigned i = 0; i < module.numExits(); i++) {
        const AsmJSModule::Exit& exit = module.exit(i);
        exit.datum(module).fun = &ffis[exit.ffiIndex()]->as<JSFunction>();
    }

    // See the comment in AllocateExecutableMemory.
    ExecutableAllocator::makeExecutable(module.codeBase(), module.codeBytes());

    return true;
}

static bool
ChangeHeap(JSContext* cx, AsmJSModule& module, const CallArgs& args)
{
    HandleValue bufferArg = args.get(0);
    if (!IsArrayBuffer(bufferArg)) {
        ReportIncompatible(cx, args);
        return false;
    }

    Rooted<ArrayBufferObject*> newBuffer(cx, &bufferArg.toObject().as<ArrayBufferObject>());
    uint32_t heapLength = newBuffer->byteLength();
    if (heapLength & module.heapLengthMask() ||
        heapLength < module.minHeapLength() ||
        heapLength > module.maxHeapLength())
    {
        args.rval().set(BooleanValue(false));
        return true;
    }

    if (!module.hasArrayView()) {
        args.rval().set(BooleanValue(true));
        return true;
    }

    MOZ_ASSERT(IsValidAsmJSHeapLength(heapLength));

    if (!ArrayBufferObject::prepareForAsmJS(cx, newBuffer, module.usesSignalHandlersForOOB()))
        return false;

    args.rval().set(BooleanValue(module.changeHeap(newBuffer, cx)));
    return true;
}

// An asm.js function stores, in its extended slots:
//  - a pointer to the module from which it was returned
//  - its index in the ordered list of exported functions
static const unsigned ASM_MODULE_SLOT = 0;
static const unsigned ASM_EXPORT_INDEX_SLOT = 1;

static unsigned
FunctionToExportedFunctionIndex(HandleFunction fun)
{
    MOZ_ASSERT(IsAsmJSFunction(fun));
    Value v = fun->getExtendedSlot(ASM_EXPORT_INDEX_SLOT);
    return v.toInt32();
}

static const AsmJSModule::ExportedFunction&
FunctionToExportedFunction(HandleFunction fun, AsmJSModule& module)
{
    unsigned funIndex = FunctionToExportedFunctionIndex(fun);
    return module.exportedFunction(funIndex);
}

static AsmJSModule&
FunctionToEnclosingModule(HandleFunction fun)
{
    return fun->getExtendedSlot(ASM_MODULE_SLOT).toObject().as<AsmJSModuleObject>().module();
}

// This is the js::Native for functions exported by an asm.js module.
static bool
CallAsmJS(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs callArgs = CallArgsFromVp(argc, vp);
    RootedFunction callee(cx, &callArgs.callee().as<JSFunction>());
    AsmJSModule& module = FunctionToEnclosingModule(callee);
    const AsmJSModule::ExportedFunction& func = FunctionToExportedFunction(callee, module);

    // The heap-changing function is a special-case and is implemented by C++.
    if (func.isChangeHeap())
        return ChangeHeap(cx, module, callArgs);

    // Enable/disable profiling in the asm.js module to match the current global
    // profiling state. Don't do this if the module is already active on the
    // stack since this would leave the module in a state where profiling is
    // enabled but the stack isn't unwindable.
    if (module.profilingEnabled() != cx->runtime()->spsProfiler.enabled() && !module.active())
        module.setProfilingEnabled(cx->runtime()->spsProfiler.enabled(), cx);

    // The calling convention for an external call into asm.js is to pass an
    // array of 16-byte values where each value contains either a coerced int32
    // (in the low word), a double value (in the low dword) or a SIMD vector
    // value, with the coercions specified by the asm.js signature. The
    // external entry point unpacks this array into the system-ABI-specified
    // registers and stack memory and then calls into the internal entry point.
    // The return value is stored in the first element of the array (which,
    // therefore, must have length >= 1).
    js::Vector<AsmJSModule::EntryArg, 8> coercedArgs(cx);
    if (!coercedArgs.resize(Max<size_t>(1, func.sig().args().length())))
        return false;

    RootedValue v(cx);
    for (unsigned i = 0; i < func.sig().args().length(); ++i) {
        v = i < callArgs.length() ? callArgs[i] : UndefinedValue();
        switch (func.sig().arg(i)) {
          case ValType::I32:
            if (!ToInt32(cx, v, (int32_t*)&coercedArgs[i]))
                return false;
            break;
          case ValType::I64:
            MOZ_CRASH("int64");
          case ValType::F32:
            if (!RoundFloat32(cx, v, (float*)&coercedArgs[i]))
                return false;
            break;
          case ValType::F64:
            if (!ToNumber(cx, v, (double*)&coercedArgs[i]))
                return false;
            break;
          case ValType::I32x4: {
            SimdConstant simd;
            if (!ToSimdConstant<Int32x4>(cx, v, &simd))
                return false;
            memcpy(&coercedArgs[i], simd.asInt32x4(), Simd128DataSize);
            break;
          }
          case ValType::F32x4: {
            SimdConstant simd;
            if (!ToSimdConstant<Float32x4>(cx, v, &simd))
                return false;
            memcpy(&coercedArgs[i], simd.asFloat32x4(), Simd128DataSize);
            break;
          }
        }
    }

    // The correct way to handle this situation would be to allocate a new range
    // of PROT_NONE memory and module.changeHeap to this memory. That would
    // cause every access to take the out-of-bounds signal-handler path which
    // does the right thing. For now, just throw an out-of-memory exception
    // since these can technically pop out anywhere and the full fix may
    // actually OOM when trying to allocate the PROT_NONE memory.
    if (module.hasDetachedHeap()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_OUT_OF_MEMORY);
        return false;
    }

    {
        // Push an AsmJSActivation to describe the asm.js frames we're about to
        // push when running this module. Additionally, push a JitActivation so
        // that the optimized asm.js-to-Ion FFI call path (which we want to be
        // very fast) can avoid doing so. The JitActivation is marked as
        // inactive so stack iteration will skip over it.
        AsmJSActivation activation(cx, module);
        JitActivation jitActivation(cx, /* active */ false);

        // Call the per-exported-function trampoline created by GenerateEntry.
        AsmJSModule::CodePtr enter = module.entryTrampoline(func);
        if (!CALL_GENERATED_2(enter, coercedArgs.begin(), module.globalData()))
            return false;
    }

    if (callArgs.isConstructing()) {
        // By spec, when a function is called as a constructor and this function
        // returns a primary type, which is the case for all asm.js exported
        // functions, the returned value is discarded and an empty object is
        // returned instead.
        PlainObject* obj = NewBuiltinClassInstance<PlainObject>(cx);
        if (!obj)
            return false;
        callArgs.rval().set(ObjectValue(*obj));
        return true;
    }

    JSObject* simdObj;
    switch (func.sig().ret()) {
      case ExprType::Void:
        callArgs.rval().set(UndefinedValue());
        break;
      case ExprType::I32:
        callArgs.rval().set(Int32Value(*(int32_t*)&coercedArgs[0]));
        break;
      case ExprType::I64:
        MOZ_CRASH("int64");
      case ExprType::F32:
      case ExprType::F64:
        callArgs.rval().set(NumberValue(*(double*)&coercedArgs[0]));
        break;
      case ExprType::I32x4:
        simdObj = CreateSimd<Int32x4>(cx, (int32_t*)&coercedArgs[0]);
        if (!simdObj)
            return false;
        callArgs.rval().set(ObjectValue(*simdObj));
        break;
      case ExprType::F32x4:
        simdObj = CreateSimd<Float32x4>(cx, (float*)&coercedArgs[0]);
        if (!simdObj)
            return false;
        callArgs.rval().set(ObjectValue(*simdObj));
        break;
    }

    return true;
}

static JSFunction*
NewExportedFunction(JSContext* cx, const AsmJSModule::ExportedFunction& func,
                    HandleObject moduleObj, unsigned exportIndex)
{
    RootedPropertyName name(cx, func.name());
    unsigned numArgs = func.isChangeHeap() ? 1 : func.sig().args().length();
    JSFunction* fun =
        NewNativeConstructor(cx, CallAsmJS, numArgs, name,
                             gc::AllocKind::FUNCTION_EXTENDED, GenericObject,
                             JSFunction::ASMJS_CTOR);
    if (!fun)
        return nullptr;

    fun->setExtendedSlot(ASM_MODULE_SLOT, ObjectValue(*moduleObj));
    fun->setExtendedSlot(ASM_EXPORT_INDEX_SLOT, Int32Value(exportIndex));
    return fun;
}

static bool
HandleDynamicLinkFailure(JSContext* cx, const CallArgs& args, AsmJSModule& module,
                         HandlePropertyName name)
{
    if (cx->isExceptionPending())
        return false;

    // Source discarding is allowed to affect JS semantics because it is never
    // enabled for normal JS content.
    bool haveSource = module.scriptSource()->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, module.scriptSource(), &haveSource))
        return false;
    if (!haveSource) {
        JS_ReportError(cx, "asm.js link failure with source discarding enabled");
        return false;
    }

    uint32_t begin = module.srcBodyStart();  // starts right after 'use asm'
    uint32_t end = module.srcEndBeforeCurly();
    Rooted<JSFlatString*> src(cx, module.scriptSource()->substringDontDeflate(cx, begin, end));
    if (!src)
        return false;

    RootedFunction fun(cx, NewScriptedFunction(cx, 0, JSFunction::INTERPRETED_NORMAL,
                                               name, gc::AllocKind::FUNCTION,
                                               TenuredObject));
    if (!fun)
        return false;

    Rooted<PropertyNameVector> formals(cx, PropertyNameVector(cx));
    if (!formals.reserve(3))
        return false;

    if (module.globalArgumentName())
        formals.infallibleAppend(module.globalArgumentName());
    if (module.importArgumentName())
        formals.infallibleAppend(module.importArgumentName());
    if (module.bufferArgumentName())
        formals.infallibleAppend(module.bufferArgumentName());

    CompileOptions options(cx);
    options.setMutedErrors(module.scriptSource()->mutedErrors())
           .setFile(module.scriptSource()->filename())
           .setNoScriptRval(false);

    // The exported function inherits an implicit strict context if the module
    // also inherited it somehow.
    if (module.strict())
        options.strictOption = true;

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, src))
        return false;

    const char16_t* chars = stableChars.twoByteRange().start().get();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    SourceBufferHolder srcBuf(chars, end - begin, ownership);
    if (!frontend::CompileFunctionBody(cx, &fun, options, formals, srcBuf))
        return false;

    // Call the function we just recompiled.
    args.setCallee(ObjectValue(*fun));
    return Invoke(cx, args, args.isConstructing() ? CONSTRUCT : NO_CONSTRUCT);
}

#ifdef MOZ_VTUNE
static bool
SendFunctionsToVTune(JSContext* cx, AsmJSModule& module)
{
    uint8_t* base = module.codeBase();

    for (unsigned i = 0; i < module.numProfiledFunctions(); i++) {
        const AsmJSModule::ProfiledFunction& func = module.profiledFunction(i);

        uint8_t* start = base + func.pod.startCodeOffset;
        uint8_t* end   = base + func.pod.endCodeOffset;
        MOZ_ASSERT(end >= start);

        unsigned method_id = iJIT_GetNewMethodID();
        if (method_id == 0)
            return false;

        JSAutoByteString bytes;
        const char* method_name = AtomToPrintableString(cx, func.name, &bytes);
        if (!method_name)
            return false;

        iJIT_Method_Load method;
        method.method_id = method_id;
        method.method_name = const_cast<char*>(method_name);
        method.method_load_address = (void*)start;
        method.method_size = unsigned(end - start);
        method.line_number_size = 0;
        method.line_number_table = nullptr;
        method.class_id = 0;
        method.class_file_name = nullptr;
        method.source_file_name = nullptr;

        iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&method);
    }

    return true;
}
#endif

#ifdef JS_ION_PERF
static bool
SendFunctionsToPerf(JSContext* cx, AsmJSModule& module)
{
    if (!PerfFuncEnabled())
        return true;

    uintptr_t base = (uintptr_t) module.codeBase();
    const char* filename = module.scriptSource()->filename();

    for (unsigned i = 0; i < module.numProfiledFunctions(); i++) {
        const AsmJSModule::ProfiledFunction& func = module.profiledFunction(i);
        uintptr_t start = base + (unsigned long) func.pod.startCodeOffset;
        uintptr_t end   = base + (unsigned long) func.pod.endCodeOffset;
        MOZ_ASSERT(end >= start);
        size_t size = end - start;

        JSAutoByteString bytes;
        const char* name = AtomToPrintableString(cx, func.name, &bytes);
        if (!name)
            return false;

        writePerfSpewerAsmJSFunctionMap(start, size, filename, func.pod.lineno,
                                        func.pod.columnIndex, name);
    }

    return true;
}
#endif

static bool
SendModuleToAttachedProfiler(JSContext* cx, AsmJSModule& module)
{
#if defined(MOZ_VTUNE)
    if (IsVTuneProfilingActive() && !SendFunctionsToVTune(cx, module))
        return false;
#endif
#if defined(JS_ION_PERF)
    if (!SendFunctionsToPerf(cx, module))
        return false;
#endif

    return true;
}


static JSObject*
CreateExportObject(JSContext* cx, Handle<AsmJSModuleObject*> moduleObj)
{
    AsmJSModule& module = moduleObj->module();

    if (module.numExportedFunctions() == 1) {
        const AsmJSModule::ExportedFunction& func = module.exportedFunction(0);
        if (!func.maybeFieldName())
            return NewExportedFunction(cx, func, moduleObj, 0);
    }

    gc::AllocKind allocKind = gc::GetGCObjectKind(module.numExportedFunctions());
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, allocKind));
    if (!obj)
        return nullptr;

    for (unsigned i = 0; i < module.numExportedFunctions(); i++) {
        const AsmJSModule::ExportedFunction& func = module.exportedFunction(i);

        RootedFunction fun(cx, NewExportedFunction(cx, func, moduleObj, i));
        if (!fun)
            return nullptr;

        MOZ_ASSERT(func.maybeFieldName() != nullptr);
        RootedId id(cx, NameToId(func.maybeFieldName()));
        RootedValue val(cx, ObjectValue(*fun));
        if (!NativeDefineProperty(cx, obj, id, val, nullptr, nullptr, JSPROP_ENUMERATE))
            return nullptr;
    }

    return obj;
}

static const unsigned MODULE_FUN_SLOT = 0;

static AsmJSModuleObject&
ModuleFunctionToModuleObject(JSFunction* fun)
{
    return fun->getExtendedSlot(MODULE_FUN_SLOT).toObject().as<AsmJSModuleObject>();
}

// Implements the semantics of an asm.js module function that has been successfully validated.
static bool
LinkAsmJS(JSContext* cx, unsigned argc, JS::Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // The LinkAsmJS builtin (created by NewAsmJSModuleFunction) is an extended
    // function and stores its module in an extended slot.
    RootedFunction fun(cx, &args.callee().as<JSFunction>());
    Rooted<AsmJSModuleObject*> moduleObj(cx, &ModuleFunctionToModuleObject(fun));


    // When a module is linked, it is dynamically specialized to the given
    // arguments (buffer, ffis). Thus, if the module is linked again (it is just
    // a function so it can be called multiple times), we need to clone a new
    // module.
    if (moduleObj->module().isDynamicallyLinked() && !CloneModule(cx, &moduleObj))
        return false;

    AsmJSModule& module = moduleObj->module();

    AutoFlushICache afc("LinkAsmJS");
    module.setAutoFlushICacheRange();

    // Link the module by performing the link-time validation checks in the
    // asm.js spec and then patching the generated module to associate it with
    // the given heap (ArrayBuffer) and a new global data segment (the closure
    // state shared by the inner asm.js functions).
    if (!DynamicallyLinkModule(cx, args, module)) {
        // Linking failed, so reparse the entire asm.js module from scratch to
        // get normal interpreted bytecode which we can simply Invoke. Very slow.
        RootedPropertyName name(cx, fun->name());
        return HandleDynamicLinkFailure(cx, args, module, name);
    }

    // Notify profilers so that asm.js generated code shows up with JS function
    // names and lines in native (i.e., not SPS) profilers.
    if (!SendModuleToAttachedProfiler(cx, module))
        return false;

    // Link-time validation succeeded, so wrap all the exported functions with
    // CallAsmJS builtins that trampoline into the generated code.
    JSObject* obj = CreateExportObject(cx, moduleObj);
    if (!obj)
        return false;

    args.rval().set(ObjectValue(*obj));
    return true;
}

JSFunction*
js::NewAsmJSModuleFunction(ExclusiveContext* cx, JSFunction* origFun, HandleObject moduleObj)
{
    RootedPropertyName name(cx, origFun->name());

    JSFunction::Flags flags = origFun->isLambda() ? JSFunction::ASMJS_LAMBDA_CTOR
                                                  : JSFunction::ASMJS_CTOR;
    JSFunction* moduleFun =
        NewNativeConstructor(cx, LinkAsmJS, origFun->nargs(), name,
                             gc::AllocKind::FUNCTION_EXTENDED, TenuredObject,
                             flags);
    if (!moduleFun)
        return nullptr;

    moduleFun->setExtendedSlot(MODULE_FUN_SLOT, ObjectValue(*moduleObj));
    return moduleFun;
}

bool
js::IsAsmJSModuleNative(js::Native native)
{
    return native == LinkAsmJS;
}

static bool
IsMaybeWrappedNativeFunction(const Value& v, Native native, JSFunction** fun = nullptr)
{
    if (!v.isObject())
        return false;

    JSObject* obj = CheckedUnwrap(&v.toObject());
    if (!obj)
        return false;

    if (!obj->is<JSFunction>())
        return false;

    if (fun)
        *fun = &obj->as<JSFunction>();

    return obj->as<JSFunction>().maybeNative() == native;
}

bool
js::IsAsmJSModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    bool rval = args.hasDefined(0) && IsMaybeWrappedNativeFunction(args.get(0), LinkAsmJS);
    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSModule(HandleFunction fun)
{
    return fun->isNative() && fun->maybeNative() == LinkAsmJS;
}

static bool
AppendUseStrictSource(JSContext* cx, HandleFunction fun, Handle<JSFlatString*> src, StringBuffer& out)
{
    // We need to add "use strict" in the body right after the opening
    // brace.
    size_t bodyStart = 0, bodyEnd;

    // No need to test for functions created with the Function ctor as
    // these don't implicitly inherit the "use strict" context. Strict mode is
    // enabled for functions created with the Function ctor only if they begin with
    // the "use strict" directive, but these functions won't validate as asm.js
    // modules.

    if (!FindBody(cx, fun, src, &bodyStart, &bodyEnd))
        return false;

    return out.appendSubstring(src, 0, bodyStart) &&
           out.append("\n\"use strict\";\n") &&
           out.appendSubstring(src, bodyStart, src->length() - bodyStart);
}

JSString*
js::AsmJSModuleToString(JSContext* cx, HandleFunction fun, bool addParenToLambda)
{
    AsmJSModule& module = ModuleFunctionToModuleObject(fun).module();

    uint32_t begin = module.srcStart();
    uint32_t end = module.srcEndAfterCurly();
    ScriptSource* source = module.scriptSource();
    StringBuffer out(cx);

    if (addParenToLambda && fun->isLambda() && !out.append("("))
        return nullptr;

    if (!out.append("function "))
        return nullptr;

    if (fun->atom() && !out.append(fun->atom()))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        // Whether the function has been created with a Function ctor
        bool funCtor = begin == 0 && end == source->length() && source->argumentsNotIncluded();
        if (funCtor) {
            // Functions created with the function constructor don't have arguments in their source.
            if (!out.append("("))
                return nullptr;

            if (PropertyName* argName = module.globalArgumentName()) {
                if (!out.append(argName))
                    return nullptr;
            }
            if (PropertyName* argName = module.importArgumentName()) {
                if (!out.append(", ") || !out.append(argName))
                    return nullptr;
            }
            if (PropertyName* argName = module.bufferArgumentName()) {
                if (!out.append(", ") || !out.append(argName))
                    return nullptr;
            }

            if (!out.append(") {\n"))
                return nullptr;
        }

        Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
        if (!src)
            return nullptr;

        if (module.strict()) {
            if (!AppendUseStrictSource(cx, fun, src, out))
                return nullptr;
        } else {
            if (!out.append(src))
                return nullptr;
        }

        if (funCtor && !out.append("\n}"))
            return nullptr;
    }

    if (addParenToLambda && fun->isLambda() && !out.append(")"))
        return nullptr;

    return out.finishString();
}

bool
js::IsAsmJSModuleLoadedFromCache(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunction* fun;
    if (!args.hasDefined(0) || !IsMaybeWrappedNativeFunction(args[0], LinkAsmJS, &fun)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_USE_ASM_TYPE_FAIL,
                             "argument passed to isAsmJSModuleLoadedFromCache is not a "
                             "validated asm.js module");
        return false;
    }

    bool loadedFromCache = ModuleFunctionToModuleObject(fun).module().loadedFromCache();

    args.rval().set(BooleanValue(loadedFromCache));
    return true;
}

bool
js::IsAsmJSFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    bool rval = args.hasDefined(0) && IsMaybeWrappedNativeFunction(args[0], CallAsmJS);
    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSFunction(HandleFunction fun)
{
    return fun->isNative() && fun->maybeNative() == CallAsmJS;
}

JSString*
js::AsmJSFunctionToString(JSContext* cx, HandleFunction fun)
{
    AsmJSModule& module = FunctionToEnclosingModule(fun);
    const AsmJSModule::ExportedFunction& f = FunctionToExportedFunction(fun, module);
    uint32_t begin = module.srcStart() + f.startOffsetInModule();
    uint32_t end = module.srcStart() + f.endOffsetInModule();

    ScriptSource* source = module.scriptSource();
    StringBuffer out(cx);

    if (!out.append("function "))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        // asm.js functions can't be anonymous
        MOZ_ASSERT(fun->atom());
        if (!out.append(fun->atom()))
            return nullptr;
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        // asm.js functions cannot have been created with a Function constructor
        // as they belong within a module.
        MOZ_ASSERT(!(begin == 0 && end == source->length() && source->argumentsNotIncluded()));

        if (module.strict()) {
            // AppendUseStrictSource expects its input to start right after the
            // function name, so split the source chars from the src into two parts:
            // the function name and the rest (arguments + body).

            // asm.js functions can't be anonymous
            MOZ_ASSERT(fun->atom());
            if (!out.append(fun->atom()))
                return nullptr;

            size_t nameEnd = begin + fun->atom()->length();
            Rooted<JSFlatString*> src(cx, source->substring(cx, nameEnd, end));
            if (!src || !AppendUseStrictSource(cx, fun, src, out))
                return nullptr;
        } else {
            Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
            if (!src)
                return nullptr;
            if (!out.append(src))
                return nullptr;
        }
    }

    return out.finishString();
}
