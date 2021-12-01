/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES2017 draft rev a785b0832b071f505a694e1946182adeab84c972
// 7.3.17 CreateListFromArrayLike (obj [ , elementTypes ] )
function CreateListFromArrayLikeForArgs(obj) {
    // Step 1 (not applicable).

    // Step 2.
    assert(IsObject(obj), "object must be passed to CreateListFromArrayLikeForArgs");

    // Step 3.
    var len = ToLength(obj.length);

    // This version of CreateListFromArrayLike is only used for argument lists.
    if (len > MAX_ARGS_LENGTH)
        ThrowRangeError(JSMSG_TOO_MANY_ARGUMENTS);

    // Steps 4-6.
    var list = std_Array(len);
    for (var i = 0; i < len; i++)
        _DefineDataProperty(list, i, obj[i]);

    // Step 7.
    return list;
}

// ES2017 draft rev a785b0832b071f505a694e1946182adeab84c972
// 26.1.1 Reflect.apply ( target, thisArgument, argumentsList )
function Reflect_apply(target, thisArgument, argumentsList) {
    // Step 1.
    if (!IsCallable(target))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, target));

    // Step 2.
    if (!IsObject(argumentsList)) {
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT_ARG, "`argumentsList`", "Reflect.apply",
                       ToSource(argumentsList));
    }

    // Steps 2-4.
    return callFunction(std_Function_apply, target, thisArgument, argumentsList);
}

// ES2017 draft rev a785b0832b071f505a694e1946182adeab84c972
// 26.1.2 Reflect.construct ( target, argumentsList [ , newTarget ] )
function Reflect_construct(target, argumentsList/*, newTarget*/) {
    // Step 1.
    if (!IsConstructor(target))
        ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, DecompileArg(0, target));

    // Steps 2-3.
    var newTarget;
    if (arguments.length > 2) {
        newTarget = arguments[2];
        if (!IsConstructor(newTarget))
            ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, DecompileArg(2, newTarget));
    } else {
        newTarget = target;
    }

    // Step 4.
    if (!IsObject(argumentsList)) {
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT_ARG, "`argumentsList`", "Reflect.construct",
                       ToSource(argumentsList));
    }

    // Fast path when we can avoid calling CreateListFromArrayLikeForArgs().
    var args = (IsPackedArray(argumentsList) && argumentsList.length <= MAX_ARGS_LENGTH)
               ? argumentsList
               : CreateListFromArrayLikeForArgs(argumentsList);

    // Step 5.
    switch (args.length) {
      case 0:
        return constructContentFunction(target, newTarget);
      case 1:
        return constructContentFunction(target, newTarget, SPREAD(args, 1));
      case 2:
        return constructContentFunction(target, newTarget, SPREAD(args, 2));
      case 3:
        return constructContentFunction(target, newTarget, SPREAD(args, 3));
      case 4:
        return constructContentFunction(target, newTarget, SPREAD(args, 4));
      case 5:
        return constructContentFunction(target, newTarget, SPREAD(args, 5));
      case 6:
        return constructContentFunction(target, newTarget, SPREAD(args, 6));
      case 7:
        return constructContentFunction(target, newTarget, SPREAD(args, 7));
      case 8:
        return constructContentFunction(target, newTarget, SPREAD(args, 8));
      case 9:
        return constructContentFunction(target, newTarget, SPREAD(args, 9));
      case 10:
        return constructContentFunction(target, newTarget, SPREAD(args, 10));
      case 11:
        return constructContentFunction(target, newTarget, SPREAD(args, 11));
      case 12:
        return constructContentFunction(target, newTarget, SPREAD(args, 12));
      default:
        return _ConstructFunction(target, newTarget, args);
    }
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 26.1.3 Reflect.defineProperty ( target, propertyKey, attributes )
function Reflect_defineProperty(obj, propertyKey, attributes) {
    // Steps 1-4.
    return ObjectOrReflectDefineProperty(obj, propertyKey, attributes, false);
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 26.1.6 Reflect.getOwnPropertyDescriptor ( target, propertyKey )
function Reflect_getOwnPropertyDescriptor(target, propertyKey) {
    // Step 1.
    if (!IsObject(target))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, DecompileArg(0, target));

    // Steps 2-3.
    // The other steps are identical to Object.getOwnPropertyDescriptor().
    return ObjectGetOwnPropertyDescriptor(target, propertyKey);
}

// ES2017 draft rev a785b0832b071f505a694e1946182adeab84c972
// 26.1.8 Reflect.has ( target, propertyKey )
function Reflect_has(target, propertyKey) {
    // Step 1.
    if (!IsObject(target)) {
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT_ARG, "`target`", "Reflect.has",
                       ToSource(target));
    }

    // Steps 2-3 are identical to the runtime semantics of the "in" operator.
    return propertyKey in target;
}

// ES2018 draft rev 0525bb33861c7f4e9850f8a222c89642947c4b9c
// 26.1.5 Reflect.get ( target, propertyKey [ , receiver ] )
function Reflect_get(target, propertyKey/*, receiver*/) {
    // Step 1.
    if (!IsObject(target)) {
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT_ARG, "`target`", "Reflect.get",
                       ToSource(target));
    }

    // Step 3 (reordered).
    if (arguments.length > 2) {
        // Steps 2, 4.
        return getPropertySuper(target, propertyKey, arguments[2]);
    }

    // Steps 2, 4.
    return target[propertyKey];
}
