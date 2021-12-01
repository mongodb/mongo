/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProtoKey_h
#define js_ProtoKey_h

/* A higher-order macro for enumerating all JSProtoKey values. */
/*
 * Consumers define macros as follows:
 * macro(name, init, clasp)
 *   name:    The canonical name of the class.
 *   init:    Initialization function. These are |extern "C";|, and clients should use
 *            |extern "C" {}| as appropriate when using this macro.
 *   clasp:   The JSClass for this object, or "dummy" if it doesn't exist.
 *
 *
 * Consumers wishing to iterate over all the JSProtoKey values, can use
 * JS_FOR_EACH_PROTOTYPE. However, there are certain values that don't correspond
 * to real constructors, like Null or constructors that are disabled via
 * preprocessor directives. We still need to include these in the JSProtoKey list
 * in order to maintain binary XDR compatibility, but we need to provide a tool
 * to handle them differently. JS_FOR_PROTOTYPES fills this niche.
 *
 * Consumers pass two macros to JS_FOR_PROTOTYPES - |real| and |imaginary|. The
 * former is invoked for entries that have real client-exposed constructors, and
 * the latter is called for the rest. Consumers that don't care about this
 * distinction can simply pass the same macro to both, which is exactly what
 * JS_FOR_EACH_PROTOTYPE does.
 */

#define CLASP(name)                 (&name##Class)
#define OCLASP(name)                (&name##Object::class_)
#define TYPED_ARRAY_CLASP(type)     (&TypedArrayObject::classes[Scalar::type])
#define ERROR_CLASP(type)           (&ErrorObject::classes[type])

#ifdef EXPOSE_INTL_API
#define IF_INTL(real,imaginary) real
#else
#define IF_INTL(real,imaginary) imaginary
#endif

#ifdef ENABLE_BINARYDATA
#define IF_BDATA(real,imaginary) real
#else
#define IF_BDATA(real,imaginary) imaginary
#endif

#ifdef ENABLE_SIMD
# define IF_SIMD(real,imaginary) real
#else
# define IF_SIMD(real,imaginary) imaginary
#endif

#ifdef ENABLE_SHARED_ARRAY_BUFFER
#define IF_SAB(real,imaginary) real
#else
#define IF_SAB(real,imaginary) imaginary
#endif

#define JS_FOR_PROTOTYPES(real,imaginary) \
    imaginary(Null,             InitNullClass,          dummy) \
    real(Object,                InitViaClassSpec,       OCLASP(Plain)) \
    real(Function,              InitViaClassSpec,       &JSFunction::class_) \
    real(Array,                 InitViaClassSpec,       OCLASP(Array)) \
    real(Boolean,               InitBooleanClass,       OCLASP(Boolean)) \
    real(JSON,                  InitJSONClass,          CLASP(JSON)) \
    real(Date,                  InitViaClassSpec,       OCLASP(Date)) \
    real(Math,                  InitMathClass,          CLASP(Math)) \
    real(Number,                InitNumberClass,        OCLASP(Number)) \
    real(String,                InitStringClass,        OCLASP(String)) \
    real(RegExp,                InitViaClassSpec,       OCLASP(RegExp)) \
    real(Error,                 InitViaClassSpec,       ERROR_CLASP(JSEXN_ERR)) \
    real(InternalError,         InitViaClassSpec,       ERROR_CLASP(JSEXN_INTERNALERR)) \
    real(EvalError,             InitViaClassSpec,       ERROR_CLASP(JSEXN_EVALERR)) \
    real(RangeError,            InitViaClassSpec,       ERROR_CLASP(JSEXN_RANGEERR)) \
    real(ReferenceError,        InitViaClassSpec,       ERROR_CLASP(JSEXN_REFERENCEERR)) \
    real(SyntaxError,           InitViaClassSpec,       ERROR_CLASP(JSEXN_SYNTAXERR)) \
    real(TypeError,             InitViaClassSpec,       ERROR_CLASP(JSEXN_TYPEERR)) \
    real(URIError,              InitViaClassSpec,       ERROR_CLASP(JSEXN_URIERR)) \
    real(DebuggeeWouldRun,      InitViaClassSpec,       ERROR_CLASP(JSEXN_DEBUGGEEWOULDRUN)) \
    real(CompileError,          InitViaClassSpec,       ERROR_CLASP(JSEXN_WASMCOMPILEERROR)) \
    real(LinkError,             InitViaClassSpec,       ERROR_CLASP(JSEXN_WASMLINKERROR)) \
    real(RuntimeError,          InitViaClassSpec,       ERROR_CLASP(JSEXN_WASMRUNTIMEERROR)) \
    imaginary(Iterator,         dummy,                  dummy) \
    real(ArrayBuffer,           InitViaClassSpec,       OCLASP(ArrayBuffer)) \
    real(Int8Array,             InitViaClassSpec,       TYPED_ARRAY_CLASP(Int8)) \
    real(Uint8Array,            InitViaClassSpec,       TYPED_ARRAY_CLASP(Uint8)) \
    real(Int16Array,            InitViaClassSpec,       TYPED_ARRAY_CLASP(Int16)) \
    real(Uint16Array,           InitViaClassSpec,       TYPED_ARRAY_CLASP(Uint16)) \
    real(Int32Array,            InitViaClassSpec,       TYPED_ARRAY_CLASP(Int32)) \
    real(Uint32Array,           InitViaClassSpec,       TYPED_ARRAY_CLASP(Uint32)) \
    real(Float32Array,          InitViaClassSpec,       TYPED_ARRAY_CLASP(Float32)) \
    real(Float64Array,          InitViaClassSpec,       TYPED_ARRAY_CLASP(Float64)) \
    real(Uint8ClampedArray,     InitViaClassSpec,       TYPED_ARRAY_CLASP(Uint8Clamped)) \
    real(Proxy,                 InitProxyClass,         js::ProxyClassPtr) \
    real(WeakMap,               InitWeakMapClass,       OCLASP(WeakMap)) \
    real(Map,                   InitViaClassSpec,       OCLASP(Map)) \
    real(Set,                   InitViaClassSpec,       OCLASP(Set)) \
    real(DataView,              InitViaClassSpec,       OCLASP(DataView)) \
    real(Symbol,                InitSymbolClass,        OCLASP(Symbol)) \
IF_SAB(real,imaginary)(SharedArrayBuffer,       InitViaClassSpec, OCLASP(SharedArrayBuffer)) \
IF_INTL(real,imaginary) (Intl,                  InitIntlClass,          CLASP(Intl)) \
IF_BDATA(real,imaginary)(TypedObject,           InitTypedObjectModuleObject,   OCLASP(TypedObjectModule)) \
    real(Reflect,               InitReflect,            nullptr) \
IF_SIMD(real,imaginary)(SIMD,                   InitSimdClass, OCLASP(Simd)) \
    real(WeakSet,               InitWeakSetClass,       OCLASP(WeakSet)) \
    real(TypedArray,            InitViaClassSpec,       &js::TypedArrayObject::sharedTypedArrayPrototypeClass) \
IF_SAB(real,imaginary)(Atomics, InitAtomicsClass, OCLASP(Atomics)) \
    real(SavedFrame,            InitViaClassSpec,       &js::SavedFrame::class_) \
    real(Promise,               InitViaClassSpec,       OCLASP(Promise)) \
    real(ReadableStream,        InitViaClassSpec,       &js::ReadableStream::class_) \
    real(ReadableStreamDefaultReader,           InitViaClassSpec, &js::ReadableStreamDefaultReader::class_) \
    real(ReadableStreamBYOBReader,              InitViaClassSpec, &js::ReadableStreamBYOBReader::class_) \
    real(ReadableStreamDefaultController,       InitViaClassSpec, &js::ReadableStreamDefaultController::class_) \
    real(ReadableByteStreamController,          InitViaClassSpec, &js::ReadableByteStreamController::class_) \
    real(ReadableStreamBYOBRequest,             InitViaClassSpec, &js::ReadableStreamBYOBRequest::class_) \
    imaginary(WritableStream,   dummy,                  dummy) \
    imaginary(WritableStreamDefaultWriter,      dummy,  dummy) \
    imaginary(WritableStreamDefaultController,  dummy,  dummy) \
    real(ByteLengthQueuingStrategy,             InitViaClassSpec, &js::ByteLengthQueuingStrategy::class_) \
    real(CountQueuingStrategy,  InitViaClassSpec,       &js::CountQueuingStrategy::class_) \
    real(WebAssembly,           InitWebAssemblyClass,   CLASP(WebAssembly)) \
    imaginary(WasmModule,       dummy,                  dummy) \
    imaginary(WasmInstance,     dummy,                  dummy) \
    imaginary(WasmMemory,       dummy,                  dummy) \
    imaginary(WasmTable,        dummy,                  dummy) \
    imaginary(WasmGlobal,       dummy,                  dummy) \

#define JS_FOR_EACH_PROTOTYPE(macro) JS_FOR_PROTOTYPES(macro,macro)

#endif /* js_ProtoKey_h */
