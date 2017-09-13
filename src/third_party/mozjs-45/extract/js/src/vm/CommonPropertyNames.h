/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A higher-order macro for enumerating all cached property names. */

#ifndef vm_CommonPropertyNames_h
#define vm_CommonPropertyNames_h

#include "jsprototypes.h"

#define FOR_EACH_COMMON_PROPERTYNAME(macro) \
    macro(add, add, "add") \
    macro(anonymous, anonymous, "anonymous") \
    macro(Any, Any, "Any") \
    macro(apply, apply, "apply") \
    macro(arguments, arguments, "arguments") \
    macro(as, as, "as") \
    macro(ArrayIteratorNext, ArrayIteratorNext, "ArrayIteratorNext") \
    macro(ArrayType, ArrayType, "ArrayType") \
    macro(ArrayValues, ArrayValues, "ArrayValues") \
    macro(ArrayValuesAt, ArrayValuesAt, "ArrayValuesAt") \
    macro(Async, Async, "Async") \
    macro(breakdown, breakdown, "breakdown") \
    macro(buffer, buffer, "buffer") \
    macro(builder, builder, "builder") \
    macro(by, by, "by") \
    macro(byteLength, byteLength, "byteLength") \
    macro(byteAlignment, byteAlignment, "byteAlignment") \
    macro(byteOffset, byteOffset, "byteOffset") \
    macro(bytes, bytes, "bytes") \
    macro(BYTES_PER_ELEMENT, BYTES_PER_ELEMENT, "BYTES_PER_ELEMENT") \
    macro(call, call, "call") \
    macro(callee, callee, "callee") \
    macro(caller, caller, "caller") \
    macro(callFunction, callFunction, "callFunction") \
    macro(caseFirst, caseFirst, "caseFirst") \
    macro(class_, class_, "class") \
    macro(close, close, "close") \
    macro(Collator, Collator, "Collator") \
    macro(CollatorCompareGet, CollatorCompareGet, "Intl_Collator_compare_get") \
    macro(collections, collections, "collections") \
    macro(columnNumber, columnNumber, "columnNumber") \
    macro(comma, comma, ",") \
    macro(compare, compare, "compare") \
    macro(configurable, configurable, "configurable") \
    macro(construct, construct, "construct") \
    macro(constructor, constructor, "constructor") \
    macro(ConvertAndCopyTo, ConvertAndCopyTo, "ConvertAndCopyTo") \
    macro(count, count, "count") \
    macro(currency, currency, "currency") \
    macro(currencyDisplay, currencyDisplay, "currencyDisplay") \
    macro(DateTimeFormat, DateTimeFormat, "DateTimeFormat") \
    macro(DateTimeFormatFormatGet, DateTimeFormatFormatGet, "Intl_DateTimeFormat_format_get") \
    macro(decodeURI, decodeURI, "decodeURI") \
    macro(decodeURIComponent, decodeURIComponent, "decodeURIComponent") \
    macro(default_, default_, "default") \
    macro(defineProperty, defineProperty, "defineProperty") \
    macro(defineGetter, defineGetter, "__defineGetter__") \
    macro(defineSetter, defineSetter, "__defineSetter__") \
    macro(delete, delete_, "delete") \
    macro(deleteProperty, deleteProperty, "deleteProperty") \
    macro(displayURL, displayURL, "displayURL") \
    macro(done, done, "done") \
    macro(dotGenerator, dotGenerator, ".generator") \
    macro(dotThis, dotThis, ".this") \
    macro(each, each, "each") \
    macro(elementType, elementType, "elementType") \
    macro(empty, empty, "") \
    macro(emptyRegExp, emptyRegExp, "(?:)") \
    macro(encodeURI, encodeURI, "encodeURI") \
    macro(encodeURIComponent, encodeURIComponent, "encodeURIComponent") \
    macro(endTimestamp, endTimestamp, "endTimestamp") \
    macro(enumerable, enumerable, "enumerable") \
    macro(enumerate, enumerate, "enumerate") \
    macro(escape, escape, "escape") \
    macro(eval, eval, "eval") \
    macro(false, false_, "false") \
    macro(fieldOffsets, fieldOffsets, "fieldOffsets") \
    macro(fieldTypes, fieldTypes, "fieldTypes") \
    macro(fileName, fileName, "fileName") \
    macro(fix, fix, "fix") \
    macro(flags, flags, "flags") \
    macro(float32, float32, "float32") \
    macro(float32x4, float32x4, "Float32x4") \
    macro(float64, float64, "float64") \
    macro(float64x2, float64x2, "Float64x2") \
    macro(forceInterpreter, forceInterpreter, "forceInterpreter") \
    macro(forEach, forEach, "forEach") \
    macro(format, format, "format") \
    macro(frame, frame, "frame") \
    macro(from, from, "from") \
    macro(gcCycleNumber, gcCycleNumber, "gcCycleNumber") \
    macro(GeneratorFunction, GeneratorFunction, "GeneratorFunction") \
    macro(get, get, "get") \
    macro(getInternals, getInternals, "getInternals") \
    macro(getOwnPropertyDescriptor, getOwnPropertyDescriptor, "getOwnPropertyDescriptor") \
    macro(getOwnPropertyNames, getOwnPropertyNames, "getOwnPropertyNames") \
    macro(getPropertyDescriptor, getPropertyDescriptor, "getPropertyDescriptor") \
    macro(global, global, "global") \
    macro(Handle, Handle, "Handle") \
    macro(has, has, "has") \
    macro(hasOwn, hasOwn, "hasOwn") \
    macro(hasOwnProperty, hasOwnProperty, "hasOwnProperty") \
    macro(ignoreCase, ignoreCase, "ignoreCase") \
    macro(ignorePunctuation, ignorePunctuation, "ignorePunctuation") \
    macro(index, index, "index") \
    macro(InitializeCollator, InitializeCollator, "InitializeCollator") \
    macro(InitializeDateTimeFormat, InitializeDateTimeFormat, "InitializeDateTimeFormat") \
    macro(InitializeNumberFormat, InitializeNumberFormat, "InitializeNumberFormat") \
    macro(inNursery, inNursery, "inNursery") \
    macro(innermost, innermost, "innermost") \
    macro(input, input, "input") \
    macro(int8x16, int8x16, "Int8x16") \
    macro(int16x8, int16x8, "Int16x8") \
    macro(int32x4, int32x4, "Int32x4") \
    macro(isFinite, isFinite, "isFinite") \
    macro(isNaN, isNaN, "isNaN") \
    macro(isPrototypeOf, isPrototypeOf, "isPrototypeOf") \
    macro(iterate, iterate, "iterate") \
    macro(Infinity, Infinity, "Infinity") \
    macro(InterpretGeneratorResume, InterpretGeneratorResume, "InterpretGeneratorResume") \
    macro(int8, int8, "int8") \
    macro(int16, int16, "int16") \
    macro(int32, int32, "int32") \
    macro(isEntryPoint, isEntryPoint, "isEntryPoint") \
    macro(isExtensible, isExtensible, "isExtensible") \
    macro(iteratorIntrinsic, iteratorIntrinsic, "__iterator__") \
    macro(join, join, "join") \
    macro(keys, keys, "keys") \
    macro(label, label, "label") \
    macro(lastIndex, lastIndex, "lastIndex") \
    macro(LegacyGeneratorCloseInternal, LegacyGeneratorCloseInternal, "LegacyGeneratorCloseInternal") \
    macro(length, length, "length") \
    macro(let, let, "let") \
    macro(line, line, "line") \
    macro(lineNumber, lineNumber, "lineNumber") \
    macro(loc, loc, "loc") \
    macro(locale, locale, "locale") \
    macro(lookupGetter, lookupGetter, "__lookupGetter__") \
    macro(lookupSetter, lookupSetter, "__lookupSetter__") \
    macro(maximumFractionDigits, maximumFractionDigits, "maximumFractionDigits") \
    macro(maximumSignificantDigits, maximumSignificantDigits, "maximumSignificantDigits") \
    macro(message, message, "message") \
    macro(minimumFractionDigits, minimumFractionDigits, "minimumFractionDigits") \
    macro(minimumIntegerDigits, minimumIntegerDigits, "minimumIntegerDigits") \
    macro(minimumSignificantDigits, minimumSignificantDigits, "minimumSignificantDigits") \
    macro(missingArguments, missingArguments, "missingArguments") \
    macro(module, module, "module") \
    macro(multiline, multiline, "multiline") \
    macro(name, name, "name") \
    macro(NaN, NaN, "NaN") \
    macro(new, new_, "new") \
    macro(next, next, "next") \
    macro(NFC, NFC, "NFC") \
    macro(NFD, NFD, "NFD") \
    macro(NFKC, NFKC, "NFKC") \
    macro(NFKD, NFKD, "NFKD") \
    macro(nonincrementalReason, nonincrementalReason, "nonincrementalReason") \
    macro(noFilename, noFilename, "noFilename") \
    macro(noStack, noStack, "noStack") \
    macro(NumberFormat, NumberFormat, "NumberFormat") \
    macro(NumberFormatFormatGet, NumberFormatFormatGet, "Intl_NumberFormat_format_get") \
    macro(numeric, numeric, "numeric") \
    macro(objectArray, objectArray, "[object Array]") \
    macro(objectFunction, objectFunction, "[object Function]") \
    macro(objectNull, objectNull, "[object Null]") \
    macro(objectNumber, objectNumber, "[object Number]") \
    macro(objectObject, objectObject, "[object Object]") \
    macro(objects, objects, "objects") \
    macro(objectString, objectString, "[object String]") \
    macro(objectUndefined, objectUndefined, "[object Undefined]") \
    macro(objectWindow, objectWindow, "[object Window]") \
    macro(of, of, "of") \
    macro(offset, offset, "offset") \
    macro(optimizedOut, optimizedOut, "optimizedOut") \
    macro(other, other, "other") \
    macro(outOfMemory, outOfMemory, "out of memory") \
    macro(ownKeys, ownKeys, "ownKeys") \
    macro(parseFloat, parseFloat, "parseFloat") \
    macro(parseInt, parseInt, "parseInt") \
    macro(pattern, pattern, "pattern") \
    macro(preventExtensions, preventExtensions, "preventExtensions") \
    macro(propertyIsEnumerable, propertyIsEnumerable, "propertyIsEnumerable") \
    macro(proto, proto, "__proto__") \
    macro(prototype, prototype, "prototype") \
    macro(proxy, proxy, "proxy") \
    macro(reason, reason, "reason") \
    macro(Reify, Reify, "Reify") \
    macro(RequireObjectCoercible, RequireObjectCoercible, "RequireObjectCoercible") \
    macro(resumeGenerator, resumeGenerator, "resumeGenerator") \
    macro(return, return_, "return") \
    macro(revoke, revoke, "revoke") \
    macro(script, script, "script") \
    macro(scripts, scripts, "scripts") \
    macro(sensitivity, sensitivity, "sensitivity") \
    macro(set, set, "set") \
    macro(shape, shape, "shape") \
    macro(signMask, signMask, "signMask") \
    macro(size, size, "size") \
    macro(source, source, "source") \
    macro(stack, stack, "stack") \
    macro(star, star, "*") \
    macro(starDefaultStar, starDefaultStar, "*default*") \
    macro(startTimestamp, startTimestamp, "startTimestamp") \
    macro(static, static_, "static") \
    macro(sticky, sticky, "sticky") \
    macro(strings, strings, "strings") \
    macro(StructType, StructType, "StructType") \
    macro(style, style, "style") \
    macro(super, super, "super") \
    macro(target, target, "target") \
    macro(test, test, "test") \
    macro(then, then, "then") \
    macro(throw, throw_, "throw") \
    macro(timestamp, timestamp, "timestamp") \
    macro(timeZone, timeZone, "timeZone") \
    macro(toGMTString, toGMTString, "toGMTString") \
    macro(toISOString, toISOString, "toISOString") \
    macro(toJSON, toJSON, "toJSON") \
    macro(toLocaleString, toLocaleString, "toLocaleString") \
    macro(toSource, toSource, "toSource") \
    macro(toString, toString, "toString") \
    macro(toUTCString, toUTCString, "toUTCString") \
    macro(true, true_, "true") \
    macro(unescape, unescape, "unescape") \
    macro(uneval, uneval, "uneval") \
    macro(unicode, unicode, "unicode") \
    macro(uninitialized, uninitialized, "uninitialized") \
    macro(uint8, uint8, "uint8") \
    macro(uint8Clamped, uint8Clamped, "uint8Clamped") \
    macro(uint16, uint16, "uint16") \
    macro(uint32, uint32, "uint32") \
    macro(unsized, unsized, "unsized") \
    macro(unwatch, unwatch, "unwatch") \
    macro(url, url, "url") \
    macro(usage, usage, "usage") \
    macro(useGrouping, useGrouping, "useGrouping") \
    macro(useAsm, useAsm, "use asm") \
    macro(useStrict, useStrict, "use strict") \
    macro(value, value, "value") \
    macro(values, values, "values") \
    macro(valueOf, valueOf, "valueOf") \
    macro(var, var, "var") \
    macro(variable, variable, "variable") \
    macro(void0, void0, "(void 0)") \
    macro(watch, watch, "watch") \
    macro(WeakSet_add, WeakSet_add, "WeakSet_add") \
    macro(writable, writable, "writable") \
    macro(w, w, "w") \
    macro(x, x, "x") \
    macro(y, y, "y") \
    macro(yield, yield, "yield") \
    macro(z, z, "z") \
    macro(raw, raw, "raw") \
    /* Type names must be contiguous and ordered; see js::TypeName. */ \
    macro(undefined, undefined, "undefined") \
    macro(object, object, "object") \
    macro(function, function, "function") \
    macro(string, string, "string") \
    macro(number, number, "number") \
    macro(boolean, boolean, "boolean") \
    macro(null, null, "null") \
    macro(symbol, symbol, "symbol") \
    /* Well-known atom names must be continuous and ordered, matching \
     * enum JS::SymbolCode in jsapi.h. */ \
    macro(iterator, iterator, "iterator") \
    macro(match, match, "match") \
    macro(species, species, "species") \
    macro(toPrimitive, toPrimitive, "toPrimitive") \
    /* Same goes for the descriptions of the well-known symbols. */ \
    macro(Symbol_hasInstance, Symbol_hasInstance, "Symbol.hasInstance") \
    macro(Symbol_isConcatSpreadable, Symbol_isConcatSpreadable, "Symbol.isConcatSpreadable") \
    macro(Symbol_iterator, Symbol_iterator, "Symbol.iterator") \
    macro(Symbol_match,    Symbol_match,    "Symbol.match") \
    macro(Symbol_species,  Symbol_species,  "Symbol.species") \
    macro(Symbol_toPrimitive, Symbol_toPrimitive, "Symbol.toPrimitive") \
    macro(Symbol_toStringTag, Symbol_toStringTag, "Symbol.toStringTag") \
    macro(Symbol_unscopables, Symbol_unscopables, "Symbol.unscopables") \
    /* Function names for properties named by symbols. */ \
    macro(Symbol_iterator_fun, Symbol_iterator_fun, "[Symbol.iterator]") \

#endif /* vm_CommonPropertyNames_h */
