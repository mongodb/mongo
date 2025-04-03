/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES stage 4 proposal
function ObjectGetOwnPropertyDescriptors(O) {
  // Step 1.
  var obj = ToObject(O);

  // Step 2.
  var keys = std_Reflect_ownKeys(obj);

  // Step 3.
  var descriptors = {};

  // Step 4.
  for (var index = 0, len = keys.length; index < len; index++) {
    var key = keys[index];

    // Steps 4.a-b.
    var desc = ObjectGetOwnPropertyDescriptor(obj, key);

    // Step 4.c.
    if (typeof desc !== "undefined") {
      DefineDataProperty(descriptors, key, desc);
    }
  }

  // Step 5.
  return descriptors;
}

/* ES6 draft rev 32 (2015 Feb 2) 19.1.2.9. */
function ObjectGetPrototypeOf(obj) {
  return std_Reflect_getPrototypeOf(ToObject(obj));
}

/* ES6 draft rev 32 (2015 Feb 2) 19.1.2.11. */
function ObjectIsExtensible(obj) {
  return IsObject(obj) && std_Reflect_isExtensible(obj);
}

/* ES2015 19.1.3.5 Object.prototype.toLocaleString */
function Object_toLocaleString() {
  // Step 1.
  var O = this;

  // Step 2.
  return callContentFunction(O.toString, O);
}

// ES 2017 draft bb96899bb0d9ef9be08164a26efae2ee5f25e875 19.1.3.7
function Object_valueOf() {
  // Step 1.
  return ToObject(this);
}

// ES 2018 draft 19.1.3.2
function Object_hasOwnProperty(V) {
  // Implement hasOwnProperty as a pseudo function that becomes a JSOp
  // to easier add an inline cache for this.
  return hasOwn(V, this);
}

// ES 2021 draft rev 0b988b7700de675331ac360d164c978d6ea452ec
// B.2.2.1.1 get Object.prototype.__proto__
function $ObjectProtoGetter() {
  return std_Reflect_getPrototypeOf(ToObject(this));
}
SetCanonicalName($ObjectProtoGetter, "get __proto__");

// ES 2021 draft rev 0b988b7700de675331ac360d164c978d6ea452ec
// B.2.2.1.2 set Object.prototype.__proto__
function $ObjectProtoSetter(proto) {
  return callFunction(std_Object_setProto, this, proto);
}
SetCanonicalName($ObjectProtoSetter, "set __proto__");

// ES7 draft (2016 March 8) B.2.2.3
function ObjectDefineSetter(name, setter) {
  // Step 1.
  var object = ToObject(this);

  // Step 2.
  if (!IsCallable(setter)) {
    ThrowTypeError(JSMSG_BAD_GETTER_OR_SETTER, "setter");
  }

  // Step 4.
  var key = TO_PROPERTY_KEY(name);

  // Steps 3, 5.
  DefineProperty(
    object,
    key,
    ACCESSOR_DESCRIPTOR_KIND | ATTR_ENUMERABLE | ATTR_CONFIGURABLE,
    null,
    setter,
    true
  );

  // Step 6. (implicit)
}

// ES7 draft (2016 March 8) B.2.2.2
function ObjectDefineGetter(name, getter) {
  // Step 1.
  var object = ToObject(this);

  // Step 2.
  if (!IsCallable(getter)) {
    ThrowTypeError(JSMSG_BAD_GETTER_OR_SETTER, "getter");
  }

  // Step 4.
  var key = TO_PROPERTY_KEY(name);

  // Steps 3, 5.
  DefineProperty(
    object,
    key,
    ACCESSOR_DESCRIPTOR_KIND | ATTR_ENUMERABLE | ATTR_CONFIGURABLE,
    getter,
    null,
    true
  );

  // Step 6. (implicit)
}

// ES7 draft (2016 March 8) B.2.2.5
function ObjectLookupSetter(name) {
  // Step 1.
  var object = ToObject(this);

  // Step 2.
  var key = TO_PROPERTY_KEY(name);

  do {
    // Step 3.a.
    var desc = GetOwnPropertyDescriptorToArray(object, key);

    // Step 3.b.
    if (desc) {
      // Step.b.i.
      if (desc[PROP_DESC_ATTRS_AND_KIND_INDEX] & ACCESSOR_DESCRIPTOR_KIND) {
        return desc[PROP_DESC_SETTER_INDEX];
      }

      // Step.b.i.
      return undefined;
    }

    // Step 3.c.
    object = std_Reflect_getPrototypeOf(object);
  } while (object !== null);

  // Step 3.d. (implicit)
}

// ES7 draft (2016 March 8) B.2.2.4
function ObjectLookupGetter(name) {
  // Step 1.
  var object = ToObject(this);

  // Step 2.
  var key = TO_PROPERTY_KEY(name);

  do {
    // Step 3.a.
    var desc = GetOwnPropertyDescriptorToArray(object, key);

    // Step 3.b.
    if (desc) {
      // Step.b.i.
      if (desc[PROP_DESC_ATTRS_AND_KIND_INDEX] & ACCESSOR_DESCRIPTOR_KIND) {
        return desc[PROP_DESC_GETTER_INDEX];
      }

      // Step.b.ii.
      return undefined;
    }

    // Step 3.c.
    object = std_Reflect_getPrototypeOf(object);
  } while (object !== null);

  // Step 3.d. (implicit)
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 19.1.2.6 Object.getOwnPropertyDescriptor ( O, P )
function ObjectGetOwnPropertyDescriptor(obj, propertyKey) {
  // Steps 1-3.
  var desc = GetOwnPropertyDescriptorToArray(obj, propertyKey);

  // Step 4 (Call to 6.2.4.4 FromPropertyDescriptor).

  // 6.2.4.4 FromPropertyDescriptor, step 1.
  if (!desc) {
    return undefined;
  }

  // 6.2.4.4 FromPropertyDescriptor, steps 2-5, 8-11.
  var attrsAndKind = desc[PROP_DESC_ATTRS_AND_KIND_INDEX];
  if (attrsAndKind & DATA_DESCRIPTOR_KIND) {
    return {
      value: desc[PROP_DESC_VALUE_INDEX],
      writable: !!(attrsAndKind & ATTR_WRITABLE),
      enumerable: !!(attrsAndKind & ATTR_ENUMERABLE),
      configurable: !!(attrsAndKind & ATTR_CONFIGURABLE),
    };
  }

  // 6.2.4.4 FromPropertyDescriptor, steps 2-3, 6-11.
  assert(
    attrsAndKind & ACCESSOR_DESCRIPTOR_KIND,
    "expected accessor property descriptor"
  );
  return {
    get: desc[PROP_DESC_GETTER_INDEX],
    set: desc[PROP_DESC_SETTER_INDEX],
    enumerable: !!(attrsAndKind & ATTR_ENUMERABLE),
    configurable: !!(attrsAndKind & ATTR_CONFIGURABLE),
  };
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 19.1.2.4 Object.defineProperty ( O, P, Attributes )
// 26.1.3 Reflect.defineProperty ( target, propertyKey, attributes )
function ObjectOrReflectDefineProperty(obj, propertyKey, attributes, strict) {
  // Step 1.
  if (!IsObject(obj)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, obj));
  }

  // Step 2.
  propertyKey = TO_PROPERTY_KEY(propertyKey);

  // Step 3 (Call to 6.2.4.5 ToPropertyDescriptor).

  // 6.2.4.5 ToPropertyDescriptor, step 1.
  if (!IsObject(attributes)) {
    ThrowTypeError(
      JSMSG_OBJECT_REQUIRED_PROP_DESC,
      DecompileArg(2, attributes)
    );
  }

  // 6.2.4.5 ToPropertyDescriptor, step 2.
  var attrs = 0;
  var hasValue = false;
  var value;
  var getter = null;
  var setter = null;

  // 6.2.4.5 ToPropertyDescriptor, steps 3-4.
  if ("enumerable" in attributes) {
    attrs |= attributes.enumerable ? ATTR_ENUMERABLE : ATTR_NONENUMERABLE;
  }

  // 6.2.4.5 ToPropertyDescriptor, steps 5-6.
  if ("configurable" in attributes) {
    attrs |= attributes.configurable ? ATTR_CONFIGURABLE : ATTR_NONCONFIGURABLE;
  }

  // 6.2.4.5 ToPropertyDescriptor, steps 7-8.
  if ("value" in attributes) {
    attrs |= DATA_DESCRIPTOR_KIND;
    value = attributes.value;
    hasValue = true;
  }

  // 6.2.4.5 ToPropertyDescriptor, steps 9-10.
  if ("writable" in attributes) {
    attrs |= DATA_DESCRIPTOR_KIND;
    attrs |= attributes.writable ? ATTR_WRITABLE : ATTR_NONWRITABLE;
  }

  // 6.2.4.5 ToPropertyDescriptor, steps 11-12.
  if ("get" in attributes) {
    attrs |= ACCESSOR_DESCRIPTOR_KIND;
    getter = attributes.get;
    if (!IsCallable(getter) && getter !== undefined) {
      ThrowTypeError(JSMSG_BAD_GET_SET_FIELD, "get");
    }
  }

  // 6.2.4.5 ToPropertyDescriptor, steps 13-14.
  if ("set" in attributes) {
    attrs |= ACCESSOR_DESCRIPTOR_KIND;
    setter = attributes.set;
    if (!IsCallable(setter) && setter !== undefined) {
      ThrowTypeError(JSMSG_BAD_GET_SET_FIELD, "set");
    }
  }

  if (attrs & ACCESSOR_DESCRIPTOR_KIND) {
    // 6.2.4.5 ToPropertyDescriptor, step 15.
    if (attrs & DATA_DESCRIPTOR_KIND) {
      ThrowTypeError(JSMSG_INVALID_DESCRIPTOR);
    }

    // Step 4 (accessor descriptor property).
    return DefineProperty(obj, propertyKey, attrs, getter, setter, strict);
  }

  // Step 4 (data property descriptor with value).
  if (hasValue) {
    // Use the inlinable DefineDataProperty function when possible.
    if (strict) {
      if (
        (attrs & (ATTR_ENUMERABLE | ATTR_CONFIGURABLE | ATTR_WRITABLE)) ===
        (ATTR_ENUMERABLE | ATTR_CONFIGURABLE | ATTR_WRITABLE)
      ) {
        DefineDataProperty(obj, propertyKey, value);
        return true;
      }
    }

    // The fifth argument is set to |null| to mark that |value| is present.
    return DefineProperty(obj, propertyKey, attrs, value, null, strict);
  }

  // Step 4 (generic property descriptor or data property without value).
  return DefineProperty(obj, propertyKey, attrs, undefined, undefined, strict);
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 19.1.2.4 Object.defineProperty ( O, P, Attributes )
function ObjectDefineProperty(obj, propertyKey, attributes) {
  // Steps 1-4.
  if (!ObjectOrReflectDefineProperty(obj, propertyKey, attributes, true)) {
    // Not standardized yet: https://github.com/tc39/ecma262/pull/688
    return null;
  }

  // Step 5.
  return obj;
}

// Proposal https://tc39.github.io/proposal-object-from-entries/
// 1. Object.fromEntries ( iterable )
function ObjectFromEntries(iter) {
  // We omit the usual step number comments here because they don't help.
  // This implementation inlines AddEntriesFromIterator and
  // CreateDataPropertyOnObject, so it looks more like the polyfill
  // <https://github.com/tc39/proposal-object-from-entries/blob/master/polyfill.js>
  // than the spec algorithm.
  const obj = {};

  for (const pair of allowContentIter(iter)) {
    if (!IsObject(pair)) {
      ThrowTypeError(JSMSG_INVALID_MAP_ITERABLE, "Object.fromEntries");
    }
    DefineDataProperty(obj, pair[0], pair[1]);
  }

  return obj;
}

// Proposal https://github.com/tc39/proposal-accessible-object-hasownproperty
// 1. Object.hasOwn ( O, P )
function ObjectHasOwn(O, P) {
  // Step 1.
  var obj = ToObject(O);
  // Step 2-3.
  return hasOwn(P, obj);
}
