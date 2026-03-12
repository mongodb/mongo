/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SelfHostingDefines.h"

// Assertions and debug printing, defined here instead of in the header above
// to make `assert` invisible to C++.
#ifdef DEBUG
#define assert(b, info) \
  do { \
    if (!(b)) { \
      AssertionFailed(__FILE__ + ":" + __LINE__ + ": " + info) \
    } \
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

/********** Specification types **********/

// A "Record" is an internal type used in the ECMAScript spec to define a struct
// made up of key / values. It is never exposed to user script, but we use a
// simple Object (with null prototype) as a convenient implementation.
function new_Record() {
  return std_Object_create(null);
}

/********** Abstract operations defined in ECMAScript Language Specification **********/

/* Spec: ECMAScript Language Specification, 5.1 edition, 9.2 and 11.4.9 */
function ToBoolean(v) {
  return !!v;
}

/* Spec: ECMAScript Language Specification, 5.1 edition, 9.3 and 11.4.6 */
function ToNumber(v) {
  return +v;
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
  if (IsNullOrUndefined(func)) {
    return undefined;
  }

  // Step 4.
  if (!IsCallable(func)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, typeof func);
  }

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

// ES 2016 draft Mar 25, 2016 7.3.20.
function SpeciesConstructor(obj, defaultConstructor) {
  // Step 1.
  assert(IsObject(obj), "not passed an object");

  // Step 2.
  var ctor = obj.constructor;

  // Step 3.
  if (ctor === undefined) {
    return defaultConstructor;
  }

  // Step 4.
  if (!IsObject(ctor)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, "object's 'constructor' property");
  }

  // Steps 5.
  var s = ctor[GetBuiltinSymbol("species")];

  // Step 6.
  if (IsNullOrUndefined(s)) {
    return defaultConstructor;
  }

  // Step 7.
  if (IsConstructor(s)) {
    return s;
  }

  // Step 8.
  ThrowTypeError(
    JSMSG_NOT_CONSTRUCTOR,
    "@@species property of object's constructor"
  );
}

function GetTypeError(...args) {
  try {
    FUN_APPLY(ThrowTypeError, undefined, args);
  } catch (e) {
    return e;
  }
  assert(false, "the catch block should've returned from this function.");
}

function GetAggregateError(...args) {
  try {
    FUN_APPLY(ThrowAggregateError, undefined, args);
  } catch (e) {
    return e;
  }
  assert(false, "the catch block should've returned from this function.");
}

function GetInternalError(...args) {
  try {
    FUN_APPLY(ThrowInternalError, undefined, args);
  } catch (e) {
    return e;
  }
  assert(false, "the catch block should've returned from this function.");
}

// To be used when a function is required but calling it shouldn't do anything.
function NullFunction() {}

// ES2019 draft rev 4c2df13f4194057f09b920ee88712e5a70b1a556
// 7.3.23 CopyDataProperties (target, source, excludedItems)
function CopyDataProperties(target, source, excludedItems) {
  // Step 1.
  assert(IsObject(target), "target is an object");

  // Step 2.
  assert(IsObject(excludedItems), "excludedItems is an object");

  // Steps 3 and 7.
  if (IsNullOrUndefined(source)) {
    return;
  }

  // Step 4.
  var from = ToObject(source);

  // Step 5.
  var keys = CopyDataPropertiesOrGetOwnKeys(target, from, excludedItems);

  // Return if we copied all properties in native code.
  if (keys === null) {
    return;
  }

  // Step 6.
  for (var index = 0; index < keys.length; index++) {
    var key = keys[index];

    // We abbreviate this by calling propertyIsEnumerable which is faster
    // and returns false for not defined properties.
    if (
      !hasOwn(key, excludedItems) &&
      callFunction(std_Object_propertyIsEnumerable, from, key)
    ) {
      DefineDataProperty(target, key, from[key]);
    }
  }

  // Step 7 (Return).
}

// ES2019 draft rev 4c2df13f4194057f09b920ee88712e5a70b1a556
// 7.3.23 CopyDataProperties (target, source, excludedItems)
function CopyDataPropertiesUnfiltered(target, source) {
  // Step 1.
  assert(IsObject(target), "target is an object");

  // Step 2 (Not applicable).

  // Steps 3 and 7.
  if (IsNullOrUndefined(source)) {
    return;
  }

  // Step 4.
  var from = ToObject(source);

  // Step 5.
  var keys = CopyDataPropertiesOrGetOwnKeys(target, from, null);

  // Return if we copied all properties in native code.
  if (keys === null) {
    return;
  }

  // Step 6.
  for (var index = 0; index < keys.length; index++) {
    var key = keys[index];

    // We abbreviate this by calling propertyIsEnumerable which is faster
    // and returns false for not defined properties.
    if (callFunction(std_Object_propertyIsEnumerable, from, key)) {
      DefineDataProperty(target, key, from[key]);
    }
  }

  // Step 7 (Return).
}

/*************************************** Testing functions ***************************************/
function outer() {
  return function inner() {
    return "foo";
  };
}
