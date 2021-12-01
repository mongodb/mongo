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
         ThrowRangeError: false, ThrowTypeError: false,
         AssertionFailed: false,
         MakeConstructible: false, DecompileArg: false,
         RuntimeDefaultLocale: false,
         NewDenseArray: false,
         Dump: false,
         callFunction: false,
         TO_UINT32: false,
         JSMSG_NOT_FUNCTION: false, JSMSG_MISSING_FUN_ARG: false,
         JSMSG_EMPTY_ARRAY_REDUCE: false, JSMSG_CANT_CONVERT_TO: false,
*/

#include "SelfHostingDefines.h"
#include "TypedObjectConstants.h"

// Assertions and debug printing, defined here instead of in the header above
// to make `assert` invisible to C++.
#ifdef DEBUG
#define assert(b, info) \
    do { \
        if (!(b)) \
            AssertionFailed(__FILE__ + ":" + __LINE__ + ": " + info) \
    } while (false)
#define dbg(msg) \
    do { \
        DumpMessage(callFunction(std_Array_pop, \
                                 StringSplitString(__FILE__, '/')) + \
                    '#' + __LINE__ + ': ' + msg) \
    } while (false)
#else
#define assert(b, info) ; // Elided assertion.
#define dbg(msg) ; // Elided debugging output.
#endif

// All C++-implemented standard builtins library functions used in self-hosted
// code are installed via the std_functions JSFunctionSpec[] in
// SelfHosting.cpp.
//
// Do not create an alias to a self-hosted builtin, otherwise it will be cloned
// twice.
//
// Symbol is a bare constructor without properties or methods.
var std_Symbol = Symbol;


/********** List specification type **********/

/* Spec: ECMAScript Language Specification, 5.1 edition, 8.8 */
function List() {
    this.length = 0;
}
MakeConstructible(List, {__proto__: null});


/********** Record specification type **********/


/* Spec: ECMAScript Internationalization API Specification, draft, 5 */
function Record() {
    return std_Object_create(null);
}
MakeConstructible(Record, {});


/********** Abstract operations defined in ECMAScript Language Specification **********/


/* Spec: ECMAScript Language Specification, 5.1 edition, 9.2 and 11.4.9 */
function ToBoolean(v) {
    return !!v;
}


/* Spec: ECMAScript Language Specification, 5.1 edition, 9.3 and 11.4.6 */
function ToNumber(v) {
    return +v;
}


// ES6 7.2.1 (previously, ES5 9.10 under the name "CheckObjectCoercible").
function RequireObjectCoercible(v) {
    if (v === undefined || v === null)
        ThrowTypeError(JSMSG_CANT_CONVERT_TO, ToString(v), "object");
}

/* Spec: ECMAScript Draft, 6 edition May 22, 2014, 7.1.15 */
function ToLength(v) {
    // Step 1.
    v = ToInteger(v);

    // Step 2.
    // Use max(v, 0) here, because it's easier to optimize in Ion.
    // This is correct even for -0.
    v = std_Math_max(v, 0);

    // Step 3.
    // Math.pow(2, 53) - 1 = 0x1fffffffffffff
    return std_Math_min(v, 0x1fffffffffffff);
}

// ES2017 draft rev aebf014403a3e641fb1622aec47c40f051943527
// 7.2.9 SameValue ( x, y )
function SameValue(x, y) {
    if (x === y) {
        return (x !== 0) || (1 / x === 1 / y);
    }
    return (x !== x && y !== y);
}

// ES2017 draft rev aebf014403a3e641fb1622aec47c40f051943527
// 7.2.10 SameValueZero ( x, y )
function SameValueZero(x, y) {
    return x === y || (x !== x && y !== y);
}

// ES 2017 draft (April 6, 2016) 7.3.9
function GetMethod(V, P) {
    // Step 1.
    assert(IsPropertyKey(P), "Invalid property key");

    // Step 2.
    var func = V[P];

    // Step 3.
    if (func === undefined || func === null)
        return undefined;

    // Step 4.
    if (!IsCallable(func))
        ThrowTypeError(JSMSG_NOT_FUNCTION, typeof func);

    // Step 5.
    return func;
}

/* Spec: ECMAScript Draft, 6th edition Dec 24, 2014, 7.2.7 */
function IsPropertyKey(argument) {
    var type = typeof argument;
    return type === "string" || type === "symbol";
}

#define TO_PROPERTY_KEY(name) \
(typeof name !== "string" && typeof name !== "number" && typeof name !== "symbol" ? ToPropertyKey(name) : name)

var _builtinCtorsCache = {__proto__: null};

function GetBuiltinConstructor(builtinName) {
    var ctor = _builtinCtorsCache[builtinName] ||
               (_builtinCtorsCache[builtinName] = GetBuiltinConstructorImpl(builtinName));
    assert(ctor, `No builtin with name "${builtinName}" found`);
    return ctor;
}

function GetBuiltinPrototype(builtinName) {
    return (_builtinCtorsCache[builtinName] || GetBuiltinConstructor(builtinName)).prototype;
}

// ES 2016 draft Mar 25, 2016 7.3.20.
function SpeciesConstructor(obj, defaultConstructor) {
    // Step 1.
    assert(IsObject(obj), "not passed an object");

    // Step 2.
    var ctor = obj.constructor;

    // Step 3.
    if (ctor === undefined)
        return defaultConstructor;

    // Step 4.
    if (!IsObject(ctor))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, "object's 'constructor' property");

    // Steps 5.
    var s = ctor[std_species];

    // Step 6.
    if (s === undefined || s === null)
        return defaultConstructor;

    // Step 7.
    if (IsConstructor(s))
        return s;

    // Step 8.
    ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, "@@species property of object's constructor");
}

function GetTypeError(msg) {
    try {
        FUN_APPLY(ThrowTypeError, undefined, arguments);
    } catch (e) {
        return e;
    }
    assert(false, "the catch block should've returned from this function.");
}

function GetInternalError(msg) {
    try {
        FUN_APPLY(ThrowInternalError, undefined, arguments);
    } catch (e) {
        return e;
    }
    assert(false, "the catch block should've returned from this function.");
}

// To be used when a function is required but calling it shouldn't do anything.
function NullFunction() {}

// Object Rest/Spread Properties proposal
// Abstract operation: CopyDataProperties (target, source, excluded)
function CopyDataProperties(target, source, excluded) {
    // Step 1.
    assert(IsObject(target), "target is an object");

    // Step 2.
    assert(IsObject(excluded), "excluded is an object");

    // Steps 3, 6.
    if (source === undefined || source === null)
        return;

    // Step 4.a.
    source = ToObject(source);

    // Step 4.b.
    var keys = OwnPropertyKeys(source);

    // Step 5.
    for (var index = 0; index < keys.length; index++) {
        var key = keys[index];

        // We abbreviate this by calling propertyIsEnumerable which is faster
        // and returns false for not defined properties.
        if (!hasOwn(key, excluded) && callFunction(std_Object_propertyIsEnumerable, source, key))
            _DefineDataProperty(target, key, source[key]);
    }

    // Step 6 (Return).
}

// Object Rest/Spread Properties proposal
// Abstract operation: CopyDataProperties (target, source, excluded)
function CopyDataPropertiesUnfiltered(target, source) {
    // Step 1.
    assert(IsObject(target), "target is an object");

    // Step 2 (Not applicable).

    // Steps 3, 6.
    if (source === undefined || source === null)
        return;

    // Step 4.a.
    source = ToObject(source);

    // Step 4.b.
    var keys = OwnPropertyKeys(source);

    // Step 5.
    for (var index = 0; index < keys.length; index++) {
        var key = keys[index];

        // We abbreviate this by calling propertyIsEnumerable which is faster
        // and returns false for not defined properties.
        if (callFunction(std_Object_propertyIsEnumerable, source, key))
            _DefineDataProperty(target, key, source[key]);
    }

    // Step 6 (Return).
}

/*************************************** Testing functions ***************************************/
function outer() {
    return function inner() {
        return "foo";
    };
}
