/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*jshint bitwise: true, camelcase: false, curly: false, eqeqeq: true,
         es5: true, forin: true, immed: true, indent: 4, latedef: false,
         newcap: false, noarg: true, noempty: true, nonew: true,
         plusplus: false, quotmark: false, regexp: true, undef: true,
         unused: false, strict: false, trailing: true,
*/

/*global ToObject: false, ToInteger: false, IsCallable: false,
         ThrowError: false, AssertionFailed: false,
         MakeConstructible: false, DecompileArg: false,
         RuntimeDefaultLocale: false,
         ParallelDo: false, ParallelSlices: false, NewDenseArray: false,
         UnsafePutElements: false, ShouldForceSequential: false,
         ParallelTestsShouldPass: false,
         Dump: false,
         callFunction: false,
         TO_UINT32: false,
         JSMSG_NOT_FUNCTION: false, JSMSG_MISSING_FUN_ARG: false,
         JSMSG_EMPTY_ARRAY_REDUCE: false, JSMSG_CANT_CONVERT_TO: false,
*/

#include "SelfHostingDefines.h"

// All C++-implemented standard builtins library functions used in self-hosted
// code are installed via the std_functions JSFunctionSpec[] in
// SelfHosting.cpp.
//
// The few items below here are either self-hosted or installing them under a
// std_Foo name would require ugly contortions, so they just get aliased here.
var std_Array_indexOf = ArrayIndexOf;
var std_String_substring = String_substring;
// WeakMap is a bare constructor without properties or methods.
var std_WeakMap = WeakMap;
// StopIteration is a bare constructor without properties or methods.
var std_StopIteration = StopIteration;


/********** List specification type **********/


/* Spec: ECMAScript Language Specification, 5.1 edition, 8.8 */
function List() {
    this.length = 0;
}

{
  let ListProto = std_Object_create(null);
  ListProto.indexOf = std_Array_indexOf;
  ListProto.join = std_Array_join;
  ListProto.push = std_Array_push;
  ListProto.slice = std_Array_slice;
  ListProto.sort = std_Array_sort;
  MakeConstructible(List, ListProto);
}


/********** Record specification type **********/


/* Spec: ECMAScript Internationalization API Specification, draft, 5 */
function Record() {
    return std_Object_create(null);
}
MakeConstructible(Record, {});


/********** Abstract operations defined in ECMAScript Language Specification **********/


/* Spec: ECMAScript Language Specification, 5.1 edition, 8.12.6 and 11.8.7 */
function HasProperty(o, p) {
    return p in o;
}


/* Spec: ECMAScript Language Specification, 5.1 edition, 9.2 and 11.4.9 */
function ToBoolean(v) {
    return !!v;
}


/* Spec: ECMAScript Language Specification, 5.1 edition, 9.3 and 11.4.6 */
function ToNumber(v) {
    return +v;
}


/* Spec: ECMAScript Language Specification, 5.1 edition, 9.10 */
function CheckObjectCoercible(v) {
    if (v === undefined || v === null)
        ThrowError(JSMSG_CANT_CONVERT_TO, ToString(v), "object");
}

/* Spec: ECMAScript Draft, 6 edition May 22, 2014, 7.1.15 */
function ToLength(v) {
    v = ToInteger(v);

    if (v <= 0)
        return 0;

    // Math.pow(2, 53) - 1 = 0x1fffffffffffff
    return std_Math_min(v, 0x1fffffffffffff);
}

/* Spec: ECMAScript Draft, 6th edition Oct 14, 2014, 7.2.4 */
function SameValueZero(x, y) {
    return x === y || (x !== x && y !== y);
}

/* Spec: ECMAScript Draft, 6th edition Dec 24, 2014, 7.3.8 */
function GetMethod(O, P) {
    // Step 1.
    assert(IsPropertyKey(P), "Invalid property key");

    // Steps 2-3.
    var func = ToObject(O)[P];

    // Step 4.
    if (func === undefined || func === null)
        return undefined;

    // Step 5.
    if (!IsCallable(func))
        ThrowError(JSMSG_NOT_FUNCTION, typeof func);

    // Step 6.
    return func;
}

/* Spec: ECMAScript Draft, 6th edition Dec 24, 2014, 7.2.7 */
function IsPropertyKey(argument) {
    var type = typeof argument;
    return type === "string" || type === "symbol";
}

/* Spec: ECMAScript Draft, 6th edition Dec 24, 2014, 7.4.1 */
function GetIterator(obj, method) {
    // Steps 1-2.
    if (arguments.length === 1)
        method = GetMethod(obj, std_iterator);

    // Steps 3-4.
    var iterator = callFunction(method, obj);

    // Step 5.
    if (!IsObject(iterator))
        ThrowError(JSMSG_NOT_ITERABLE, ToString(iterator));

    // Step 6.
    return iterator;
}

function SpeciesConstructor(obj, defaultConstructor) {
    var C = obj.constructor;
    if (C === undefined) {
        return defaultConstructor;
    }
    if (!IsConstructor(C)) {
        ThrowError(JSMSG_NOT_CONSTRUCTOR, DecompileArg(1, C));
    }
    return C;
}
