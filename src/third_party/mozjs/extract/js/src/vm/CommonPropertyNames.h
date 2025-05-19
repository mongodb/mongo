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
// Parameter list is (ID, TEXT).
//
// Each entry should use one of MACRO* based on the length of TEXT
//   * MACRO0: length-0 text
//   * MACRO1: length-1 text
//   * MACRO2: length-2 text
//   * MACRO_: other text
#define FOR_EACH_COMMON_PROPERTYNAME_(MACRO0, MACRO1, MACRO2, MACRO_)          \
  MACRO_(abort, "abort")                                                       \
  IF_DECORATORS(MACRO_(access, "access"))                                      \
  IF_DECORATORS(MACRO_(accessor, "accessor"))                                  \
  MACRO_(add, "add")                                                           \
  IF_DECORATORS(MACRO_(addInitializer, "addInitializer"))                      \
  MACRO_(allowContentIter, "allowContentIter")                                 \
  MACRO_(allowContentIterWith, "allowContentIterWith")                         \
  MACRO_(allowContentIterWithNext, "allowContentIterWithNext")                 \
  MACRO_(alphabet, "alphabet")                                                 \
  MACRO_(ambiguous, "ambiguous")                                               \
  MACRO_(anonymous, "anonymous")                                               \
  MACRO_(Any, "Any")                                                           \
  MACRO_(any, "any")                                                           \
  MACRO_(apply, "apply")                                                       \
  MACRO_(approximatelySign, "approximatelySign")                               \
  MACRO_(arguments, "arguments")                                               \
  MACRO_(ArgumentsLength, "ArgumentsLength")                                   \
  MACRO_(Array_Iterator_, "Array Iterator")                                    \
  MACRO_(ArrayIteratorNext, "ArrayIteratorNext")                               \
  MACRO_(ArraySort, "ArraySort")                                               \
  MACRO_(ArraySpeciesCreate, "ArraySpeciesCreate")                             \
  MACRO_(ArrayToLocaleString, "ArrayToLocaleString")                           \
  MACRO_(ArrayType, "ArrayType")                                               \
  MACRO2(as, "as")                                                             \
  MACRO_(assert_, "assert")                                                    \
  MACRO_(Async, "Async")                                                       \
  MACRO_(async, "async")                                                       \
  MACRO_(Async_from_Sync_Iterator_, "Async-from-Sync Iterator")                \
  MACRO_(AsyncFunctionNext, "AsyncFunctionNext")                               \
  MACRO_(AsyncFunctionThrow, "AsyncFunctionThrow")                             \
  MACRO_(AsyncGenerator, "AsyncGenerator")                                     \
  MACRO_(AsyncGeneratorNext, "AsyncGeneratorNext")                             \
  MACRO_(AsyncGeneratorReturn, "AsyncGeneratorReturn")                         \
  MACRO_(AsyncGeneratorThrow, "AsyncGeneratorThrow")                           \
  MACRO_(AsyncWrapped, "AsyncWrapped")                                         \
  MACRO2(at, "at")                                                             \
  MACRO_(autoAllocateChunkSize, "autoAllocateChunkSize")                       \
  MACRO_(await, "await")                                                       \
  MACRO_(bigint64, "bigint64")                                                 \
  MACRO_(biguint64, "biguint64")                                               \
  MACRO_(bound, "bound")                                                       \
  MACRO_(boundWithSpace_, "bound ")                                            \
  MACRO_(break_, "break")                                                      \
  MACRO_(breakdown, "breakdown")                                               \
  MACRO_(buffer, "buffer")                                                     \
  MACRO_(builder, "builder")                                                   \
  MACRO2(by, "by")                                                             \
  MACRO_(byob, "byob")                                                         \
  MACRO_(byteAlignment, "byteAlignment")                                       \
  MACRO_(byteLength, "byteLength")                                             \
  MACRO_(byteOffset, "byteOffset")                                             \
  MACRO_(bytes, "bytes")                                                       \
  MACRO_(BYTES_PER_ELEMENT, "BYTES_PER_ELEMENT")                               \
  MACRO_(calendar, "calendar")                                                 \
  MACRO_(calendarName, "calendarName")                                         \
  MACRO_(call, "call")                                                         \
  MACRO_(callContentFunction, "callContentFunction")                           \
  MACRO_(callee, "callee")                                                     \
  MACRO_(caller, "caller")                                                     \
  MACRO_(callFunction, "callFunction")                                         \
  MACRO_(cancel, "cancel")                                                     \
  MACRO_(case_, "case")                                                        \
  MACRO_(caseFirst, "caseFirst")                                               \
  MACRO_(catch_, "catch")                                                      \
  MACRO_(cause, "cause")                                                       \
  MACRO_(class_, "class")                                                      \
  MACRO_(cleanupSome, "cleanupSome")                                           \
  MACRO_(close, "close")                                                       \
  MACRO_(collation, "collation")                                               \
  MACRO_(collections, "collections")                                           \
  MACRO_(columnNumber, "columnNumber")                                         \
  MACRO1(comma_, ",")                                                          \
  MACRO_(compact, "compact")                                                   \
  MACRO_(compactDisplay, "compactDisplay")                                     \
  MACRO_(compare, "compare")                                                   \
  MACRO_(configurable, "configurable")                                         \
  MACRO_(const_, "const")                                                      \
  MACRO_(constrain, "constrain")                                               \
  MACRO_(construct, "construct")                                               \
  MACRO_(constructContentFunction, "constructContentFunction")                 \
  MACRO_(constructor, "constructor")                                           \
  MACRO_(continue_, "continue")                                                \
  MACRO_(CopyDataProperties, "CopyDataProperties")                             \
  MACRO_(CopyDataPropertiesUnfiltered, "CopyDataPropertiesUnfiltered")         \
  MACRO_(copyWithin, "copyWithin")                                             \
  MACRO_(count, "count")                                                       \
  MACRO_(CreateResolvingFunctions, "CreateResolvingFunctions")                 \
  MACRO_(currency, "currency")                                                 \
  MACRO_(currencyDisplay, "currencyDisplay")                                   \
  MACRO_(currencySign, "currencySign")                                         \
  MACRO_(date, "date")                                                         \
  MACRO_(dateAdd, "dateAdd")                                                   \
  MACRO_(dateFromFields, "dateFromFields")                                     \
  MACRO_(dateStyle, "dateStyle")                                               \
  MACRO_(dateUntil, "dateUntil")                                               \
  MACRO_(day, "day")                                                           \
  MACRO_(dayOfWeek, "dayOfWeek")                                               \
  MACRO_(dayOfYear, "dayOfYear")                                               \
  MACRO_(dayPeriod, "dayPeriod")                                               \
  MACRO_(days, "days")                                                         \
  MACRO_(daysInMonth, "daysInMonth")                                           \
  MACRO_(daysInWeek, "daysInWeek")                                             \
  MACRO_(daysInYear, "daysInYear")                                             \
  MACRO_(debugger, "debugger")                                                 \
  MACRO_(decimal, "decimal")                                                   \
  MACRO_(decodeURI, "decodeURI")                                               \
  MACRO_(decodeURIComponent, "decodeURIComponent")                             \
  MACRO_(default_, "default")                                                  \
  MACRO_(DefineDataProperty, "DefineDataProperty")                             \
  MACRO_(defineGetter_, "__defineGetter__")                                    \
  MACRO_(defineProperty, "defineProperty")                                     \
  MACRO_(defineSetter_, "__defineSetter__")                                    \
  MACRO_(delete_, "delete")                                                    \
  MACRO_(deleteProperty, "deleteProperty")                                     \
  MACRO_(detached, "detached")                                                 \
  MACRO_(difference, "difference")                                             \
  MACRO_(disambiguation, "disambiguation")                                     \
  MACRO_(displayURL, "displayURL")                                             \
  MACRO2(do_, "do")                                                            \
  MACRO_(dollar_ArrayBufferSpecies_, "$ArrayBufferSpecies")                    \
  MACRO_(dollar_ArraySpecies_, "$ArraySpecies")                                \
  MACRO_(dollar_ArrayValues_, "$ArrayValues")                                  \
  MACRO_(dollar_RegExpFlagsGetter_, "$RegExpFlagsGetter")                      \
  MACRO_(dollar_RegExpToString_, "$RegExpToString")                            \
  MACRO_(domNode, "domNode")                                                   \
  MACRO_(done, "done")                                                         \
  MACRO_(dotAll, "dotAll")                                                     \
  MACRO_(dot_args_, ".args")                                                   \
  MACRO_(dot_fieldKeys_, ".fieldKeys")                                         \
  MACRO_(dot_generator_, ".generator")                                         \
  MACRO_(dot_initializers_, ".initializers")                                   \
  IF_DECORATORS(                                                               \
      MACRO_(dot_instanceExtraInitializers_, ".instanceExtraInitializers"))    \
  MACRO_(dot_newTarget_, ".newTarget")                                         \
  MACRO_(dot_privateBrand_, ".privateBrand")                                   \
  MACRO_(dot_staticFieldKeys_, ".staticFieldKeys")                             \
  MACRO_(dot_staticInitializers_, ".staticInitializers")                       \
  MACRO_(dot_this_, ".this")                                                   \
  MACRO_(each, "each")                                                         \
  MACRO_(element, "element")                                                   \
  MACRO_(elementType, "elementType")                                           \
  MACRO_(else_, "else")                                                        \
  MACRO0(empty_, "")                                                           \
  MACRO_(emptyRegExp_, "(?:)")                                                 \
  MACRO_(encodeURI, "encodeURI")                                               \
  MACRO_(encodeURIComponent, "encodeURIComponent")                             \
  MACRO_(endRange, "endRange")                                                 \
  MACRO_(endTimestamp, "endTimestamp")                                         \
  MACRO_(entries, "entries")                                                   \
  MACRO_(enum_, "enum")                                                        \
  MACRO_(enumerable, "enumerable")                                             \
  MACRO_(enumerate, "enumerate")                                               \
  MACRO_(era, "era")                                                           \
  MACRO_(eraYear, "eraYear")                                                   \
  MACRO_(errors, "errors")                                                     \
  MACRO_(ErrorToStringWithTrailingNewline, "ErrorToStringWithTrailingNewline") \
  MACRO_(escape, "escape")                                                     \
  MACRO_(eval, "eval")                                                         \
  MACRO_(exec, "exec")                                                         \
  MACRO_(exponentInteger, "exponentInteger")                                   \
  MACRO_(exponentMinusSign, "exponentMinusSign")                               \
  MACRO_(exponentSeparator, "exponentSeparator")                               \
  MACRO_(export_, "export")                                                    \
  MACRO_(extends, "extends")                                                   \
  MACRO_(f16round, "f16round")                                                 \
  MACRO_(false_, "false")                                                      \
  MACRO_(few, "few")                                                           \
  IF_DECORATORS(MACRO_(field, "field"))                                        \
  MACRO_(fieldOffsets, "fieldOffsets")                                         \
  MACRO_(fields, "fields")                                                     \
  MACRO_(fieldTypes, "fieldTypes")                                             \
  MACRO_(fileName, "fileName")                                                 \
  MACRO_(fill, "fill")                                                         \
  MACRO_(finally_, "finally")                                                  \
  MACRO_(find, "find")                                                         \
  MACRO_(findIndex, "findIndex")                                               \
  MACRO_(findLast, "findLast")                                                 \
  MACRO_(findLastIndex, "findLastIndex")                                       \
  MACRO_(firstDayOfWeek, "firstDayOfWeek")                                     \
  MACRO_(fix, "fix")                                                           \
  MACRO_(flags, "flags")                                                       \
  MACRO_(flat, "flat")                                                         \
  MACRO_(flatMap, "flatMap")                                                   \
  MACRO_(float32, "float32")                                                   \
  MACRO_(float64, "float64")                                                   \
  MACRO_(for_, "for")                                                          \
  MACRO_(forceInterpreter, "forceInterpreter")                                 \
  MACRO_(forEach, "forEach")                                                   \
  MACRO_(format, "format")                                                     \
  MACRO_(fraction, "fraction")                                                 \
  MACRO_(fractionalSecond, "fractionalSecond")                                 \
  MACRO_(fractionalSecondDigits, "fractionalSecondDigits")                     \
  MACRO_(frame, "frame")                                                       \
  MACRO_(from, "from")                                                         \
  MACRO_(fromAsync, "fromAsync")                                               \
  MACRO_(fromBase64, "fromBase64")                                             \
  MACRO_(fromHex, "fromHex")                                                   \
  MACRO_(fulfilled, "fulfilled")                                               \
  MACRO_(GatherAsyncParentCompletions, "GatherAsyncParentCompletions")         \
  MACRO_(gcCycleNumber, "gcCycleNumber")                                       \
  MACRO_(Generator, "Generator")                                               \
  MACRO_(GeneratorNext, "GeneratorNext")                                       \
  MACRO_(GeneratorReturn, "GeneratorReturn")                                   \
  MACRO_(GeneratorThrow, "GeneratorThrow")                                     \
  MACRO_(get, "get")                                                           \
  IF_DECORATORS(MACRO_(getter, "getter"))                                      \
  MACRO_(GetAggregateError, "GetAggregateError")                               \
  MACRO_(GetArgument, "GetArgument")                                           \
  MACRO_(getBigInt64, "getBigInt64")                                           \
  MACRO_(getBigUint64, "getBigUint64")                                         \
  MACRO_(GetBuiltinConstructor, "GetBuiltinConstructor")                       \
  MACRO_(GetBuiltinPrototype, "GetBuiltinPrototype")                           \
  MACRO_(GetBuiltinSymbol, "GetBuiltinSymbol")                                 \
  MACRO_(getFloat16, "getFloat16")                                             \
  MACRO_(GetInternalError, "GetInternalError")                                 \
  MACRO_(getInternals, "getInternals")                                         \
  MACRO_(GetIterator, "GetIterator")                                           \
  MACRO_(GetModuleNamespace, "GetModuleNamespace")                             \
  MACRO_(getOffsetNanosecondsFor, "getOffsetNanosecondsFor")                   \
  MACRO_(getOwnPropertyDescriptor, "getOwnPropertyDescriptor")                 \
  MACRO_(getOwnPropertyNames, "getOwnPropertyNames")                           \
  MACRO_(getPossibleInstantsFor, "getPossibleInstantsFor")                     \
  MACRO_(getPropertySuper, "getPropertySuper")                                 \
  MACRO_(getPrototypeOf, "getPrototypeOf")                                     \
  MACRO_(GetTypeError, "GetTypeError")                                         \
  MACRO_(global, "global")                                                     \
  MACRO_(globalThis, "globalThis")                                             \
  MACRO_(granularity, "granularity")                                           \
  MACRO_(group, "group")                                                       \
  MACRO_(groupBy, "groupBy")                                                   \
  MACRO_(groups, "groups")                                                     \
  MACRO_(grow, "grow")                                                         \
  MACRO_(growable, "growable")                                                 \
  MACRO_(h11, "h11")                                                           \
  MACRO_(h12, "h12")                                                           \
  MACRO_(h23, "h23")                                                           \
  MACRO_(h24, "h24")                                                           \
  MACRO_(Handle, "Handle")                                                     \
  MACRO_(has, "has")                                                           \
  MACRO_(hash_constructor_, "#constructor")                                    \
  MACRO_(hasIndices, "hasIndices")                                             \
  MACRO_(hasOwn, "hasOwn")                                                     \
  MACRO_(hasOwnProperty, "hasOwnProperty")                                     \
  MACRO_(highWaterMark, "highWaterMark")                                       \
  MACRO_(hour, "hour")                                                         \
  MACRO_(hour12, "hour12")                                                     \
  MACRO_(hourCycle, "hourCycle")                                               \
  MACRO_(hours, "hours")                                                       \
  MACRO2(id, "id")                                                             \
  MACRO2(if_, "if")                                                            \
  MACRO_(ignoreCase, "ignoreCase")                                             \
  MACRO_(ignorePunctuation, "ignorePunctuation")                               \
  MACRO_(implements, "implements")                                             \
  MACRO_(import, "import")                                                     \
  MACRO2(in, "in")                                                             \
  MACRO_(includes, "includes")                                                 \
  MACRO_(incumbentGlobal, "incumbentGlobal")                                   \
  MACRO_(index, "index")                                                       \
  MACRO_(indices, "indices")                                                   \
  MACRO_(infinity, "infinity")                                                 \
  MACRO_(intersection, "intersection")                                         \
  MACRO_(Infinity, "Infinity")                                                 \
  IF_DECORATORS(MACRO_(init, "init"))                                          \
  MACRO_(initial, "initial")                                                   \
  MACRO_(InitializeCollator, "InitializeCollator")                             \
  MACRO_(InitializeDateTimeFormat, "InitializeDateTimeFormat")                 \
  MACRO_(InitializeDisplayNames, "InitializeDisplayNames")                     \
  MACRO_(InitializeListFormat, "InitializeListFormat")                         \
  MACRO_(InitializeLocale, "InitializeLocale")                                 \
  MACRO_(InitializeNumberFormat, "InitializeNumberFormat")                     \
  MACRO_(InitializePluralRules, "InitializePluralRules")                       \
  MACRO_(InitializeRelativeTimeFormat, "InitializeRelativeTimeFormat")         \
  MACRO_(InitializeSegmenter, "InitializeSegmenter")                           \
  IF_DECORATORS(MACRO_(initializer, "initializer"))                            \
  MACRO_(inLeapYear, "inLeapYear")                                             \
  MACRO_(innermost, "innermost")                                               \
  MACRO_(inNursery, "inNursery")                                               \
  MACRO_(input, "input")                                                       \
  MACRO_(instanceof, "instanceof")                                             \
  MACRO_(int8, "int8")                                                         \
  MACRO_(int16, "int16")                                                       \
  MACRO_(int32, "int32")                                                       \
  MACRO_(integer, "integer")                                                   \
  MACRO_(interface, "interface")                                               \
  MACRO_(InterpretGeneratorResume, "InterpretGeneratorResume")                 \
  MACRO_(Invalid_Date_, "Invalid Date")                                        \
  MACRO_(isBreakpoint, "isBreakpoint")                                         \
  IF_EXPLICIT_RESOURCE_MANAGEMENT_OR_DECORATORS(                               \
      MACRO_(IsCallable, "IsCallable"))                                        \
  MACRO_(isDisjointFrom, "isDisjointFrom")                                     \
  MACRO_(isEntryPoint, "isEntryPoint")                                         \
  MACRO_(isExtensible, "isExtensible")                                         \
  MACRO_(isFinite, "isFinite")                                                 \
  MACRO_(isNaN, "isNaN")                                                       \
  MACRO_(IsNullOrUndefined, "IsNullOrUndefined")                               \
  MACRO_(isPrototypeOf, "isPrototypeOf")                                       \
  MACRO_(iso8601, "iso8601")                                                   \
  MACRO_(isoDay, "isoDay")                                                     \
  MACRO_(isoHour, "isoHour")                                                   \
  MACRO_(isoMicrosecond, "isoMicrosecond")                                     \
  MACRO_(isoMillisecond, "isoMillisecond")                                     \
  MACRO_(isoMinute, "isoMinute")                                               \
  MACRO_(isoMonth, "isoMonth")                                                 \
  MACRO_(isoNanosecond, "isoNanosecond")                                       \
  MACRO_(isRawJSON, "isRawJSON")                                               \
  MACRO_(isoSecond, "isoSecond")                                               \
  MACRO_(isoYear, "isoYear")                                                   \
  MACRO_(isStepStart, "isStepStart")                                           \
  MACRO_(isSubsetOf, "isSubsetOf")                                             \
  MACRO_(isSupersetOf, "isSupersetOf")                                         \
  MACRO_(IterableToList, "IterableToList")                                     \
  MACRO_(IteratorClose, "IteratorClose")                                       \
  MACRO_(Iterator_Helper_, "Iterator Helper")                                  \
  MACRO_(IteratorNext, "IteratorNext")                                         \
  MACRO_(iterate, "iterate")                                                   \
  MACRO_(join, "join")                                                         \
  MACRO2(js, "js")                                                             \
  MACRO_(jsTag, "JSTag")                                                       \
  MACRO_(jsStringModule, "js-string")                                          \
  MACRO_(json, "json")                                                         \
  MACRO_(keys, "keys")                                                         \
  IF_DECORATORS(MACRO_(kind, "kind"))                                          \
  MACRO_(label, "label")                                                       \
  MACRO_(language, "language")                                                 \
  MACRO_(largestUnit, "largestUnit")                                           \
  MACRO_(lastChunkHandling, "lastChunkHandling")                               \
  MACRO_(lastIndex, "lastIndex")                                               \
  MACRO_(length, "length")                                                     \
  MACRO_(let, "let")                                                           \
  MACRO_(line, "line")                                                         \
  MACRO_(lineNumber, "lineNumber")                                             \
  MACRO_(literal, "literal")                                                   \
  MACRO_(loc, "loc")                                                           \
  MACRO_(locale, "locale")                                                     \
  MACRO_(lookupGetter_, "__lookupGetter__")                                    \
  MACRO_(lookupSetter_, "__lookupSetter__")                                    \
  MACRO_(many, "many")                                                         \
  MACRO_(MapConstructorInit, "MapConstructorInit")                             \
  MACRO_(Map_Iterator_, "Map Iterator")                                        \
  MACRO_(maxByteLength, "maxByteLength")                                       \
  MACRO_(maxColumn, "maxColumn")                                               \
  MACRO_(maximum, "maximum")                                                   \
  MACRO_(maximumFractionDigits, "maximumFractionDigits")                       \
  MACRO_(maximumSignificantDigits, "maximumSignificantDigits")                 \
  MACRO_(maxLine, "maxLine")                                                   \
  MACRO_(maxOffset, "maxOffset")                                               \
  MACRO_(mergeFields, "mergeFields")                                           \
  MACRO_(message, "message")                                                   \
  IF_DECORATORS(MACRO_(method, "method"))                                      \
  MACRO_(meta, "meta")                                                         \
  MACRO_(microsecond, "microsecond")                                           \
  MACRO_(microseconds, "microseconds")                                         \
  MACRO_(millisecond, "millisecond")                                           \
  MACRO_(milliseconds, "milliseconds")                                         \
  MACRO_(minColumn, "minColumn")                                               \
  MACRO_(minDays, "minDays")                                                   \
  MACRO_(minimum, "minimum")                                                   \
  MACRO_(minimumFractionDigits, "minimumFractionDigits")                       \
  MACRO_(minimumIntegerDigits, "minimumIntegerDigits")                         \
  MACRO_(minimumSignificantDigits, "minimumSignificantDigits")                 \
  MACRO_(minLine, "minLine")                                                   \
  MACRO_(minOffset, "minOffset")                                               \
  MACRO_(minusSign, "minusSign")                                               \
  MACRO_(minute, "minute")                                                     \
  MACRO_(minutes, "minutes")                                                   \
  MACRO_(missingArguments, "missingArguments")                                 \
  MACRO_(mode, "mode")                                                         \
  MACRO_(module, "module")                                                     \
  MACRO_(Module, "Module")                                                     \
  MACRO_(ModuleEvaluate, "ModuleEvaluate")                                     \
  MACRO_(ModuleInstantiate, "ModuleInstantiate")                               \
  MACRO_(month, "month")                                                       \
  MACRO_(monthCode, "monthCode")                                               \
  MACRO_(monthDayFromFields, "monthDayFromFields")                             \
  MACRO_(months, "months")                                                     \
  MACRO_(monthsInYear, "monthsInYear")                                         \
  MACRO_(multiline, "multiline")                                               \
  MACRO_(mutable_, "mutable")                                                  \
  MACRO_(name, "name")                                                         \
  MACRO_(nan, "nan")                                                           \
  MACRO_(NaN, "NaN")                                                           \
  MACRO_(nanosecond, "nanosecond")                                             \
  MACRO_(nanoseconds, "nanoseconds")                                           \
  MACRO_(NegativeInfinity_, "-Infinity")                                       \
  MACRO_(new_, "new")                                                          \
  MACRO_(next, "next")                                                         \
  MACRO_(nextMethod, "nextMethod")                                             \
  MACRO_(NFC, "NFC")                                                           \
  MACRO_(NFD, "NFD")                                                           \
  MACRO_(NFKC, "NFKC")                                                         \
  MACRO_(NFKD, "NFKD")                                                         \
  MACRO_(noFilename, "noFilename")                                             \
  MACRO_(nonincrementalReason, "nonincrementalReason")                         \
  MACRO_(NoPrivateGetter, "NoPrivateGetter")                                   \
  MACRO_(noStack, "noStack")                                                   \
  MACRO_(not_equal_, "not-equal")                                              \
  MACRO_(notation, "notation")                                                 \
  MACRO_(notes, "notes")                                                       \
  MACRO_(Now, "Now")                                                           \
  MACRO_(null, "null")                                                         \
  MACRO_(numberingSystem, "numberingSystem")                                   \
  MACRO_(numeric, "numeric")                                                   \
  MACRO_(object_Arguments_, "[object Arguments]")                              \
  MACRO_(object_Array_, "[object Array]")                                      \
  MACRO_(object_BigInt_, "[object BigInt]")                                    \
  MACRO_(object_Boolean_, "[object Boolean]")                                  \
  MACRO_(object_Date_, "[object Date]")                                        \
  MACRO_(object_Error_, "[object Error]")                                      \
  MACRO_(object_Function_, "[object Function]")                                \
  MACRO_(object_Null_, "[object Null]")                                        \
  MACRO_(object_Number_, "[object Number]")                                    \
  MACRO_(object_Object_, "[object Object]")                                    \
  IF_RECORD_TUPLE(MACRO_(object_Record_, "[object Record]"))                   \
  MACRO_(object_RegExp_, "[object RegExp]")                                    \
  MACRO_(object_String_, "[object String]")                                    \
  MACRO_(object_Symbol_, "[object Symbol]")                                    \
  IF_RECORD_TUPLE(MACRO_(object_Tuple_, "[object Tuple]"))                     \
  MACRO_(object_Undefined_, "[object Undefined]")                              \
  MACRO_(Object_valueOf, "Object_valueOf")                                     \
  MACRO_(objects, "objects")                                                   \
  MACRO2(of, "of")                                                             \
  MACRO_(offset, "offset")                                                     \
  MACRO2(ok, "ok")                                                             \
  MACRO_(one, "one")                                                           \
  MACRO_(optimizedOut, "optimizedOut")                                         \
  MACRO_(other, "other")                                                       \
  MACRO_(out_of_memory_, "out of memory")                                      \
  MACRO_(overflow, "overflow")                                                 \
  MACRO_(ownKeys, "ownKeys")                                                   \
  MACRO_(package, "package")                                                   \
  MACRO_(parameters, "parameters")                                             \
  MACRO_(parseFloat, "parseFloat")                                             \
  MACRO_(parseInt, "parseInt")                                                 \
  MACRO_(pattern, "pattern")                                                   \
  MACRO_(pending, "pending")                                                   \
  MACRO_(percentSign, "percentSign")                                           \
  MACRO_(pipeTo, "pipeTo")                                                     \
  MACRO_(plainDate, "plainDate")                                               \
  MACRO_(plainTime, "plainTime")                                               \
  MACRO_(plusSign, "plusSign")                                                 \
  MACRO_(preventAbort, "preventAbort")                                         \
  MACRO_(preventCancel, "preventCancel")                                       \
  MACRO_(preventClose, "preventClose")                                         \
  MACRO_(preventExtensions, "preventExtensions")                               \
  MACRO_(private_, "private")                                                  \
  MACRO_(promise, "promise")                                                   \
  MACRO_(propertyIsEnumerable, "propertyIsEnumerable")                         \
  MACRO_(protected_, "protected")                                              \
  MACRO_(proto_, "__proto__")                                                  \
  MACRO_(prototype, "prototype")                                               \
  MACRO_(proxy, "proxy")                                                       \
  MACRO_(public_, "public")                                                    \
  MACRO_(pull, "pull")                                                         \
  MACRO_(quarter, "quarter")                                                   \
  MACRO_(raw, "raw")                                                           \
  MACRO_(rawJSON, "rawJSON")                                                   \
  MACRO_(read, "read")                                                         \
  MACRO_(reason, "reason")                                                     \
  MACRO_(RegExp_String_Iterator_, "RegExp String Iterator")                    \
  MACRO_(RegExp_prototype_Exec, "RegExp_prototype_Exec")                       \
  MACRO_(region, "region")                                                     \
  MACRO_(register_, "register")                                                \
  MACRO_(Reify, "Reify")                                                       \
  MACRO_(reject, "reject")                                                     \
  MACRO_(rejected, "rejected")                                                 \
  MACRO_(relatedYear, "relatedYear")                                           \
  MACRO_(relativeTo, "relativeTo")                                             \
  MACRO_(RequireObjectCoercible, "RequireObjectCoercible")                     \
  MACRO_(resize, "resize")                                                     \
  MACRO_(resizable, "resizable")                                               \
  MACRO_(resolve, "resolve")                                                   \
  MACRO_(result, "result")                                                     \
  MACRO_(results, "results")                                                   \
  MACRO_(resumeGenerator, "resumeGenerator")                                   \
  MACRO_(return_, "return")                                                    \
  MACRO_(revoke, "revoke")                                                     \
  MACRO_(roundingIncrement, "roundingIncrement")                               \
  MACRO_(roundingMode, "roundingMode")                                         \
  MACRO_(roundingPriority, "roundingPriority")                                 \
  MACRO_(script, "script")                                                     \
  MACRO_(scripts, "scripts")                                                   \
  MACRO_(second, "second")                                                     \
  MACRO_(seconds, "seconds")                                                   \
  MACRO_(self_hosted_, "self-hosted")                                          \
  MACRO_(sensitivity, "sensitivity")                                           \
  MACRO_(set, "set")                                                           \
  IF_DECORATORS(MACRO_(setter, "setter"))                                      \
  MACRO_(setBigInt64, "setBigInt64")                                           \
  MACRO_(setBigUint64, "setBigUint64")                                         \
  MACRO_(SetCanonicalName, "SetCanonicalName")                                 \
  MACRO_(SetConstructorInit, "SetConstructorInit")                             \
  MACRO_(setFloat16, "setFloat16")                                             \
  MACRO_(SetIsInlinableLargeFunction, "SetIsInlinableLargeFunction")           \
  MACRO_(Set_Iterator_, "Set Iterator")                                        \
  MACRO_(setFromBase64, "setFromBase64")                                       \
  MACRO_(setFromHex, "setFromHex")                                             \
  MACRO_(setPrototypeOf, "setPrototypeOf")                                     \
  MACRO_(shape, "shape")                                                       \
  MACRO_(shared, "shared")                                                     \
  MACRO_(signal, "signal")                                                     \
  MACRO_(signDisplay, "signDisplay")                                           \
  MACRO_(size, "size")                                                         \
  MACRO_(skeleton, "skeleton")                                                 \
  MACRO_(smallestUnit, "smallestUnit")                                         \
  MACRO_(source, "source")                                                     \
  MACRO_(SpeciesConstructor, "SpeciesConstructor")                             \
  MACRO_(stack, "stack")                                                       \
  MACRO_(star_namespace_star_, "*namespace*")                                  \
  MACRO_(start, "start")                                                       \
  MACRO_(startRange, "startRange")                                             \
  MACRO_(startTimestamp, "startTimestamp")                                     \
  MACRO_(state, "state")                                                       \
  MACRO_(static_, "static")                                                    \
  MACRO_(status, "status")                                                     \
  MACRO_(sticky, "sticky")                                                     \
  MACRO_(String_Iterator_, "String Iterator")                                  \
  MACRO_(strings, "strings")                                                   \
  MACRO_(String_split, "String_split")                                         \
  MACRO_(StructType, "StructType")                                             \
  MACRO_(style, "style")                                                       \
  MACRO_(super, "super")                                                       \
  MACRO_(switch_, "switch")                                                    \
  MACRO_(symmetricDifference, "symmetricDifference")                           \
  MACRO_(target, "target")                                                     \
  MACRO_(test, "test")                                                         \
  MACRO_(then, "then")                                                         \
  MACRO_(this_, "this")                                                        \
  MACRO_(throw_, "throw")                                                      \
  MACRO_(timed_out_, "timed-out")                                              \
  MACRO_(timestamp, "timestamp")                                               \
  MACRO_(timeStyle, "timeStyle")                                               \
  MACRO_(timeZone, "timeZone")                                                 \
  MACRO_(timeZoneName, "timeZoneName")                                         \
  MACRO_(toBase64, "toBase64")                                                 \
  MACRO_(toGMTString, "toGMTString")                                           \
  MACRO_(toHex, "toHex")                                                       \
  MACRO_(toISOString, "toISOString")                                           \
  MACRO_(toJSON, "toJSON")                                                     \
  MACRO_(toLocaleString, "toLocaleString")                                     \
  MACRO_(ToNumeric, "ToNumeric")                                               \
  MACRO_(toSource, "toSource")                                                 \
  MACRO_(toString, "toString")                                                 \
  MACRO_(ToString, "ToString")                                                 \
  MACRO_(toUTCString, "toUTCString")                                           \
  MACRO_(trailingZeroDisplay, "trailingZeroDisplay")                           \
  MACRO_(transfer, "transfer")                                                 \
  MACRO_(transferToFixedLength, "transferToFixedLength")                       \
  MACRO_(trimEnd, "trimEnd")                                                   \
  MACRO_(trimLeft, "trimLeft")                                                 \
  MACRO_(trimRight, "trimRight")                                               \
  MACRO_(trimStart, "trimStart")                                               \
  MACRO_(true_, "true")                                                        \
  MACRO_(try_, "try")                                                          \
  MACRO_(two, "two")                                                           \
  MACRO_(type, "type")                                                         \
  MACRO_(typeof_, "typeof")                                                    \
  MACRO_(uint8, "uint8")                                                       \
  MACRO_(uint8Clamped, "uint8Clamped")                                         \
  MACRO_(uint16, "uint16")                                                     \
  MACRO_(uint32, "uint32")                                                     \
  MACRO_(Uint8x16, "Uint8x16")                                                 \
  MACRO_(Uint16x8, "Uint16x8")                                                 \
  MACRO_(Uint32x4, "Uint32x4")                                                 \
  MACRO_(unescape, "unescape")                                                 \
  MACRO_(uneval, "uneval")                                                     \
  MACRO_(unicode, "unicode")                                                   \
  MACRO_(unicodeSets, "unicodeSets")                                           \
  MACRO_(uninitialized, "uninitialized")                                       \
  MACRO_(union_, "union")                                                      \
  MACRO_(unit, "unit")                                                         \
  MACRO_(unitDisplay, "unitDisplay")                                           \
  MACRO_(unknown, "unknown")                                                   \
  MACRO_(unregister, "unregister")                                             \
  MACRO_(UnsafeGetInt32FromReservedSlot, "UnsafeGetInt32FromReservedSlot")     \
  MACRO_(UnsafeGetObjectFromReservedSlot, "UnsafeGetObjectFromReservedSlot")   \
  MACRO_(UnsafeGetReservedSlot, "UnsafeGetReservedSlot")                       \
  MACRO_(UnsafeGetStringFromReservedSlot, "UnsafeGetStringFromReservedSlot")   \
  MACRO_(UnsafeSetReservedSlot, "UnsafeSetReservedSlot")                       \
  MACRO_(unsized, "unsized")                                                   \
  MACRO_(unwatch, "unwatch")                                                   \
  MACRO_(url, "url")                                                           \
  MACRO_(usage, "usage")                                                       \
  MACRO_(use_asm_, "use asm")                                                  \
  MACRO_(use_strict_, "use strict")                                            \
  MACRO_(useGrouping, "useGrouping")                                           \
  IF_EXPLICIT_RESOURCE_MANAGEMENT(MACRO_(using_, "using"))                     \
  MACRO_(UTC, "UTC")                                                           \
  MACRO_(value, "value")                                                       \
  MACRO_(valueOf, "valueOf")                                                   \
  MACRO_(values, "values")                                                     \
  MACRO_(var, "var")                                                           \
  MACRO_(variable, "variable")                                                 \
  MACRO_(void_, "void")                                                        \
  MACRO_(void_0_, "(void 0)")                                                  \
  MACRO_(wasm, "wasm")                                                         \
  MACRO_(WasmAnyRef, "WasmAnyRef")                                             \
  MACRO_(wasmcall, "wasmcall")                                                 \
  MACRO_(watch, "watch")                                                       \
  MACRO_(WeakMapConstructorInit, "WeakMapConstructorInit")                     \
  MACRO_(WeakSetConstructorInit, "WeakSetConstructorInit")                     \
  MACRO_(WeakSet_add, "WeakSet_add")                                           \
  MACRO_(week, "week")                                                         \
  MACRO_(weekday, "weekday")                                                   \
  MACRO_(weekend, "weekend")                                                   \
  MACRO_(weekOfYear, "weekOfYear")                                             \
  MACRO_(weeks, "weeks")                                                       \
  MACRO_(while_, "while")                                                      \
  MACRO_(with, "with")                                                         \
  MACRO_(written, "written")                                                   \
  MACRO_(toReversed, "toReversed")                                             \
  MACRO_(toSorted, "toSorted")                                                 \
  MACRO_(toSpliced, "toSpliced")                                               \
  MACRO_(writable, "writable")                                                 \
  MACRO_(write, "write")                                                       \
  MACRO_(year, "year")                                                         \
  MACRO_(yearMonthFromFields, "yearMonthFromFields")                           \
  MACRO_(yearName, "yearName")                                                 \
  MACRO_(yearOfWeek, "yearOfWeek")                                             \
  MACRO_(years, "years")                                                       \
  MACRO_(yield, "yield")                                                       \
  MACRO_(zero, "zero")                                                         \
  /* Type names must be contiguous and ordered; see js::TypeName. */           \
  MACRO_(undefined, "undefined")                                               \
  MACRO_(object, "object")                                                     \
  MACRO_(function, "function")                                                 \
  MACRO_(string, "string")                                                     \
  MACRO_(number, "number")                                                     \
  MACRO_(boolean, "boolean")                                                   \
  MACRO_(symbol, "symbol")                                                     \
  MACRO_(bigint, "bigint")                                                     \
  IF_RECORD_TUPLE(MACRO_(record, "record"))                                    \
  IF_RECORD_TUPLE(MACRO_(tuple, "tuple"))

#define PROPERTY_NAME_IGNORE(ID, TEXT)

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
