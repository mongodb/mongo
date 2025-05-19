/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProtoKey_h
#define js_ProtoKey_h

/* A higher-order macro for enumerating all JSProtoKey values. */
/*
 * Consumers define macros as follows:
 * MACRO(name, clasp)
 *   name:    The canonical name of the class.
 *   clasp:   The JSClass for this object, or "dummy" if it doesn't exist.
 *
 *
 * Consumers wishing to iterate over all the JSProtoKey values, can use
 * JS_FOR_EACH_PROTOTYPE. However, there are certain values that don't
 * correspond to real constructors, like Null or constructors that are disabled
 * via preprocessor directives. We still need to include these in the JSProtoKey
 * list in order to maintain binary XDR compatibility, but we need to provide a
 * tool to handle them differently. JS_FOR_PROTOTYPES fills this niche.
 *
 * Consumers pass two macros to JS_FOR_PROTOTYPES - |REAL| and |IMAGINARY|. The
 * former is invoked for entries that have real client-exposed constructors, and
 * the latter is called for the rest. Consumers that don't care about this
 * distinction can simply pass the same macro to both, which is exactly what
 * JS_FOR_EACH_PROTOTYPE does.
 */

#define CLASP(NAME) (&NAME##Class)
#define OCLASP(NAME) (&NAME##Object::class_)
#define TYPED_ARRAY_CLASP(TYPE) \
  (&TypedArrayObject::fixedLengthClasses[JS::Scalar::TYPE])
#define ERROR_CLASP(TYPE) (&ErrorObject::classes[TYPE])

#ifdef JS_HAS_INTL_API
#  define IF_INTL(REAL, IMAGINARY) REAL
#else
#  define IF_INTL(REAL, IMAGINARY) IMAGINARY
#endif

#ifdef JS_HAS_TEMPORAL_API
#  define IF_TEMPORAL(REAL, IMAGINARY) REAL
#else
#  define IF_TEMPORAL(REAL, IMAGINARY) IMAGINARY
#endif

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
#  define IF_WASM_TYPE(REAL, IMAGINARY) REAL
#else
#  define IF_WASM_TYPE(REAL, IMAGINARY) IMAGINARY
#endif

#ifdef ENABLE_WASM_JSPI
#  define IF_WASM_JSPI(REAL, IMAGINARY) REAL
#else
#  define IF_WASM_JSPI(REAL, IMAGINARY) IMAGINARY
#endif

#ifdef NIGHTLY_BUILD
#  define IF_NIGHTLY(REAL, IMAGINARY) REAL
#else
#  define IF_NIGHTLY(REAL, IMAGINARY) IMAGINARY
#endif

#define JS_FOR_PROTOTYPES_(REAL, IMAGINARY, REAL_IF_INTL, REAL_IF_TEMPORAL, \
                           REAL_IF_WASM_TYPE, REAL_IF_WASM_JSPI,            \
                           REAL_IF_NIGHTLY)                                 \
  IMAGINARY(Null, dummy)                                                    \
  REAL(Object, OCLASP(Plain))                                               \
  REAL(Function, &FunctionClass)                                            \
  IMAGINARY(BoundFunction, OCLASP(BoundFunction))                           \
  REAL(Array, OCLASP(Array))                                                \
  REAL(Boolean, OCLASP(Boolean))                                            \
  REAL(JSON, CLASP(JSON))                                                   \
  REAL(Date, OCLASP(Date))                                                  \
  REAL(Math, CLASP(Math))                                                   \
  REAL(Number, OCLASP(Number))                                              \
  REAL(String, OCLASP(String))                                              \
  REAL(RegExp, OCLASP(RegExp))                                              \
  REAL(Error, ERROR_CLASP(JSEXN_ERR))                                       \
  REAL(InternalError, ERROR_CLASP(JSEXN_INTERNALERR))                       \
  REAL(AggregateError, ERROR_CLASP(JSEXN_AGGREGATEERR))                     \
  REAL(EvalError, ERROR_CLASP(JSEXN_EVALERR))                               \
  REAL(RangeError, ERROR_CLASP(JSEXN_RANGEERR))                             \
  REAL(ReferenceError, ERROR_CLASP(JSEXN_REFERENCEERR))                     \
  REAL(SyntaxError, ERROR_CLASP(JSEXN_SYNTAXERR))                           \
  REAL(TypeError, ERROR_CLASP(JSEXN_TYPEERR))                               \
  REAL(URIError, ERROR_CLASP(JSEXN_URIERR))                                 \
  REAL(DebuggeeWouldRun, ERROR_CLASP(JSEXN_DEBUGGEEWOULDRUN))               \
  REAL(CompileError, ERROR_CLASP(JSEXN_WASMCOMPILEERROR))                   \
  REAL(LinkError, ERROR_CLASP(JSEXN_WASMLINKERROR))                         \
  REAL(RuntimeError, ERROR_CLASP(JSEXN_WASMRUNTIMEERROR))                   \
  REAL(ArrayBuffer, OCLASP(FixedLengthArrayBuffer))                         \
  REAL(Int8Array, TYPED_ARRAY_CLASP(Int8))                                  \
  REAL(Uint8Array, TYPED_ARRAY_CLASP(Uint8))                                \
  REAL(Int16Array, TYPED_ARRAY_CLASP(Int16))                                \
  REAL(Uint16Array, TYPED_ARRAY_CLASP(Uint16))                              \
  REAL(Int32Array, TYPED_ARRAY_CLASP(Int32))                                \
  REAL(Uint32Array, TYPED_ARRAY_CLASP(Uint32))                              \
  REAL(Float32Array, TYPED_ARRAY_CLASP(Float32))                            \
  REAL(Float64Array, TYPED_ARRAY_CLASP(Float64))                            \
  REAL(Uint8ClampedArray, TYPED_ARRAY_CLASP(Uint8Clamped))                  \
  REAL(BigInt64Array, TYPED_ARRAY_CLASP(BigInt64))                          \
  REAL(BigUint64Array, TYPED_ARRAY_CLASP(BigUint64))                        \
  REAL_IF_NIGHTLY(Float16Array, TYPED_ARRAY_CLASP(Float16))                 \
  REAL(BigInt, OCLASP(BigInt))                                              \
  REAL(Proxy, CLASP(Proxy))                                                 \
  REAL(WeakMap, OCLASP(WeakMap))                                            \
  REAL(Map, OCLASP(Map))                                                    \
  REAL(Set, OCLASP(Set))                                                    \
  REAL(DataView, OCLASP(FixedLengthDataView))                               \
  REAL(Symbol, OCLASP(Symbol))                                              \
  REAL(ShadowRealm, OCLASP(ShadowRealm))                                    \
  REAL(SharedArrayBuffer, OCLASP(FixedLengthSharedArrayBuffer))             \
  REAL_IF_INTL(Intl, CLASP(Intl))                                           \
  REAL_IF_INTL(Collator, OCLASP(Collator))                                  \
  REAL_IF_INTL(DateTimeFormat, OCLASP(DateTimeFormat))                      \
  REAL_IF_INTL(DisplayNames, OCLASP(DisplayNames))                          \
  REAL_IF_INTL(ListFormat, OCLASP(ListFormat))                              \
  REAL_IF_INTL(Locale, OCLASP(Locale))                                      \
  REAL_IF_INTL(NumberFormat, OCLASP(NumberFormat))                          \
  REAL_IF_INTL(PluralRules, OCLASP(PluralRules))                            \
  REAL_IF_INTL(RelativeTimeFormat, OCLASP(RelativeTimeFormat))              \
  REAL_IF_INTL(Segmenter, OCLASP(Segmenter))                                \
  REAL(Reflect, CLASP(Reflect))                                             \
  REAL(WeakSet, OCLASP(WeakSet))                                            \
  REAL(TypedArray, &js::TypedArrayObject::sharedTypedArrayPrototypeClass)   \
  REAL(Atomics, OCLASP(Atomics))                                            \
  REAL(SavedFrame, &js::SavedFrame::class_)                                 \
  REAL(Promise, OCLASP(Promise))                                            \
  REAL(AsyncFunction, CLASP(AsyncFunction))                                 \
  REAL(GeneratorFunction, CLASP(GeneratorFunction))                         \
  REAL(AsyncGeneratorFunction, CLASP(AsyncGeneratorFunction))               \
  REAL(WebAssembly, OCLASP(WasmNamespace))                                  \
  REAL(WasmModule, OCLASP(WasmModule))                                      \
  REAL(WasmInstance, OCLASP(WasmInstance))                                  \
  REAL(WasmMemory, OCLASP(WasmMemory))                                      \
  REAL(WasmTable, OCLASP(WasmTable))                                        \
  REAL(WasmGlobal, OCLASP(WasmGlobal))                                      \
  REAL(WasmTag, OCLASP(WasmTag))                                            \
  REAL_IF_WASM_TYPE(WasmFunction, CLASP(WasmFunction))                      \
  REAL_IF_WASM_JSPI(WasmSuspending, OCLASP(WasmSuspending))                 \
  REAL(WasmException, OCLASP(WasmException))                                \
  REAL(FinalizationRegistry, OCLASP(FinalizationRegistry))                  \
  REAL(WeakRef, OCLASP(WeakRef))                                            \
  REAL(Iterator, OCLASP(Iterator))                                          \
  REAL(AsyncIterator, OCLASP(AsyncIterator))                                \
  REAL_IF_TEMPORAL(Temporal, OCLASP(temporal::Temporal))                    \
  REAL_IF_TEMPORAL(Calendar, OCLASP(temporal::Calendar))                    \
  REAL_IF_TEMPORAL(Duration, OCLASP(temporal::Duration))                    \
  REAL_IF_TEMPORAL(Instant, OCLASP(temporal::Instant))                      \
  REAL_IF_TEMPORAL(PlainDate, OCLASP(temporal::PlainDate))                  \
  REAL_IF_TEMPORAL(PlainDateTime, OCLASP(temporal::PlainDateTime))          \
  REAL_IF_TEMPORAL(PlainMonthDay, OCLASP(temporal::PlainMonthDay))          \
  REAL_IF_TEMPORAL(PlainYearMonth, OCLASP(temporal::PlainYearMonth))        \
  REAL_IF_TEMPORAL(PlainTime, OCLASP(temporal::PlainTime))                  \
  REAL_IF_TEMPORAL(TemporalNow, OCLASP(temporal::TemporalNow))              \
  REAL_IF_TEMPORAL(TimeZone, OCLASP(temporal::TimeZone))                    \
  REAL_IF_TEMPORAL(ZonedDateTime, OCLASP(temporal::ZonedDateTime))          \
  IF_RECORD_TUPLE(REAL(Record, (&RecordType::class_)))                      \
  IF_RECORD_TUPLE(REAL(Tuple, (&TupleType::class_)))

#define JS_FOR_PROTOTYPES(REAL, IMAGINARY)                                     \
  JS_FOR_PROTOTYPES_(                                                          \
      REAL, IMAGINARY, IF_INTL(REAL, IMAGINARY), IF_TEMPORAL(REAL, IMAGINARY), \
      IF_WASM_TYPE(REAL, IMAGINARY), IF_WASM_JSPI(REAL, IMAGINARY),            \
      IF_NIGHTLY(REAL, IMAGINARY))

#define JS_FOR_EACH_PROTOTYPE(MACRO) JS_FOR_PROTOTYPES(MACRO, MACRO)

#endif /* js_ProtoKey_h */
