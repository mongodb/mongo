/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A higher-order macro for enumerating all cached property names. */

#ifndef vm_CommonPropertyNames_h
#define vm_CommonPropertyNames_h

// The following common atoms are reserved by the js::StaticStrigs /
// js::frontend::WellKnownParserAtoms{,_ROM} mechanisms. We still use a named
// reference for the parser and VM to use.
//
// Parameter list is (IDPART, ID, TEXT).
//
// Each entry should use one of MACRO* based on the length of TEXT
//   * MACRO0: length-0 text
//   * MACRO1: length-1 text
//   * MACRO2: length-2 text
//   * MACRO_: other text
#define FOR_EACH_COMMON_PROPERTYNAME_(MACRO0, MACRO1, MACRO2, MACRO_)          \
  MACRO_(abort, abort, "abort")                                                \
  MACRO_(add, add, "add")                                                      \
  MACRO_(allowContentIter, allowContentIter, "allowContentIter")               \
  MACRO_(anonymous, anonymous, "anonymous")                                    \
  MACRO_(Any, Any, "Any")                                                      \
  MACRO_(apply, apply, "apply")                                                \
  MACRO_(arguments, arguments, "arguments")                                    \
  MACRO_(ArrayBufferSpecies, ArrayBufferSpecies, "$ArrayBufferSpecies")        \
  MACRO_(ArrayIterator, ArrayIterator, "Array Iterator")                       \
  MACRO_(ArrayIteratorNext, ArrayIteratorNext, "ArrayIteratorNext")            \
  MACRO_(ArraySort, ArraySort, "ArraySort")                                    \
  MACRO_(ArraySpecies, ArraySpecies, "$ArraySpecies")                          \
  MACRO_(ArraySpeciesCreate, ArraySpeciesCreate, "ArraySpeciesCreate")         \
  MACRO_(ArrayToLocaleString, ArrayToLocaleString, "ArrayToLocaleString")      \
  MACRO_(ArrayType, ArrayType, "ArrayType")                                    \
  MACRO_(ArrayValues, ArrayValues, "$ArrayValues")                             \
  MACRO2(as, as, "as")                                                         \
  MACRO_(Async, Async, "Async")                                                \
  MACRO_(AsyncFromSyncIterator, AsyncFromSyncIterator,                         \
         "Async-from-Sync Iterator")                                           \
  MACRO_(AsyncFunctionNext, AsyncFunctionNext, "AsyncFunctionNext")            \
  MACRO_(AsyncFunctionThrow, AsyncFunctionThrow, "AsyncFunctionThrow")         \
  MACRO_(AsyncGenerator, AsyncGenerator, "AsyncGenerator")                     \
  MACRO_(AsyncGeneratorNext, AsyncGeneratorNext, "AsyncGeneratorNext")         \
  MACRO_(AsyncGeneratorReturn, AsyncGeneratorReturn, "AsyncGeneratorReturn")   \
  MACRO_(AsyncGeneratorThrow, AsyncGeneratorThrow, "AsyncGeneratorThrow")      \
  MACRO_(AsyncWrapped, AsyncWrapped, "AsyncWrapped")                           \
  MACRO_(async, async, "async")                                                \
  MACRO2(at, at, "at")                                                         \
  MACRO_(autoAllocateChunkSize, autoAllocateChunkSize,                         \
         "autoAllocateChunkSize")                                              \
  MACRO_(await, await, "await")                                                \
  MACRO_(bigint64, bigint64, "bigint64")                                       \
  MACRO_(biguint64, biguint64, "biguint64")                                    \
  MACRO_(boundWithSpace, boundWithSpace, "bound ")                             \
  MACRO_(break, break_, "break")                                               \
  MACRO_(breakdown, breakdown, "breakdown")                                    \
  MACRO_(buffer, buffer, "buffer")                                             \
  MACRO_(builder, builder, "builder")                                          \
  MACRO2(by, by, "by")                                                         \
  MACRO_(byob, byob, "byob")                                                   \
  MACRO_(byteAlignment, byteAlignment, "byteAlignment")                        \
  MACRO_(byteLength, byteLength, "byteLength")                                 \
  MACRO_(byteOffset, byteOffset, "byteOffset")                                 \
  MACRO_(bytes, bytes, "bytes")                                                \
  MACRO_(BYTES_PER_ELEMENT, BYTES_PER_ELEMENT, "BYTES_PER_ELEMENT")            \
  MACRO_(calendar, calendar, "calendar")                                       \
  MACRO_(call, call, "call")                                                   \
  MACRO_(callContentFunction, callContentFunction, "callContentFunction")      \
  MACRO_(callee, callee, "callee")                                             \
  MACRO_(caller, caller, "caller")                                             \
  MACRO_(callFunction, callFunction, "callFunction")                           \
  MACRO_(cancel, cancel, "cancel")                                             \
  MACRO_(case, case_, "case")                                                  \
  MACRO_(caseFirst, caseFirst, "caseFirst")                                    \
  MACRO_(catch, catch_, "catch")                                               \
  MACRO_(cause, cause, "cause")                                                \
  MACRO_(class, class_, "class")                                               \
  MACRO_(cleanupSome, cleanupSome, "cleanupSome")                              \
  MACRO_(close, close, "close")                                                \
  MACRO_(collation, collation, "collation")                                    \
  MACRO_(collections, collections, "collections")                              \
  MACRO_(columnNumber, columnNumber, "columnNumber")                           \
  MACRO1(comma, comma, ",")                                                    \
  MACRO_(compare, compare, "compare")                                          \
  MACRO_(configurable, configurable, "configurable")                           \
  MACRO_(const, const_, "const")                                               \
  MACRO_(construct, construct, "construct")                                    \
  MACRO_(constructContentFunction, constructContentFunction,                   \
         "constructContentFunction")                                           \
  MACRO_(constructor, constructor, "constructor")                              \
  MACRO_(continue, continue_, "continue")                                      \
  MACRO_(CopyDataProperties, CopyDataProperties, "CopyDataProperties")         \
  MACRO_(CopyDataPropertiesUnfiltered, CopyDataPropertiesUnfiltered,           \
         "CopyDataPropertiesUnfiltered")                                       \
  MACRO_(copyWithin, copyWithin, "copyWithin")                                 \
  MACRO_(compact, compact, "compact")                                          \
  MACRO_(compactDisplay, compactDisplay, "compactDisplay")                     \
  MACRO_(count, count, "count")                                                \
  MACRO_(CreateResolvingFunctions, CreateResolvingFunctions,                   \
         "CreateResolvingFunctions")                                           \
  MACRO_(currency, currency, "currency")                                       \
  MACRO_(currencyDisplay, currencyDisplay, "currencyDisplay")                  \
  MACRO_(currencySign, currencySign, "currencySign")                           \
  MACRO_(day, day, "day")                                                      \
  MACRO_(dayPeriod, dayPeriod, "dayPeriod")                                    \
  MACRO_(debugger, debugger, "debugger")                                       \
  MACRO_(decimal, decimal, "decimal")                                          \
  MACRO_(decodeURI, decodeURI, "decodeURI")                                    \
  MACRO_(decodeURIComponent, decodeURIComponent, "decodeURIComponent")         \
  MACRO_(default, default_, "default")                                         \
  MACRO_(defineGetter, defineGetter, "__defineGetter__")                       \
  MACRO_(defineProperty, defineProperty, "defineProperty")                     \
  MACRO_(defineSetter, defineSetter, "__defineSetter__")                       \
  MACRO_(delete, delete_, "delete")                                            \
  MACRO_(deleteProperty, deleteProperty, "deleteProperty")                     \
  MACRO_(direction, direction, "direction")                                    \
  MACRO_(displayURL, displayURL, "displayURL")                                 \
  MACRO2(do, do_, "do")                                                        \
  MACRO_(domNode, domNode, "domNode")                                          \
  MACRO_(done, done, "done")                                                   \
  MACRO_(dotAll, dotAll, "dotAll")                                             \
  MACRO_(dotArgs, dotArgs, ".args")                                            \
  MACRO_(dotGenerator, dotGenerator, ".generator")                             \
  MACRO_(dotThis, dotThis, ".this")                                            \
  MACRO_(dotInitializers, dotInitializers, ".initializers")                    \
  MACRO_(dotFieldKeys, dotFieldKeys, ".fieldKeys")                             \
  MACRO_(dotPrivateBrand, dotPrivateBrand, ".privateBrand")                    \
  MACRO_(dotStaticInitializers, dotStaticInitializers, ".staticInitializers")  \
  MACRO_(dotStaticFieldKeys, dotStaticFieldKeys, ".staticFieldKeys")           \
  MACRO_(each, each, "each")                                                   \
  MACRO_(element, element, "element")                                          \
  MACRO_(elementType, elementType, "elementType")                              \
  MACRO_(else, else_, "else")                                                  \
  MACRO0(empty, empty, "")                                                     \
  MACRO_(emptyRegExp, emptyRegExp, "(?:)")                                     \
  MACRO_(encodeURI, encodeURI, "encodeURI")                                    \
  MACRO_(encodeURIComponent, encodeURIComponent, "encodeURIComponent")         \
  MACRO_(endRange, endRange, "endRange")                                       \
  MACRO_(endTimestamp, endTimestamp, "endTimestamp")                           \
  MACRO_(entries, entries, "entries")                                          \
  MACRO_(enum, enum_, "enum")                                                  \
  MACRO_(enumerable, enumerable, "enumerable")                                 \
  MACRO_(enumerate, enumerate, "enumerate")                                    \
  MACRO_(era, era, "era")                                                      \
  MACRO_(ErrorToStringWithTrailingNewline, ErrorToStringWithTrailingNewline,   \
         "ErrorToStringWithTrailingNewline")                                   \
  MACRO_(errors, errors, "errors")                                             \
  MACRO_(escape, escape, "escape")                                             \
  MACRO_(eval, eval, "eval")                                                   \
  MACRO_(exec, exec, "exec")                                                   \
  MACRO_(exponentInteger, exponentInteger, "exponentInteger")                  \
  MACRO_(exponentMinusSign, exponentMinusSign, "exponentMinusSign")            \
  MACRO_(exponentSeparator, exponentSeparator, "exponentSeparator")            \
  MACRO_(export, export_, "export")                                            \
  MACRO_(extends, extends, "extends")                                          \
  MACRO_(false, false_, "false")                                               \
  MACRO_(few, few, "few")                                                      \
  MACRO_(fieldOffsets, fieldOffsets, "fieldOffsets")                           \
  MACRO_(fieldTypes, fieldTypes, "fieldTypes")                                 \
  MACRO_(fileName, fileName, "fileName")                                       \
  MACRO_(fill, fill, "fill")                                                   \
  MACRO_(finally, finally_, "finally")                                         \
  MACRO_(find, find, "find")                                                   \
  MACRO_(findIndex, findIndex, "findIndex")                                    \
  MACRO_(firstDayOfWeek, firstDayOfWeek, "firstDayOfWeek")                     \
  MACRO_(fix, fix, "fix")                                                      \
  MACRO_(flags, flags, "flags")                                                \
  MACRO_(flat, flat, "flat")                                                   \
  MACRO_(flatMap, flatMap, "flatMap")                                          \
  MACRO_(float32, float32, "float32")                                          \
  MACRO_(float64, float64, "float64")                                          \
    MACRO_(for, for_, "for")                                                   \
  MACRO_(forceInterpreter, forceInterpreter, "forceInterpreter")               \
  MACRO_(forEach, forEach, "forEach")                                          \
  MACRO_(format, format, "format")                                             \
  MACRO_(fraction, fraction, "fraction")                                       \
  MACRO_(fractionalSecond, fractionalSecond, "fractionalSecond")               \
  MACRO_(frame, frame, "frame")                                                \
  MACRO_(from, from, "from")                                                   \
  MACRO_(fulfilled, fulfilled, "fulfilled")                                    \
  MACRO_(futexNotEqual, futexNotEqual, "not-equal")                            \
  MACRO2(futexOK, futexOK, "ok")                                               \
  MACRO_(futexTimedOut, futexTimedOut, "timed-out")                            \
  MACRO_(gcCycleNumber, gcCycleNumber, "gcCycleNumber")                        \
  MACRO_(GatherAsyncParentCompletions, GatherAsyncParentCompletions,           \
         "GatherAsyncParentCompletions")                                       \
  MACRO_(Generator, Generator, "Generator")                                    \
  MACRO_(GeneratorNext, GeneratorNext, "GeneratorNext")                        \
  MACRO_(GeneratorReturn, GeneratorReturn, "GeneratorReturn")                  \
  MACRO_(GeneratorThrow, GeneratorThrow, "GeneratorThrow")                     \
  MACRO_(get, get, "get")                                                      \
  MACRO_(GetAggregateError, GetAggregateError, "GetAggregateError")            \
  MACRO_(GetBuiltinConstructor, GetBuiltinConstructor,                         \
         "GetBuiltinConstructor")                                              \
  MACRO_(GetBuiltinPrototype, GetBuiltinPrototype, "GetBuiltinPrototype")      \
  MACRO_(GetBuiltinSymbol, GetBuiltinSymbol, "GetBuiltinSymbol")               \
  MACRO_(GetInternalError, GetInternalError, "GetInternalError")               \
  MACRO_(getBigInt64, getBigInt64, "getBigInt64")                              \
  MACRO_(getBigUint64, getBigUint64, "getBigUint64")                           \
  MACRO_(getInternals, getInternals, "getInternals")                           \
  MACRO_(GetModuleNamespace, GetModuleNamespace, "GetModuleNamespace")         \
  MACRO_(getOwnPropertyDescriptor, getOwnPropertyDescriptor,                   \
         "getOwnPropertyDescriptor")                                           \
  MACRO_(getOwnPropertyNames, getOwnPropertyNames, "getOwnPropertyNames")      \
  MACRO_(getPropertySuper, getPropertySuper, "getPropertySuper")               \
  MACRO_(getPrototypeOf, getPrototypeOf, "getPrototypeOf")                     \
  MACRO_(GetTypeError, GetTypeError, "GetTypeError")                           \
  MACRO_(global, global, "global")                                             \
  MACRO_(globalThis, globalThis, "globalThis")                                 \
  MACRO_(group, group, "group")                                                \
  MACRO_(groups, groups, "groups")                                             \
  MACRO_(h11, h11, "h11")                                                      \
  MACRO_(h12, h12, "h12")                                                      \
  MACRO_(h23, h23, "h23")                                                      \
  MACRO_(h24, h24, "h24")                                                      \
  MACRO_(Handle, Handle, "Handle")                                             \
  MACRO_(has, has, "has")                                                      \
  MACRO_(hashConstructor, hashConstructor, "#constructor")                     \
  MACRO_(hasIndices, hasIndices, "hasIndices")                                 \
  MACRO_(hasOwn, hasOwn, "hasOwn")                                             \
  MACRO_(hasOwnProperty, hasOwnProperty, "hasOwnProperty")                     \
  MACRO_(highWaterMark, highWaterMark, "highWaterMark")                        \
  MACRO_(hour, hour, "hour")                                                   \
  MACRO_(hourCycle, hourCycle, "hourCycle")                                    \
  MACRO2(if, if_, "if")                                                        \
  MACRO_(ignoreCase, ignoreCase, "ignoreCase")                                 \
  MACRO_(ignorePunctuation, ignorePunctuation, "ignorePunctuation")            \
  MACRO_(implements, implements, "implements")                                 \
  MACRO_(import, import, "import")                                             \
  MACRO2(in, in, "in")                                                         \
  MACRO_(includes, includes, "includes")                                       \
  MACRO_(incumbentGlobal, incumbentGlobal, "incumbentGlobal")                  \
  MACRO_(index, index, "index")                                                \
  MACRO_(indices, indices, "indices")                                          \
  MACRO_(infinity, infinity, "infinity")                                       \
  MACRO_(Infinity, Infinity, "Infinity")                                       \
  MACRO_(initial, initial, "initial")                                          \
  MACRO_(InitializeCollator, InitializeCollator, "InitializeCollator")         \
  MACRO_(InitializeDateTimeFormat, InitializeDateTimeFormat,                   \
         "InitializeDateTimeFormat")                                           \
  MACRO_(InitializeDisplayNames, InitializeDisplayNames,                       \
         "InitializeDisplayNames")                                             \
  MACRO_(InitializeListFormat, InitializeListFormat, "InitializeListFormat")   \
  MACRO_(InitializeLocale, InitializeLocale, "InitializeLocale")               \
  MACRO_(InitializeNumberFormat, InitializeNumberFormat,                       \
         "InitializeNumberFormat")                                             \
  MACRO_(InitializePluralRules, InitializePluralRules,                         \
         "InitializePluralRules")                                              \
  MACRO_(InitializeRelativeTimeFormat, InitializeRelativeTimeFormat,           \
         "InitializeRelativeTimeFormat")                                       \
  MACRO_(innermost, innermost, "innermost")                                    \
  MACRO_(inNursery, inNursery, "inNursery")                                    \
  MACRO_(input, input, "input")                                                \
  MACRO_(instanceof, instanceof, "instanceof")                                 \
  MACRO_(int8, int8, "int8")                                                   \
  MACRO_(int16, int16, "int16")                                                \
  MACRO_(int32, int32, "int32")                                                \
  MACRO_(integer, integer, "integer")                                          \
  MACRO_(interface, interface, "interface")                                    \
  MACRO_(InterpretGeneratorResume, InterpretGeneratorResume,                   \
         "InterpretGeneratorResume")                                           \
  MACRO_(InvalidDate, InvalidDate, "Invalid Date")                             \
  MACRO_(isBreakpoint, isBreakpoint, "isBreakpoint")                           \
  MACRO_(isEntryPoint, isEntryPoint, "isEntryPoint")                           \
  MACRO_(isExtensible, isExtensible, "isExtensible")                           \
  MACRO_(isFinite, isFinite, "isFinite")                                       \
  MACRO_(isNaN, isNaN, "isNaN")                                                \
  MACRO_(isPrototypeOf, isPrototypeOf, "isPrototypeOf")                        \
  MACRO_(isStepStart, isStepStart, "isStepStart")                              \
  MACRO_(IterableToList, IterableToList, "IterableToList")                     \
  MACRO_(iterate, iterate, "iterate")                                          \
  MACRO_(join, join, "join")                                                   \
  MACRO2(js, js, "js")                                                         \
  MACRO_(keys, keys, "keys")                                                   \
  MACRO_(label, label, "label")                                                \
  MACRO_(language, language, "language")                                       \
  MACRO_(lastIndex, lastIndex, "lastIndex")                                    \
  MACRO_(length, length, "length")                                             \
  MACRO_(let, let, "let")                                                      \
  MACRO_(line, line, "line")                                                   \
  MACRO_(lineNumber, lineNumber, "lineNumber")                                 \
  MACRO_(literal, literal, "literal")                                          \
  MACRO_(loc, loc, "loc")                                                      \
  MACRO_(locale, locale, "locale")                                             \
  MACRO_(lookupGetter, lookupGetter, "__lookupGetter__")                       \
  MACRO_(lookupSetter, lookupSetter, "__lookupSetter__")                       \
  MACRO_(ltr, ltr, "ltr")                                                      \
  MACRO_(many, many, "many")                                                   \
  MACRO_(MapConstructorInit, MapConstructorInit, "MapConstructorInit")         \
  MACRO_(MapIterator, MapIterator, "Map Iterator")                             \
  MACRO_(maxColumn, maxColumn, "maxColumn")                                    \
  MACRO_(maximum, maximum, "maximum")                                          \
  MACRO_(maximumFractionDigits, maximumFractionDigits,                         \
         "maximumFractionDigits")                                              \
  MACRO_(maximumSignificantDigits, maximumSignificantDigits,                   \
         "maximumSignificantDigits")                                           \
  MACRO_(maxLine, maxLine, "maxLine")                                          \
  MACRO_(maxOffset, maxOffset, "maxOffset")                                    \
  MACRO_(message, message, "message")                                          \
  MACRO_(meta, meta, "meta")                                                   \
  MACRO_(minColumn, minColumn, "minColumn")                                    \
  MACRO_(minDays, minDays, "minDays")                                          \
  MACRO_(minimum, minimum, "minimum")                                          \
  MACRO_(minimumFractionDigits, minimumFractionDigits,                         \
         "minimumFractionDigits")                                              \
  MACRO_(minimumIntegerDigits, minimumIntegerDigits, "minimumIntegerDigits")   \
  MACRO_(minimumSignificantDigits, minimumSignificantDigits,                   \
         "minimumSignificantDigits")                                           \
  MACRO_(minLine, minLine, "minLine")                                          \
  MACRO_(minOffset, minOffset, "minOffset")                                    \
  MACRO_(minusSign, minusSign, "minusSign")                                    \
  MACRO_(minute, minute, "minute")                                             \
  MACRO_(missingArguments, missingArguments, "missingArguments")               \
  MACRO_(mode, mode, "mode")                                                   \
  MACRO_(module, module, "module")                                             \
  MACRO_(Module, Module, "Module")                                             \
  MACRO_(ModuleInstantiate, ModuleInstantiate, "ModuleInstantiate")            \
  MACRO_(ModuleEvaluate, ModuleEvaluate, "ModuleEvaluate")                     \
  MACRO_(month, month, "month")                                                \
  MACRO_(multiline, multiline, "multiline")                                    \
  MACRO_(mutable, mutable_, "mutable")                                         \
  MACRO_(name, name, "name")                                                   \
  MACRO_(nan, nan, "nan")                                                      \
  MACRO_(NaN, NaN, "NaN")                                                      \
  MACRO_(NegativeInfinity, NegativeInfinity, "-Infinity")                      \
  MACRO_(new, new_, "new")                                                     \
  MACRO_(NewPrivateName, NewPrivateName, "NewPrivateName")                     \
  MACRO_(next, next, "next")                                                   \
  MACRO_(NFC, NFC, "NFC")                                                      \
  MACRO_(NFD, NFD, "NFD")                                                      \
  MACRO_(NFKC, NFKC, "NFKC")                                                   \
  MACRO_(NFKD, NFKD, "NFKD")                                                   \
  MACRO_(noFilename, noFilename, "noFilename")                                 \
  MACRO_(nonincrementalReason, nonincrementalReason, "nonincrementalReason")   \
  MACRO_(NoPrivateGetter, NoPrivateGetter, "NoPrivateGetter")                  \
  MACRO_(noStack, noStack, "noStack")                                          \
  MACRO_(notation, notation, "notation")                                       \
  MACRO_(notes, notes, "notes")                                                \
  MACRO_(numberingSystem, numberingSystem, "numberingSystem")                  \
  MACRO_(numeric, numeric, "numeric")                                          \
  MACRO_(objectArguments, objectArguments, "[object Arguments]")               \
  MACRO_(objectArray, objectArray, "[object Array]")                           \
  MACRO_(objectBigInt, objectBigInt, "[object BigInt]")                        \
  MACRO_(objectBoolean, objectBoolean, "[object Boolean]")                     \
  MACRO_(objectDate, objectDate, "[object Date]")                              \
  MACRO_(objectError, objectError, "[object Error]")                           \
  MACRO_(objectFunction, objectFunction, "[object Function]")                  \
  MACRO_(objectNull, objectNull, "[object Null]")                              \
  MACRO_(objectNumber, objectNumber, "[object Number]")                        \
  MACRO_(objectObject, objectObject, "[object Object]")                        \
  MACRO_(objectRegExp, objectRegExp, "[object RegExp]")                        \
  MACRO_(objects, objects, "objects")                                          \
  MACRO_(objectString, objectString, "[object String]")                        \
  MACRO_(objectSymbol, objectSymbol, "[object Symbol]")                        \
  MACRO_(objectUndefined, objectUndefined, "[object Undefined]")               \
  MACRO2(of, of, "of")                                                         \
  MACRO_(offset, offset, "offset")                                             \
  MACRO_(one, one, "one")                                                      \
  MACRO_(optimizedOut, optimizedOut, "optimizedOut")                           \
  MACRO_(other, other, "other")                                                \
  MACRO_(outOfMemory, outOfMemory, "out of memory")                            \
  MACRO_(ownKeys, ownKeys, "ownKeys")                                          \
  MACRO_(Object_valueOf, Object_valueOf, "Object_valueOf")                     \
  MACRO_(package, package, "package")                                          \
  MACRO_(parseFloat, parseFloat, "parseFloat")                                 \
  MACRO_(parseInt, parseInt, "parseInt")                                       \
  MACRO_(pattern, pattern, "pattern")                                          \
  MACRO_(pending, pending, "pending")                                          \
  MACRO_(percentSign, percentSign, "percentSign")                              \
  MACRO_(pipeTo, pipeTo, "pipeTo")                                             \
  MACRO_(plusSign, plusSign, "plusSign")                                       \
  MACRO_(public, public_, "public")                                            \
  MACRO_(pull, pull, "pull")                                                   \
  MACRO_(preventAbort, preventAbort, "preventAbort")                           \
  MACRO_(preventClose, preventClose, "preventClose")                           \
  MACRO_(preventCancel, preventCancel, "preventCancel")                        \
  MACRO_(preventExtensions, preventExtensions, "preventExtensions")            \
  MACRO_(private, private_, "private")                                         \
  MACRO_(promise, promise, "promise")                                          \
  MACRO_(propertyIsEnumerable, propertyIsEnumerable, "propertyIsEnumerable")   \
  MACRO_(protected, protected_, "protected")                                   \
  MACRO_(proto, proto, "__proto__")                                            \
  MACRO_(prototype, prototype, "prototype")                                    \
  MACRO_(proxy, proxy, "proxy")                                                \
  MACRO_(quarter, quarter, "quarter")                                          \
  MACRO_(raw, raw, "raw")                                                      \
  MACRO_(reason, reason, "reason")                                             \
  MACRO_(RegExpFlagsGetter, RegExpFlagsGetter, "$RegExpFlagsGetter")           \
  MACRO_(RegExpStringIterator, RegExpStringIterator, "RegExp String Iterator") \
  MACRO_(RegExpToString, RegExpToString, "$RegExpToString")                    \
  MACRO_(region, region, "region")                                             \
  MACRO_(register, register_, "register")                                      \
  MACRO_(Reify, Reify, "Reify")                                                \
  MACRO_(reject, reject, "reject")                                             \
  MACRO_(rejected, rejected, "rejected")                                       \
  MACRO_(relatedYear, relatedYear, "relatedYear")                              \
  MACRO_(RelativeTimeFormatFormat, RelativeTimeFormatFormat,                   \
         "Intl_RelativeTimeFormat_Format")                                     \
  MACRO_(RequireObjectCoercible, RequireObjectCoercible,                       \
         "RequireObjectCoercible")                                             \
  MACRO_(resolve, resolve, "resolve")                                          \
  MACRO_(result, result, "result")                                             \
  MACRO_(resumeGenerator, resumeGenerator, "resumeGenerator")                  \
  MACRO_(return, return_, "return")                                            \
  MACRO_(revoke, revoke, "revoke")                                             \
  MACRO_(rtl, rtl, "rtl")                                                      \
  MACRO_(script, script, "script")                                             \
  MACRO_(scripts, scripts, "scripts")                                          \
  MACRO_(second, second, "second")                                             \
  MACRO_(selfHosted, selfHosted, "self-hosted")                                \
  MACRO_(sensitivity, sensitivity, "sensitivity")                              \
  MACRO_(set, set, "set")                                                      \
  MACRO_(setBigInt64, setBigInt64, "setBigInt64")                              \
  MACRO_(setBigUint64, setBigUint64, "setBigUint64")                           \
  MACRO_(SetCanonicalName, SetCanonicalName, "SetCanonicalName")               \
  MACRO_(SetConstructorInit, SetConstructorInit, "SetConstructorInit")         \
  MACRO_(SetIsInlinableLargeFunction, SetIsInlinableLargeFunction,             \
         "SetIsInlinableLargeFunction")                                        \
  MACRO_(SetIterator, SetIterator, "Set Iterator")                             \
  MACRO_(setPrototypeOf, setPrototypeOf, "setPrototypeOf")                     \
  MACRO_(shape, shape, "shape")                                                \
  MACRO_(shared, shared, "shared")                                             \
  MACRO_(signal, signal, "signal")                                             \
  MACRO_(signDisplay, signDisplay, "signDisplay")                              \
  MACRO_(size, size, "size")                                                   \
  MACRO_(skeleton, skeleton, "skeleton")                                       \
  MACRO_(source, source, "source")                                             \
  MACRO_(SpeciesConstructor, SpeciesConstructor, "SpeciesConstructor")         \
  MACRO_(stack, stack, "stack")                                                \
  MACRO1(star, star, "*")                                                      \
  MACRO_(starNamespaceStar, starNamespaceStar, "*namespace*")                  \
  MACRO_(start, start, "start")                                                \
  MACRO_(startRange, startRange, "startRange")                                 \
  MACRO_(startTimestamp, startTimestamp, "startTimestamp")                     \
  MACRO_(state, state, "state")                                                \
  MACRO_(static, static_, "static")                                            \
  MACRO_(status, status, "status")                                             \
  MACRO_(std_Function_apply, std_Function_apply, "std_Function_apply")         \
  MACRO_(sticky, sticky, "sticky")                                             \
  MACRO_(StringIterator, StringIterator, "String Iterator")                    \
  MACRO_(strings, strings, "strings")                                          \
  MACRO_(String_split, String_split, "String_split")                           \
  MACRO_(StructType, StructType, "StructType")                                 \
  MACRO_(style, style, "style")                                                \
  MACRO_(super, super, "super")                                                \
  MACRO_(switch, switch_, "switch")                                            \
  MACRO_(Symbol_iterator_fun, Symbol_iterator_fun, "[Symbol.iterator]")        \
  MACRO_(target, target, "target")                                             \
  MACRO_(test, test, "test")                                                   \
  MACRO_(then, then, "then")                                                   \
  MACRO_(this, this_, "this")                                                  \
  MACRO_(throw, throw_, "throw")                                               \
  MACRO_(timestamp, timestamp, "timestamp")                                    \
  MACRO_(timeZone, timeZone, "timeZone")                                       \
  MACRO_(timeZoneName, timeZoneName, "timeZoneName")                           \
  MACRO_(trimEnd, trimEnd, "trimEnd")                                          \
  MACRO_(trimLeft, trimLeft, "trimLeft")                                       \
  MACRO_(trimRight, trimRight, "trimRight")                                    \
  MACRO_(trimStart, trimStart, "trimStart")                                    \
  MACRO_(toGMTString, toGMTString, "toGMTString")                              \
  MACRO_(toISOString, toISOString, "toISOString")                              \
  MACRO_(toJSON, toJSON, "toJSON")                                             \
  MACRO_(toLocaleString, toLocaleString, "toLocaleString")                     \
  MACRO_(ToNumeric, ToNumeric, "ToNumeric")                                    \
  MACRO_(toSource, toSource, "toSource")                                       \
  MACRO_(toString, toString, "toString")                                       \
  MACRO_(ToString, ToString, "ToString")                                       \
  MACRO_(toUTCString, toUTCString, "toUTCString")                              \
  MACRO_(true, true_, "true")                                                  \
  MACRO_(try, try_, "try")                                                     \
  MACRO_(two, two, "two")                                                      \
  MACRO_(type, type, "type")                                                   \
  MACRO_(typeof, typeof_, "typeof")                                            \
  MACRO_(uint8, uint8, "uint8")                                                \
  MACRO_(uint8Clamped, uint8Clamped, "uint8Clamped")                           \
  MACRO_(uint16, uint16, "uint16")                                             \
  MACRO_(uint32, uint32, "uint32")                                             \
  MACRO_(Uint8x16, Uint8x16, "Uint8x16")                                       \
  MACRO_(Uint16x8, Uint16x8, "Uint16x8")                                       \
  MACRO_(Uint32x4, Uint32x4, "Uint32x4")                                       \
  MACRO_(unescape, unescape, "unescape")                                       \
  MACRO_(uneval, uneval, "uneval")                                             \
  MACRO_(unicode, unicode, "unicode")                                          \
  MACRO_(unit, unit, "unit")                                                   \
  MACRO_(unitDisplay, unitDisplay, "unitDisplay")                              \
  MACRO_(uninitialized, uninitialized, "uninitialized")                        \
  MACRO_(unknown, unknown, "unknown")                                          \
  MACRO_(unregister, unregister, "unregister")                                 \
  MACRO_(UnsafeGetReservedSlot, UnsafeGetReservedSlot,                         \
         "UnsafeGetReservedSlot")                                              \
  MACRO_(UnsafeGetObjectFromReservedSlot, UnsafeGetObjectFromReservedSlot,     \
         "UnsafeGetObjectFromReservedSlot")                                    \
  MACRO_(UnsafeGetInt32FromReservedSlot, UnsafeGetInt32FromReservedSlot,       \
         "UnsafeGetInt32FromReservedSlot")                                     \
  MACRO_(UnsafeGetStringFromReservedSlot, UnsafeGetStringFromReservedSlot,     \
         "UnsafeGetStringFromReservedSlot")                                    \
  MACRO_(UnsafeGetBooleanFromReservedSlot, UnsafeGetBooleanFromReservedSlot,   \
         "UnsafeGetBooleanFromReservedSlot")                                   \
  MACRO_(UnsafeSetReservedSlot, UnsafeSetReservedSlot,                         \
         "UnsafeSetReservedSlot")                                              \
  MACRO_(unsized, unsized, "unsized")                                          \
  MACRO_(unwatch, unwatch, "unwatch")                                          \
  MACRO_(url, url, "url")                                                      \
  MACRO_(usage, usage, "usage")                                                \
  MACRO_(useAsm, useAsm, "use asm")                                            \
  MACRO_(useGrouping, useGrouping, "useGrouping")                              \
  MACRO_(useStrict, useStrict, "use strict")                                   \
  MACRO_(void, void_, "void")                                                  \
  MACRO_(value, value, "value")                                                \
  MACRO_(valueOf, valueOf, "valueOf")                                          \
  MACRO_(values, values, "values")                                             \
  MACRO_(var, var, "var")                                                      \
  MACRO_(variable, variable, "variable")                                       \
  MACRO_(void0, void0, "(void 0)")                                             \
  MACRO_(wasm, wasm, "wasm")                                                   \
  MACRO_(WasmAnyRef, WasmAnyRef, "WasmAnyRef")                                 \
  MACRO_(wasmcall, wasmcall, "wasmcall")                                       \
  MACRO_(watch, watch, "watch")                                                \
  MACRO_(WeakMapConstructorInit, WeakMapConstructorInit,                       \
         "WeakMapConstructorInit")                                             \
  MACRO_(WeakSetConstructorInit, WeakSetConstructorInit,                       \
         "WeakSetConstructorInit")                                             \
  MACRO_(WeakSet_add, WeakSet_add, "WeakSet_add")                              \
  MACRO_(week, week, "week")                                                   \
  MACRO_(weekday, weekday, "weekday")                                          \
  MACRO_(weekendEnd, weekendEnd, "weekendEnd")                                 \
  MACRO_(weekendStart, weekendStart, "weekendStart")                           \
  MACRO_(while, while_, "while")                                               \
  MACRO_(with, with, "with")                                                   \
  MACRO_(writable, writable, "writable")                                       \
  MACRO_(write, write, "write")                                                \
  MACRO_(year, year, "year")                                                   \
  MACRO_(yearName, yearName, "yearName")                                       \
  MACRO_(yield, yield, "yield")                                                \
  MACRO_(zero, zero, "zero")                                                   \
  /* Type names must be contiguous and ordered; see js::TypeName. */           \
  MACRO_(undefined, undefined, "undefined")                                    \
  MACRO_(object, object, "object")                                             \
  MACRO_(function, function, "function")                                       \
  MACRO_(string, string, "string")                                             \
  MACRO_(number, number, "number")                                             \
  MACRO_(boolean, boolean, "boolean")                                          \
  MACRO_(null, null, "null")                                                   \
  MACRO_(symbol, symbol, "symbol")                                             \
  MACRO_(bigint, bigint, "bigint")                                             \
  MACRO_(defineDataPropertyIntrinsic, defineDataPropertyIntrinsic,             \
         "DefineDataProperty")

#define PROPERTY_NAME_IGNORE(IDPART, ID, TEXT)

#define FOR_EACH_LENGTH1_PROPERTYNAME(MACRO)                 \
  FOR_EACH_COMMON_PROPERTYNAME_(PROPERTY_NAME_IGNORE, MACRO, \
                                PROPERTY_NAME_IGNORE, PROPERTY_NAME_IGNORE)

#define FOR_EACH_LENGTH2_PROPERTYNAME(MACRO)                                \
  FOR_EACH_COMMON_PROPERTYNAME_(PROPERTY_NAME_IGNORE, PROPERTY_NAME_IGNORE, \
                                MACRO, PROPERTY_NAME_IGNORE)

#define FOR_EACH_NON_EMPTY_TINY_PROPERTYNAME(MACRO)                 \
  FOR_EACH_COMMON_PROPERTYNAME_(PROPERTY_NAME_IGNORE, MACRO, MACRO, \
                                PROPERTY_NAME_IGNORE)

#define FOR_EACH_TINY_PROPERTYNAME(MACRO) \
  FOR_EACH_COMMON_PROPERTYNAME_(MACRO, MACRO, MACRO, PROPERTY_NAME_IGNORE)

#define FOR_EACH_NONTINY_COMMON_PROPERTYNAME(MACRO)                         \
  FOR_EACH_COMMON_PROPERTYNAME_(PROPERTY_NAME_IGNORE, PROPERTY_NAME_IGNORE, \
                                PROPERTY_NAME_IGNORE, MACRO)

#define FOR_EACH_COMMON_PROPERTYNAME(MACRO) \
  FOR_EACH_COMMON_PROPERTYNAME_(MACRO, MACRO, MACRO, MACRO)

#endif /* vm_CommonPropertyNames_h */
