var std_Symbol = Symbol;
function List() {
    this.length = 0;
}
MakeConstructible(List, {__proto__: null});
function Record() {
    return std_Object_create(null);
}
MakeConstructible(Record, {});
function ToBoolean(v) {
    return !!v;
}
function ToNumber(v) {
    return +v;
}
function RequireObjectCoercible(v) {
    if (v === undefined || v === null)
        ThrowTypeError(11, ToString(v), "object");
}
function ToLength(v) {
    v = ToInteger(v);
    v = std_Math_max(v, 0);
    return std_Math_min(v, 0x1fffffffffffff);
}
function SameValue(x, y) {
    if (x === y) {
        return (x !== 0) || (1 / x === 1 / y);
    }
    return (x !== x && y !== y);
}
function SameValueZero(x, y) {
    return x === y || (x !== x && y !== y);
}
function GetMethod(V, P) {
    ;;
    var func = V[P];
    if (func === undefined || func === null)
        return undefined;
    if (!IsCallable(func))
        ThrowTypeError(9, typeof func);
    return func;
}
function IsPropertyKey(argument) {
    var type = typeof argument;
    return type === "string" || type === "symbol";
}
var _builtinCtorsCache = {__proto__: null};
function GetBuiltinConstructor(builtinName) {
    var ctor = _builtinCtorsCache[builtinName] ||
               (_builtinCtorsCache[builtinName] = GetBuiltinConstructorImpl(builtinName));
    ;;
    return ctor;
}
function GetBuiltinPrototype(builtinName) {
    return (_builtinCtorsCache[builtinName] || GetBuiltinConstructor(builtinName)).prototype;
}
function SpeciesConstructor(obj, defaultConstructor) {
    ;;
    var ctor = obj.constructor;
    if (ctor === undefined)
        return defaultConstructor;
    if (!IsObject(ctor))
        ThrowTypeError(40, "object's 'constructor' property");
    var s = ctor[std_species];
    if (s === undefined || s === null)
        return defaultConstructor;
    if (IsConstructor(s))
        return s;
    ThrowTypeError(10, "@@species property of object's constructor");
}
function GetTypeError(msg) {
    try {
        callFunction(std_Function_apply, ThrowTypeError, undefined, arguments);
    } catch (e) {
        return e;
    }
    ;;
}
function GetInternalError(msg) {
    try {
        callFunction(std_Function_apply, ThrowInternalError, undefined, arguments);
    } catch (e) {
        return e;
    }
    ;;
}
function NullFunction() {}
function CopyDataProperties(target, source, excluded) {
    ;;
    ;;
    if (source === undefined || source === null)
        return;
    source = ToObject(source);
    var keys = OwnPropertyKeys(source);
    for (var index = 0; index < keys.length; index++) {
        var key = keys[index];
        if (!hasOwn(key, excluded) && callFunction(std_Object_propertyIsEnumerable, source, key))
            _DefineDataProperty(target, key, source[key]);
    }
}
function CopyDataPropertiesUnfiltered(target, source) {
    ;;
    if (source === undefined || source === null)
        return;
    source = ToObject(source);
    var keys = OwnPropertyKeys(source);
    for (var index = 0; index < keys.length; index++) {
        var key = keys[index];
        if (callFunction(std_Object_propertyIsEnumerable, source, key))
            _DefineDataProperty(target, key, source[key]);
    }
}
function outer() {
    return function inner() {
        return "foo";
    };
}
function ArrayIndexOf(searchElement ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (len === 0)
        return -1;
    var n = arguments.length > 1 ? ToInteger(arguments[1]) + 0 : 0;
    if (n >= len)
        return -1;
    var k;
    if (n >= 0)
        k = n;
    else {
        k = len + n;
        if (k < 0)
            k = 0;
    }
    for (; k < len; k++) {
        if (k in O && O[k] === searchElement)
            return k;
    }
    return -1;
}
function ArrayStaticIndexOf(list, searchElement ) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.indexOf");
    var fromIndex = arguments.length > 2 ? arguments[2] : 0;
    return callFunction(ArrayIndexOf, list, searchElement, fromIndex);
}
function ArrayLastIndexOf(searchElement ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (len === 0)
        return -1;
    var n = arguments.length > 1 ? ToInteger(arguments[1]) + 0 : len - 1;
    var k;
    if (n > len - 1)
        k = len - 1;
    else if (n < 0)
        k = len + n;
    else
        k = n;
    for (; k >= 0; k--) {
        if (k in O && O[k] === searchElement)
            return k;
    }
    return -1;
}
function ArrayStaticLastIndexOf(list, searchElement ) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.lastIndexOf");
    var fromIndex;
    if (arguments.length > 2) {
        fromIndex = arguments[2];
    } else {
        var O = ToObject(list);
        var len = ToLength(O.length);
        fromIndex = len - 1;
    }
    return callFunction(ArrayLastIndexOf, list, searchElement, fromIndex);
}
function ArrayEvery(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.every");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        if (k in O) {
            if (!callContentFunction(callbackfn, T, O[k], k, O))
                return false;
        }
    }
    return true;
}
function ArrayStaticEvery(list, callbackfn ) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.every");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    var T = arguments.length > 2 ? arguments[2] : void 0;
    return callFunction(ArrayEvery, list, callbackfn, T);
}
function ArraySome(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.some");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        if (k in O) {
            if (callContentFunction(callbackfn, T, O[k], k, O))
                return true;
        }
    }
    return false;
}
function ArrayStaticSome(list, callbackfn ) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.some");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    var T = arguments.length > 2 ? arguments[2] : void 0;
    return callFunction(ArraySome, list, callbackfn, T);
}
function ArraySort(comparefn) {
    if (comparefn !== undefined) {
        if (!IsCallable(comparefn))
            ThrowTypeError(5);
    }
    var O = ToObject(this);
    if (callFunction(ArrayNativeSort, O, comparefn))
        return O;
    var len = ToLength(O.length);
    if (len <= 1)
      return O;
    var wrappedCompareFn = comparefn;
    comparefn = function(x, y) {
        if (x === undefined) {
            if (y === undefined)
                return 0;
           return 1;
        }
        if (y === undefined)
            return -1;
        var v = ToNumber(wrappedCompareFn(x, y));
        return v !== v ? 0 : v;
    };
    return MergeSort(O, len, comparefn);
}
function ArrayForEach(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.forEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        if (k in O) {
            callContentFunction(callbackfn, T, O[k], k, O);
        }
    }
    return void 0;
}
function ArrayStaticForEach(list, callbackfn ) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.forEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    var T = arguments.length > 2 ? arguments[2] : void 0;
    callFunction(ArrayForEach, list, callbackfn, T);
}
function ArrayMap(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.map");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    var A = ArraySpeciesCreate(O, len);
    for (var k = 0; k < len; k++) {
        if (k in O) {
            var mappedValue = callContentFunction(callbackfn, T, O[k], k, O);
            _DefineDataProperty(A, k, mappedValue);
        }
    }
    return A;
}
function ArrayStaticMap(list, callbackfn ) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.map");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    var T = arguments.length > 2 ? arguments[2] : void 0;
    return callFunction(ArrayMap, list, callbackfn, T);
}
function ArrayFilter(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.filter");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    var A = ArraySpeciesCreate(O, 0);
    for (var k = 0, to = 0; k < len; k++) {
        if (k in O) {
            var kValue = O[k];
            var selected = callContentFunction(callbackfn, T, kValue, k, O);
            if (selected)
                _DefineDataProperty(A, to++, kValue);
        }
    }
    return A;
}
function ArrayStaticFilter(list, callbackfn ) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.filter");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    var T = arguments.length > 2 ? arguments[2] : void 0;
    return callFunction(ArrayFilter, list, callbackfn, T);
}
function ArrayReduce(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.reduce");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var k = 0;
    var accumulator;
    if (arguments.length > 1) {
        accumulator = arguments[1];
    } else {
        if (len === 0)
            ThrowTypeError(37);
        if (IsPackedArray(O)) {
            accumulator = O[k++];
        } else {
            var kPresent = false;
            for (; k < len; k++) {
                if (k in O) {
                    accumulator = O[k];
                    kPresent = true;
                    k++;
                    break;
                }
            }
            if (!kPresent)
              ThrowTypeError(37);
        }
    }
    for (; k < len; k++) {
        if (k in O) {
            accumulator = callbackfn(accumulator, O[k], k, O);
        }
    }
    return accumulator;
}
function ArrayStaticReduce(list, callbackfn) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.reduce");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    if (arguments.length > 2)
        return callFunction(ArrayReduce, list, callbackfn, arguments[2]);
    return callFunction(ArrayReduce, list, callbackfn);
}
function ArrayReduceRight(callbackfn ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.reduce");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var k = len - 1;
    var accumulator;
    if (arguments.length > 1) {
        accumulator = arguments[1];
    } else {
        if (len === 0)
            ThrowTypeError(37);
        if (IsPackedArray(O)) {
            accumulator = O[k--];
        } else {
            var kPresent = false;
            for (; k >= 0; k--) {
                if (k in O) {
                    accumulator = O[k];
                    kPresent = true;
                    k--;
                    break;
                }
            }
            if (!kPresent)
                ThrowTypeError(37);
        }
    }
    for (; k >= 0; k--) {
        if (k in O) {
            accumulator = callbackfn(accumulator, O[k], k, O);
        }
    }
    return accumulator;
}
function ArrayStaticReduceRight(list, callbackfn) {
    if (arguments.length < 2)
        ThrowTypeError(39, 0, "Array.reduceRight");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(1, callbackfn));
    if (arguments.length > 2)
        return callFunction(ArrayReduceRight, list, callbackfn, arguments[2]);
    return callFunction(ArrayReduceRight, list, callbackfn);
}
function ArrayFind(predicate ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.find");
    if (!IsCallable(predicate))
        ThrowTypeError(9, DecompileArg(0, predicate));
    var T = arguments.length > 1 ? arguments[1] : undefined;
    for (var k = 0; k < len; k++) {
        var kValue = O[k];
        if (callContentFunction(predicate, T, kValue, k, O))
            return kValue;
    }
    return undefined;
}
function ArrayFindIndex(predicate ) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "Array.prototype.find");
    if (!IsCallable(predicate))
        ThrowTypeError(9, DecompileArg(0, predicate));
    var T = arguments.length > 1 ? arguments[1] : undefined;
    for (var k = 0; k < len; k++) {
        if (callContentFunction(predicate, T, O[k], k, O))
            return k;
    }
    return -1;
}
function ArrayCopyWithin(target, start, end = undefined) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    var relativeTarget = ToInteger(target);
    var to = relativeTarget < 0 ? std_Math_max(len + relativeTarget, 0)
                                : std_Math_min(relativeTarget, len);
    var relativeStart = ToInteger(start);
    var from = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                 : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len
                                        : ToInteger(end);
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);
    var count = std_Math_min(final - from, len - to);
    if (from < to && to < (from + count)) {
        from = from + count - 1;
        to = to + count - 1;
        while (count > 0) {
            if (from in O)
                O[to] = O[from];
            else
                delete O[to];
            from--;
            to--;
            count--;
        }
    } else {
        while (count > 0) {
            if (from in O)
                O[to] = O[from];
            else
                delete O[to];
            from++;
            to++;
            count--;
        }
    }
    return O;
}
function ArrayFill(value, start = 0, end = undefined) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    var relativeStart = ToInteger(start);
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len : ToInteger(end);
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);
    for (; k < final; k++) {
        O[k] = value;
    }
    return O;
}
function ArrayIncludes(searchElement, fromIndex = 0) {
    var O = ToObject(this);
    var len = ToLength(O.length);
    if (len === 0)
        return false;
    var n = ToInteger(fromIndex);
    var k;
    if (n >= 0) {
        k = n;
    }
    else {
        k = len + n;
        if (k < 0)
            k = 0;
    }
    while (k < len) {
        if (SameValueZero(searchElement, O[k]))
            return true;
        k++;
    }
    return false;
}
function CreateArrayIterator(obj, kind) {
    var iteratedObject = ToObject(obj);
    var iterator = NewArrayIterator();
    UnsafeSetReservedSlot(iterator, 0, iteratedObject);
    UnsafeSetReservedSlot(iterator, 1, 0);
    UnsafeSetReservedSlot(iterator, 2, kind);
    return iterator;
}
function ArrayIteratorNext() {
    var obj;
    if (!IsObject(this) || (obj = GuardToArrayIterator(this)) === null) {
        return callFunction(CallArrayIteratorMethodIfWrapped, this,
                            "ArrayIteratorNext");
    }
    var a = UnsafeGetReservedSlot(obj, 0);
    var result = { value: undefined, done: false };
    if (a === null) {
      result.done = true;
      return result;
    }
    var index = UnsafeGetReservedSlot(obj, 1);
    var itemKind = UnsafeGetInt32FromReservedSlot(obj, 2);
    var len;
    if (IsPossiblyWrappedTypedArray(a)) {
        len = PossiblyWrappedTypedArrayLength(a);
        if (len === 0) {
            if (PossiblyWrappedTypedArrayHasDetachedBuffer(a))
                ThrowTypeError(461);
        }
    } else {
        len = ToLength(a.length);
    }
    if (index >= len) {
        UnsafeSetReservedSlot(obj, 0, null);
        result.done = true;
        return result;
    }
    UnsafeSetReservedSlot(obj, 1, index + 1);
    if (itemKind === 1) {
        result.value = a[index];
        return result;
    }
    if (itemKind === 2) {
        var pair = [index, a[index]];
        result.value = pair;
        return result;
    }
    ;;
    result.value = index;
    return result;
}
function ArrayValues() {
    return CreateArrayIterator(this, 1);
}
_SetCanonicalName(ArrayValues, "values");
function ArrayEntries() {
    return CreateArrayIterator(this, 2);
}
function ArrayKeys() {
    return CreateArrayIterator(this, 0);
}
function ArrayFrom(items, mapfn = undefined, thisArg = undefined) {
    var C = this;
    var mapping = mapfn !== undefined;
    if (mapping && !IsCallable(mapfn))
        ThrowTypeError(9, DecompileArg(1, mapfn));
    var T = thisArg;
    var usingIterator = items[std_iterator];
    if (usingIterator !== undefined && usingIterator !== null) {
        if (!IsCallable(usingIterator))
            ThrowTypeError(53, DecompileArg(0, items));
        var A = IsConstructor(C) ? new C() : [];
        var iterator = MakeIteratorWrapper(items, usingIterator);
        var k = 0;
        for (var nextValue of allowContentIter(iterator)) {
            var mappedValue = mapping ? callContentFunction(mapfn, T, nextValue, k) : nextValue;
            _DefineDataProperty(A, k++, mappedValue);
        }
        A.length = k;
        return A;
    }
    var arrayLike = ToObject(items);
    var len = ToLength(arrayLike.length);
    var A = IsConstructor(C) ? new C(len) : std_Array(len);
    for (var k = 0; k < len; k++) {
        var kValue = items[k];
        var mappedValue = mapping ? callContentFunction(mapfn, T, kValue, k) : kValue;
        _DefineDataProperty(A, k, mappedValue);
    }
    A.length = len;
    return A;
}
function MakeIteratorWrapper(items, method) {
    ;;
    return {
        [std_iterator]: function IteratorMethod() {
            return callContentFunction(method, items);
        }
    };
}
function ArrayToString() {
    var array = ToObject(this);
    var func = array.join;
    if (!IsCallable(func))
        return callFunction(std_Object_toString, array);
    return callContentFunction(func, array);
}
function ArrayToLocaleString(locales, options) {
    ;;
    var array = this;
    var len = ToLength(array.length);
    if (len === 0)
        return "";
    var firstElement = array[0];
    var R;
    if (firstElement === undefined || firstElement === null) {
        R = "";
    } else {
        R = ToString(callContentFunction(firstElement.toLocaleString, firstElement));
    }
    var separator = ",";
    for (var k = 1; k < len; k++) {
        var nextElement = array[k];
        R += separator;
        if (!(nextElement === undefined || nextElement === null)) {
            R += ToString(callContentFunction(nextElement.toLocaleString, nextElement));
        }
    }
    return R;
}
function ArraySpecies() {
    return this;
}
_SetCanonicalName(ArraySpecies, "get [Symbol.species]");
function ArraySpeciesCreate(originalArray, length) {
    ;;
    ;;
    if (length === -0)
        length = 0;
    if (!IsArray(originalArray))
        return std_Array(length);
    var C = originalArray.constructor;
    if (IsConstructor(C) && IsWrappedArrayConstructor(C))
        return std_Array(length);
    if (IsObject(C)) {
        C = C[std_species];
        if (C === GetBuiltinConstructor("Array"))
            return std_Array(length);
        if (C === null)
            return std_Array(length);
    }
    if (C === undefined)
        return std_Array(length);
    if (!IsConstructor(C))
        ThrowTypeError(10, "constructor property");
    return new C(length);
}
function IsConcatSpreadable(O) {
    if (!IsObject(O))
        return false;
    var spreadable = O[std_isConcatSpreadable];
    if (spreadable !== undefined)
        return ToBoolean(spreadable);
    return IsArray(O);
}
function ArrayConcat(arg1) {
    var O = ToObject(this);
    var A = ArraySpeciesCreate(O, 0);
    var n = 0;
    var i = 0, argsLen = arguments.length;
    var E = O;
    var k, len;
    while (true) {
        if (IsConcatSpreadable(E)) {
            len = ToLength(E.length);
            if (n + len > 0x1fffffffffffff)
                ThrowTypeError(454);
            if (IsPackedArray(A) && IsPackedArray(E)) {
                for (k = 0; k < len; k++) {
                    _DefineDataProperty(A, n, E[k]);
                    n++;
                }
            } else {
                for (k = 0; k < len; k++) {
                    if (k in E)
                        _DefineDataProperty(A, n, E[k]);
                    n++;
                }
            }
        } else {
            if (n >= 0x1fffffffffffff)
                ThrowTypeError(454);
            _DefineDataProperty(A, n, E);
            n++;
        }
        if (i >= argsLen)
            break;
        E = arguments[i];
        i++;
    }
    A.length = n;
    return A;
}
function ArrayFlatMap(mapperFunction ) {
    var O = ToObject(this);
    var sourceLen = ToLength(O.length);
    if (!IsCallable(mapperFunction))
        ThrowTypeError(9, DecompileArg(0, mapperFunction));
    var T = arguments.length > 1 ? arguments[1] : undefined;
    var A = ArraySpeciesCreate(O, 0);
    FlattenIntoArray(A, O, sourceLen, 0, 1, mapperFunction, T);
    return A;
}
function ArrayFlatten( ) {
    var O = ToObject(this);
    var sourceLen = ToLength(O.length);
    var depthNum = 1;
    if (arguments.length > 0 && arguments[0] !== undefined)
        depthNum = ToInteger(arguments[0]);
    var A = ArraySpeciesCreate(O, 0);
    FlattenIntoArray(A, O, sourceLen, 0, depthNum);
    return A;
}
function FlattenIntoArray(target, source, sourceLen, start, depth, mapperFunction, thisArg) {
    var targetIndex = start;
    for (var sourceIndex = 0; sourceIndex < sourceLen; sourceIndex++) {
        if (sourceIndex in source) {
            var element = source[sourceIndex];
            if (mapperFunction) {
                ;;
                element = callContentFunction(mapperFunction, thisArg, element, sourceIndex, source);
            }
            var flattenable = IsArray(element);
            if (flattenable && depth > 0) {
                var elementLen = ToLength(element.length);
                targetIndex = FlattenIntoArray(target, element, elementLen, targetIndex, depth - 1);
            } else {
                if (targetIndex >= 0x1fffffffffffff)
                    ThrowTypeError(454);
                _DefineDataProperty(target, targetIndex, element);
                targetIndex++;
            }
        }
    }
    return targetIndex;
}
function ArrayStaticConcat(arr, arg1) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.concat");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, ArrayConcat, arr, args);
}
function ArrayStaticJoin(arr, separator) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.join");
    return callFunction(std_Array_join, arr, separator);
}
function ArrayStaticReverse(arr) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.reverse");
    return callFunction(std_Array_reverse, arr);
}
function ArrayStaticSort(arr, comparefn) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.sort");
    return callFunction(ArraySort, arr, comparefn);
}
function ArrayStaticPush(arr, arg1) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.push");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, std_Array_push, arr, args);
}
function ArrayStaticPop(arr) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.pop");
    return callFunction(std_Array_pop, arr);
}
function ArrayStaticShift(arr) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.shift");
    return callFunction(std_Array_shift, arr);
}
function ArrayStaticUnshift(arr, arg1) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.unshift");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, std_Array_unshift, arr, args);
}
function ArrayStaticSplice(arr, start, deleteCount) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.splice");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, std_Array_splice, arr, args);
}
function ArrayStaticSlice(arr, start, end) {
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "Array.slice");
    return callFunction(std_Array_slice, arr, start, end);
}
function AsyncIteratorIdentity() {
    return this;
}
var DefaultDerivedClassConstructor =
    class extends null {
        constructor(...args) {
            super(...allowContentIter(args));
        }
    };
MakeDefaultConstructor(DefaultDerivedClassConstructor);
var DefaultBaseClassConstructor =
    class {
        constructor() { }
    };
MakeDefaultConstructor(DefaultBaseClassConstructor);
var dateTimeFormatCache = new Record();
function GetCachedFormat(format, required, defaults) {
    ;
                                 ;
    var formatters;
    if (!IsRuntimeDefaultLocale(dateTimeFormatCache.runtimeDefaultLocale) ||
        !intl_isDefaultTimeZone(dateTimeFormatCache.icuDefaultTimeZone))
    {
        formatters = dateTimeFormatCache.formatters = new Record();
        dateTimeFormatCache.runtimeDefaultLocale = RuntimeDefaultLocale();
        dateTimeFormatCache.icuDefaultTimeZone = intl_defaultTimeZone();
    } else {
        formatters = dateTimeFormatCache.formatters;
    }
    var fmt = formatters[format];
    if (fmt === undefined) {
        var options = ToDateTimeOptions(undefined, required, defaults);
        fmt = formatters[format] = intl_DateTimeFormat(undefined, options);
    }
    return fmt;
}
function Date_toLocaleString() {
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        dateTimeFormat = GetCachedFormat("dateTimeFormat", "any", "all");
    } else {
        options = ToDateTimeOptions(options, "any", "all");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }
    return intl_FormatDateTime(dateTimeFormat, x, false);
}
function Date_toLocaleDateString() {
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        dateTimeFormat = GetCachedFormat("dateFormat", "date", "date");
    } else {
        options = ToDateTimeOptions(options, "date", "date");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }
    return intl_FormatDateTime(dateTimeFormat, x, false);
}
function Date_toLocaleTimeString() {
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        dateTimeFormat = GetCachedFormat("timeFormat", "time", "time");
    } else {
        options = ToDateTimeOptions(options, "time", "time");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }
    return intl_FormatDateTime(dateTimeFormat, x, false);
}
function ErrorToString()
{
  var obj = this;
  if (!IsObject(obj))
    ThrowTypeError(3, "Error", "toString", "value");
  var name = obj.name;
  name = (name === undefined) ? "Error" : ToString(name);
  var msg = obj.message;
  msg = (msg === undefined) ? "" : ToString(msg);
  if (name === "")
    return msg;
  if (msg === "")
    return name;
  return name + ": " + msg;
}
function ErrorToStringWithTrailingNewline()
{
  return callFunction(std_Function_apply, ErrorToString, this, []) + "\n";
}
function FunctionBind(thisArg, ...boundArgs) {
    var target = this;
    if (!IsCallable(target))
        ThrowTypeError(3, "Function", "bind", target);
    var F;
    var argCount = boundArgs.length;
    switch (argCount) {
      case 0:
        F = bind_bindFunction0(target, thisArg, boundArgs);
        break;
      case 1:
        F = bind_bindFunction1(target, thisArg, boundArgs);
        break;
      case 2:
        F = bind_bindFunction2(target, thisArg, boundArgs);
        break;
      default:
        F = bind_bindFunctionN(target, thisArg, boundArgs);
    }
    _FinishBoundFunctionInit(F, target, argCount);
    void std_Function_apply;
    return F;
}
function bind_bindFunction0(fun, thisArg, boundArgs) {
    return function bound() {
        if (false) void boundArgs;
        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget);
              case 1:
                return constructContentFunction(fun, newTarget, arguments[0]);
              case 2:
                return constructContentFunction(fun, newTarget, arguments[0], arguments[1]);
              case 3:
                return constructContentFunction(fun, newTarget, arguments[0], arguments[1], arguments[2]);
              case 4:
                return constructContentFunction(fun, newTarget, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return constructContentFunction(fun, newTarget, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
              default:
                var args = callFunction(std_Function_apply, bind_mapArguments, null, arguments);
                return bind_constructFunctionN(fun, newTarget, args);
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg);
              case 1:
                return callContentFunction(fun, thisArg, arguments[0]);
              case 2:
                return callContentFunction(fun, thisArg, arguments[0], arguments[1]);
              case 3:
                return callContentFunction(fun, thisArg, arguments[0], arguments[1], arguments[2]);
              case 4:
                return callContentFunction(fun, thisArg, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return callContentFunction(fun, thisArg, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
              default:
                return callFunction(std_Function_apply, fun, thisArg, arguments);
            }
        }
    };
}
function bind_bindFunction1(fun, thisArg, boundArgs) {
    var bound1 = boundArgs[0];
    var combiner = null;
    return function bound() {
        if (false) void boundArgs;
        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget, bound1);
              case 1:
                return constructContentFunction(fun, newTarget, bound1, arguments[0]);
              case 2:
                return constructContentFunction(fun, newTarget, bound1, arguments[0], arguments[1]);
              case 3:
                return constructContentFunction(fun, newTarget, bound1, arguments[0], arguments[1], arguments[2]);
              case 4:
                return constructContentFunction(fun, newTarget, bound1, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return constructContentFunction(fun, newTarget, bound1, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg, bound1);
              case 1:
                return callContentFunction(fun, thisArg, bound1, arguments[0]);
              case 2:
                return callContentFunction(fun, thisArg, bound1, arguments[0], arguments[1]);
              case 3:
                return callContentFunction(fun, thisArg, bound1, arguments[0], arguments[1], arguments[2]);
              case 4:
                return callContentFunction(fun, thisArg, bound1, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return callContentFunction(fun, thisArg, bound1, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
            }
        }
        if (combiner === null) {
            combiner = function() {
                var callArgsCount = arguments.length;
                var args = std_Array(1 + callArgsCount);
                _DefineDataProperty(args, 0, bound1);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + 1, arguments[i]);
                return args;
            };
        }
        var args = callFunction(std_Function_apply, combiner, null, arguments);
        if (newTarget === undefined)
            return bind_applyFunctionN(fun, thisArg, args);
        return bind_constructFunctionN(fun, newTarget, args);
    };
}
function bind_bindFunction2(fun, thisArg, boundArgs) {
    var bound1 = boundArgs[0];
    var bound2 = boundArgs[1];
    var combiner = null;
    return function bound() {
        if (false) void boundArgs;
        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget, bound1, bound2);
              case 1:
                return constructContentFunction(fun, newTarget, bound1, bound2, arguments[0]);
              case 2:
                return constructContentFunction(fun, newTarget, bound1, bound2, arguments[0], arguments[1]);
              case 3:
                return constructContentFunction(fun, newTarget, bound1, bound2, arguments[0], arguments[1], arguments[2]);
              case 4:
                return constructContentFunction(fun, newTarget, bound1, bound2, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return constructContentFunction(fun, newTarget, bound1, bound2, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg, bound1, bound2);
              case 1:
                return callContentFunction(fun, thisArg, bound1, bound2, arguments[0]);
              case 2:
                return callContentFunction(fun, thisArg, bound1, bound2, arguments[0], arguments[1]);
              case 3:
                return callContentFunction(fun, thisArg, bound1, bound2, arguments[0], arguments[1], arguments[2]);
              case 4:
                return callContentFunction(fun, thisArg, bound1, bound2, arguments[0], arguments[1], arguments[2], arguments[3]);
              case 5:
                return callContentFunction(fun, thisArg, bound1, bound2, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
            }
        }
        if (combiner === null) {
            combiner = function() {
                var callArgsCount = arguments.length;
                var args = std_Array(2 + callArgsCount);
                _DefineDataProperty(args, 0, bound1);
                _DefineDataProperty(args, 1, bound2);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + 2, arguments[i]);
                return args;
            };
        }
        var args = callFunction(std_Function_apply, combiner, null, arguments);
        if (newTarget === undefined)
            return bind_applyFunctionN(fun, thisArg, args);
        return bind_constructFunctionN(fun, newTarget, args);
    };
}
function bind_bindFunctionN(fun, thisArg, boundArgs) {
    ;;
    var combiner = null;
    return function bound() {
        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
        }
        if (arguments.length === 0) {
            if (newTarget !== undefined)
                return bind_constructFunctionN(fun, newTarget, boundArgs);
            return bind_applyFunctionN(fun, thisArg, boundArgs);
        }
        if (combiner === null) {
            combiner = function() {
                var boundArgsCount = boundArgs.length;
                var callArgsCount = arguments.length;
                var args = std_Array(boundArgsCount + callArgsCount);
                for (var i = 0; i < boundArgsCount; i++)
                    _DefineDataProperty(args, i, boundArgs[i]);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + boundArgsCount, arguments[i]);
                return args;
            };
        }
        var args = callFunction(std_Function_apply, combiner, null, arguments);
        if (newTarget !== undefined)
            return bind_constructFunctionN(fun, newTarget, args);
        return bind_applyFunctionN(fun, thisArg, args);
    };
}
function bind_mapArguments() {
    var len = arguments.length;
    var args = std_Array(len);
    for (var i = 0; i < len; i++)
        _DefineDataProperty(args, i, arguments[i]);
    return args;
}
function bind_applyFunctionN(fun, thisArg, args) {
    switch (args.length) {
      case 0:
        return callContentFunction(fun, thisArg);
      case 1:
        return callContentFunction(fun, thisArg, args[0]);
      case 2:
        return callContentFunction(fun, thisArg, args[0], args[1]);
      case 3:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2]);
      case 4:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3]);
      case 5:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3], args[4]);
      case 6:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3], args[4], args[5]);
      case 7:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
      case 8:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
      case 9:
        return callContentFunction(fun, thisArg, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
      default:
        return callFunction(std_Function_apply, fun, thisArg, args);
    }
}
function bind_constructFunctionN(fun, newTarget, args) {
    switch (args.length) {
      case 1:
        return constructContentFunction(fun, newTarget, args[0]);
      case 2:
        return constructContentFunction(fun, newTarget, args[0], args[1]);
      case 3:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2]);
      case 4:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3]);
      case 5:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4]);
      case 6:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5]);
      case 7:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
      case 8:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
      case 9:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
      case 10:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
      case 11:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
      case 12:
        return constructContentFunction(fun, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
      default:
        ;
                                                                                      ;
        return _ConstructFunction(fun, newTarget, args);
    }
}
function GeneratorNext(val) {
    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorNext");
        if (GeneratorObjectIsClosed(this))
            return { value: undefined, done: true };
        if (GeneratorIsRunning(this))
            ThrowTypeError(31);
    }
    try {
        return resumeGenerator(this, val, "next");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}
function GeneratorThrow(val) {
    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorThrow");
        if (GeneratorObjectIsClosed(this))
            throw val;
        if (GeneratorIsRunning(this))
            ThrowTypeError(31);
    }
    try {
        return resumeGenerator(this, val, "throw");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}
function GeneratorReturn(val) {
    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorReturn");
        if (GeneratorObjectIsClosed(this))
            return { value: val, done: true };
        if (GeneratorIsRunning(this))
            ThrowTypeError(31);
    }
    try {
        var rval = { value: val, done: true };
        return resumeGenerator(this, rval, "return");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}
function InterpretGeneratorResume(gen, val, kind) {
    forceInterpreter();
    if (kind === "next")
       return resumeGenerator(gen, val, "next");
    if (kind === "throw")
       return resumeGenerator(gen, val, "throw");
    ;;
    return resumeGenerator(gen, val, "return");
}
function resolveCollatorInternals(lazyCollatorData) {
    ;;
    var internalProps = std_Object_create(null);
    var Collator = collatorInternalProperties;
    internalProps.usage = lazyCollatorData.usage;
    var collatorIsSorting = lazyCollatorData.usage === "sort";
    var localeData = collatorIsSorting
                     ? Collator.sortLocaleData
                     : Collator.searchLocaleData;
    var relevantExtensionKeys = Collator.relevantExtensionKeys;
    var r = ResolveLocale(callFunction(Collator.availableLocales, Collator),
                          lazyCollatorData.requestedLocales,
                          lazyCollatorData.opt,
                          relevantExtensionKeys,
                          localeData);
    internalProps.locale = r.locale;
    var collation = r.co;
    if (collation === null)
        collation = "default";
    internalProps.collation = collation;
    internalProps.numeric = r.kn === "true";
    internalProps.caseFirst = r.kf;
    var s = lazyCollatorData.rawSensitivity;
    if (s === undefined) {
        s = "variant";
    }
    internalProps.sensitivity = s;
    internalProps.ignorePunctuation = lazyCollatorData.ignorePunctuation;
    return internalProps;
}
function getCollatorInternals(obj) {
    ;;
    ;;
    var internals = getIntlObjectInternals(obj);
    ;;
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    internalProps = resolveCollatorInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
function InitializeCollator(collator, locales, options) {
    ;;
    ;;
    var lazyCollatorData = std_Object_create(null);
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyCollatorData.requestedLocales = requestedLocales;
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);
    var u = GetOption(options, "usage", "string", ["sort", "search"], "sort");
    lazyCollatorData.usage = u;
    var opt = new Record();
    lazyCollatorData.opt = opt;
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;
    var numericValue = GetOption(options, "numeric", "boolean", undefined, undefined);
    if (numericValue !== undefined)
        numericValue = numericValue ? "true" : "false";
    opt.kn = numericValue;
    var caseFirstValue = GetOption(options, "caseFirst", "string", ["upper", "lower", "false"], undefined);
    opt.kf = caseFirstValue;
    var s = GetOption(options, "sensitivity", "string",
                      ["base", "accent", "case", "variant"], undefined);
    lazyCollatorData.rawSensitivity = s;
    var ip = GetOption(options, "ignorePunctuation", "boolean", undefined, false);
    lazyCollatorData.ignorePunctuation = ip;
    initializeIntlObject(collator, "Collator", lazyCollatorData);
}
function Intl_Collator_supportedLocalesOf(locales ) {
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var availableLocales = callFunction(collatorInternalProperties.availableLocales,
                                        collatorInternalProperties);
    var requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}
var collatorInternalProperties = {
    sortLocaleData: collatorSortLocaleData,
    searchLocaleData: collatorSearchLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        locales = intl_Collator_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: ["co", "kn", "kf"]
};
function collatorActualLocale(locale) {
    ;;
    var availableLocales = callFunction(collatorInternalProperties.availableLocales,
                                        collatorInternalProperties);
    return BestAvailableLocaleIgnoringDefault(availableLocales, locale);
}
function collatorSortCaseFirst(locale) {
    var actualLocale = collatorActualLocale(locale);
    if (intl_isUpperCaseFirst(actualLocale))
        return ["upper", "false", "lower"];
    return ["false", "lower", "upper"];
}
function collatorSortCaseFirstDefault(locale) {
    var actualLocale = collatorActualLocale(locale);
    if (intl_isUpperCaseFirst(actualLocale))
        return "upper";
    return "false";
}
function collatorSortLocaleData() {
    return {
        co: intl_availableCollations,
        kn: function() {
            return ["false", "true"];
        },
        kf: collatorSortCaseFirst,
        default: {
            co: function() {
                return null;
            },
            kn: function() {
                return "false";
            },
            kf: collatorSortCaseFirstDefault,
        }
    };
}
function collatorSearchLocaleData() {
    return {
        co: function() {
            return [null];
        },
        kn: function() {
            return ["false", "true"];
        },
        kf: function() {
            return ["false", "lower", "upper"];
        },
        default: {
            co: function() {
                return null;
            },
            kn: function() {
                return "false";
            },
            kf: function() {
                return "false";
            },
        }
    };
}
function collatorCompareToBind(x, y) {
    var collator = this;
    ;;
    ;;
    var X = ToString(x);
    var Y = ToString(y);
    return intl_CompareStrings(collator, X, Y);
}
function Intl_Collator_compare_get() {
    var collator = this;
    if (!IsObject(collator) || (collator = GuardToCollator(collator)) === null)
        ThrowTypeError(414, "Collator", "compare", "Collator");
    var internals = getCollatorInternals(collator);
    if (internals.boundCompare === undefined) {
        var F = callFunction(FunctionBind, collatorCompareToBind, collator);
        internals.boundCompare = F;
    }
    return internals.boundCompare;
}
_SetCanonicalName(Intl_Collator_compare_get, "get compare");
function Intl_Collator_resolvedOptions() {
    var collator = this;
    if (!IsObject(collator) || (collator = GuardToCollator(collator)) === null)
        ThrowTypeError(414, "Collator", "resolvedOptions", "Collator");
    var internals = getCollatorInternals(collator);
    var result = {
        locale: internals.locale,
        usage: internals.usage,
        sensitivity: internals.sensitivity,
        ignorePunctuation: internals.ignorePunctuation,
        collation: internals.collation,
        numeric: internals.numeric,
        caseFirst: internals.caseFirst,
    };
    return result;
}
function startOfUnicodeExtensions(locale) {
    ;;
    ;;
    ;;
    ;
                                                                    ;
    if (callFunction(std_String_charCodeAt, locale, 1) === 0x2D) {
        ;
                                                                                     ;
        return -1;
    }
    var start = callFunction(std_String_indexOf, locale, "-u-");
    if (start < 0)
        return -1;
    var privateExt = callFunction(std_String_indexOf, locale, "-x-");
    if (privateExt >= 0 && privateExt < start)
        return -1;
    return start;
}
function endOfUnicodeExtensions(locale, start) {
    ;;
    ;;
    ;;
    ;;
    ;;
    ;
                                                                    ;
    for (var i = start + 5, end = locale.length - 4; i <= end; i++) {
        if (callFunction(std_String_charCodeAt, locale, i) !== 0x2D)
            continue;
        if (callFunction(std_String_charCodeAt, locale, i + 2) === 0x2D)
            return i;
        i += 2;
    }
    return locale.length;
}
function removeUnicodeExtensions(locale) {
    var start = startOfUnicodeExtensions(locale);
    if (start < 0)
        return locale;
    var end = endOfUnicodeExtensions(locale, start);
    var left = Substring(locale, 0, start);
    var right = Substring(locale, end, locale.length - end);
    var combined = left + right;
    ;
                                                            ;
    ;
                                                                                   ;
    return combined;
}
function getUnicodeExtensions(locale) {
    var start = startOfUnicodeExtensions(locale);
    ;;
    var end = endOfUnicodeExtensions(locale, start);
    return Substring(locale, start, end - start);
}
function parseLanguageTag(locale) {
    ;;
    var index = 0;
    var token = 0;
    var tokenStart = 0;
    var tokenLength = 0;
    ;
                                                                      ;
    function nextToken() {
        var type = 0b00;
        for (var i = index; i < locale.length; i++) {
            var c = callFunction(std_String_charCodeAt, locale, i);
            if ((0x41 <= c && c <= 0x5A) || (0x61 <= c && c <= 0x7A))
                type |= 0b01;
            else if (0x30 <= c && c <= 0x39)
                type |= 0b10;
            else if (c === 0x2D && i > index && i + 1 < locale.length)
                break;
            else
                return false;
        }
        token = type;
        tokenStart = index;
        tokenLength = i - index;
        index = i + 1;
        return true;
    }
    var localeLowercase = callFunction(std_String_toLowerCase, locale);
    function tokenStartCodeUnitLower() {
        var c = callFunction(std_String_charCodeAt, localeLowercase, tokenStart);
        ;
                                      ;
        return c;
    }
    function tokenStringLower() {
        return Substring(localeLowercase, tokenStart, tokenLength);
    }
    if (!nextToken())
        return null;
    if (token !== 0b01 || tokenLength > 8)
        return null;
    ;;
    var language, extlang1, extlang2, extlang3, script, region, privateuse;
    var variants = [];
    var extensions = [];
    if (tokenLength > 1) {
        if (tokenLength <= 3) {
            language = tokenStringLower();
            if (!nextToken())
                return null;
            if (token === 0b01 && tokenLength === 3) {
                extlang1 = tokenStringLower();
                if (!nextToken())
                    return null;
                if (token === 0b01 && tokenLength === 3) {
                    extlang2 = tokenStringLower();
                    if (!nextToken())
                        return null;
                    if (token === 0b01 && tokenLength === 3) {
                        extlang3 = tokenStringLower();
                        if (!nextToken())
                            return null;
                    }
                }
            }
        } else {
            ;;
            language = tokenStringLower();
            if (!nextToken())
                return null;
        }
        if (tokenLength === 4 && token === 0b01) {
            script = tokenStringLower();
            if (!nextToken())
                return null;
        }
        if ((tokenLength === 2 && token === 0b01) || (tokenLength === 3 && token === 0b10)) {
            region = tokenStringLower();
            if (!nextToken())
                return null;
        }
        while ((5 <= tokenLength && tokenLength <= 8) ||
               (tokenLength === 4 && tokenStartCodeUnitLower() <= 0x39))
        {
            ;
                                                                                               ;
            var variant = tokenStringLower();
            if (callFunction(ArrayIndexOf, variants, variant) !== -1)
                return null;
            _DefineDataProperty(variants, variants.length, variant);
            if (!nextToken())
                return null;
        }
        var seenSingletons = [];
        while (tokenLength === 1) {
            var extensionStart = tokenStart;
            var singleton = tokenStartCodeUnitLower();
            if (singleton === 0x78)
                break;
            ;
                                                     ;
            if (callFunction(ArrayIndexOf, seenSingletons, singleton) !== -1)
                return null;
            _DefineDataProperty(seenSingletons, seenSingletons.length, singleton);
            if (!nextToken())
                return null;
            if (!(2 <= tokenLength && tokenLength <= 8))
                return null;
            do {
                if (!nextToken())
                    return null;
            } while (2 <= tokenLength && tokenLength <= 8);
            var extension = Substring(localeLowercase, extensionStart,
                                      (tokenStart - 1 - extensionStart));
            _DefineDataProperty(extensions, extensions.length, extension);
        }
    }
    if (tokenLength === 1 && tokenStartCodeUnitLower() === 0x78) {
        var privateuseStart = tokenStart;
        if (!nextToken())
            return null;
        if (!(1 <= tokenLength && tokenLength <= 8))
            return null;
        do {
            if (!nextToken())
                return null;
        } while (1 <= tokenLength && tokenLength <= 8);
        privateuse = Substring(localeLowercase, privateuseStart,
                               localeLowercase.length - privateuseStart);
    }
    if (token === 0b00) {
        return {
            locale: localeLowercase,
            language,
            extlang1,
            extlang2,
            extlang3,
            script,
            region,
            variants,
            extensions,
            privateuse,
        };
    }
    do {
        if (!nextToken())
            return null;
    } while (token !== 0b00);
    switch (localeLowercase) {
      case "en-gb-oed":
      case "i-ami":
      case "i-bnn":
      case "i-default":
      case "i-enochian":
      case "i-hak":
      case "i-klingon":
      case "i-lux":
      case "i-mingo":
      case "i-navajo":
      case "i-pwn":
      case "i-tao":
      case "i-tay":
      case "i-tsu":
      case "sgn-be-fr":
      case "sgn-be-nl":
      case "sgn-ch-de":
        return { locale: localeLowercase, grandfathered: true };
      default:
        return null;
    }
}
function IsStructurallyValidLanguageTag(locale) {
    return parseLanguageTag(locale) !== null;
}
function CanonicalizeLanguageTagFromObject(localeObj) {
    ;;
    var {locale} = localeObj;
    ;
                                                        ;
    if (hasOwn(locale, langTagMappings))
        return langTagMappings[locale];
    ;
                                                            ;
    var {
        language,
        extlang1,
        extlang2,
        extlang3,
        script,
        region,
        variants,
        extensions,
        privateuse,
    } = localeObj;
    if (!language) {
        ;;
        return privateuse;
    }
    if (hasOwn(language, languageMappings))
        language = languageMappings[language];
    var canonical = language;
    if (extlang1) {
        if (hasOwn(extlang1, extlangMappings) && extlangMappings[extlang1] === language)
            canonical = extlang1;
        else
            canonical += "-" + extlang1;
    }
    if (extlang2)
        canonical += "-" + extlang2;
    if (extlang3)
        canonical += "-" + extlang3;
    if (script) {
        script = callFunction(std_String_toUpperCase, script[0]) +
                 Substring(script, 1, script.length - 1);
        canonical += "-" + script;
    }
    if (region) {
        region = callFunction(std_String_toUpperCase, region);
        if (hasOwn(region, regionMappings))
            region = regionMappings[region];
        canonical += "-" + region;
    }
    if (variants.length > 0)
        canonical += "-" + callFunction(std_Array_join, variants, "-");
    if (extensions.length > 0) {
        callFunction(ArraySort, extensions);
        canonical += "-" + callFunction(std_Array_join, extensions, "-");
    }
    if (privateuse)
        canonical += "-" + privateuse;
    return canonical;
}
function CanonicalizeLanguageTag(locale) {
    var localeObj = parseLanguageTag(locale);
    ;;
    return CanonicalizeLanguageTagFromObject(localeObj);
}
function IsASCIIAlphaString(s) {
    ;;
    for (var i = 0; i < s.length; i++) {
        var c = callFunction(std_String_charCodeAt, s, i);
        if (!((0x41 <= c && c <= 0x5A) || (0x61 <= c && c <= 0x7A)))
            return false;
    }
    return true;
}
function ValidateAndCanonicalizeLanguageTag(locale) {
    ;;
    if (locale.length === 2 || (locale.length === 3 && locale[1] !== "-")) {
        if (!IsASCIIAlphaString(locale))
            ThrowRangeError(419, locale);
        ;;
        locale = callFunction(std_String_toLowerCase, locale);
        ;;
        locale = hasOwn(locale, languageMappings)
                 ? languageMappings[locale]
                 : locale;
        ;;
        return locale;
    }
    var localeObj = parseLanguageTag(locale);
    if (localeObj === null)
        ThrowRangeError(419, locale);
    return CanonicalizeLanguageTagFromObject(localeObj);
}
function lastDitchLocale() {
    return "en-GB";
}
var oldStyleLanguageTagMappings = {
    "pa-PK": "pa-Arab-PK",
    "zh-CN": "zh-Hans-CN",
    "zh-HK": "zh-Hant-HK",
    "zh-SG": "zh-Hans-SG",
    "zh-TW": "zh-Hant-TW",
};
var localeCandidateCache = {
    runtimeDefaultLocale: undefined,
    candidateDefaultLocale: undefined,
};
var localeCache = {
    runtimeDefaultLocale: undefined,
    defaultLocale: undefined,
};
function DefaultLocaleIgnoringAvailableLocales() {
    const runtimeDefaultLocale = RuntimeDefaultLocale();
    if (runtimeDefaultLocale === localeCandidateCache.runtimeDefaultLocale)
        return localeCandidateCache.candidateDefaultLocale;
    var candidate = parseLanguageTag(runtimeDefaultLocale);
    if (candidate === null) {
        candidate = lastDitchLocale();
    } else {
        candidate = CanonicalizeLanguageTagFromObject(candidate);
        candidate = removeUnicodeExtensions(candidate);
        if (hasOwn(candidate, oldStyleLanguageTagMappings))
            candidate = oldStyleLanguageTagMappings[candidate];
    }
    localeCandidateCache.candidateDefaultLocale = candidate;
    localeCandidateCache.runtimeDefaultLocale = runtimeDefaultLocale;
    ;
                                                      ;
    ;
                                                                         ;
    return candidate;
}
function DefaultLocale() {
    if (IsRuntimeDefaultLocale(localeCache.runtimeDefaultLocale))
        return localeCache.defaultLocale;
    var runtimeDefaultLocale = RuntimeDefaultLocale();
    var candidate = DefaultLocaleIgnoringAvailableLocales();
    var locale;
    if (BestAvailableLocaleIgnoringDefault(callFunction(collatorInternalProperties.availableLocales,
                                                        collatorInternalProperties),
                                           candidate) &&
        BestAvailableLocaleIgnoringDefault(callFunction(numberFormatInternalProperties.availableLocales,
                                                        numberFormatInternalProperties),
                                           candidate) &&
        BestAvailableLocaleIgnoringDefault(callFunction(dateTimeFormatInternalProperties.availableLocales,
                                                        dateTimeFormatInternalProperties),
                                           candidate))
    {
        locale = candidate;
    } else {
        locale = lastDitchLocale();
    }
    ;
                                                                    ;
    ;
                                                           ;
    ;
                                                                                       ;
    localeCache.defaultLocale = locale;
    localeCache.runtimeDefaultLocale = runtimeDefaultLocale;
    return locale;
}
function addSpecialMissingLanguageTags(availableLocales) {
    var oldStyleLocales = std_Object_getOwnPropertyNames(oldStyleLanguageTagMappings);
    for (var i = 0; i < oldStyleLocales.length; i++) {
        var oldStyleLocale = oldStyleLocales[i];
        if (availableLocales[oldStyleLanguageTagMappings[oldStyleLocale]])
            availableLocales[oldStyleLocale] = true;
    }
    var lastDitch = lastDitchLocale();
    ;
                                                             ;
    availableLocales[lastDitch] = true;
}
function CanonicalizeLocaleList(locales) {
    if (locales === undefined)
        return [];
    if (typeof locales === "string")
        return [ValidateAndCanonicalizeLanguageTag(locales)];
    var seen = [];
    var O = ToObject(locales);
    var len = ToLength(O.length);
    var k = 0;
    while (k < len) {
        if (k in O) {
            var kValue = O[k];
            if (!(typeof kValue === "string" || IsObject(kValue)))
                ThrowTypeError(420);
            var tag = ToString(kValue);
            tag = ValidateAndCanonicalizeLanguageTag(tag);
            if (callFunction(ArrayIndexOf, seen, tag) === -1)
                _DefineDataProperty(seen, seen.length, tag);
        }
        k++;
    }
    return seen;
}
function BestAvailableLocaleHelper(availableLocales, locale, considerDefaultLocale) {
    ;;
    ;;
    ;;
    var defaultLocale;
    if (considerDefaultLocale)
        defaultLocale = DefaultLocale();
    var candidate = locale;
    while (true) {
        if (availableLocales[candidate])
            return candidate;
        if (considerDefaultLocale && candidate.length <= defaultLocale.length) {
            if (candidate === defaultLocale)
                return candidate;
            if (callFunction(std_String_startsWith, defaultLocale, candidate + "-"))
                return candidate;
        }
        var pos = callFunction(std_String_lastIndexOf, candidate, "-");
        if (pos === -1)
            return undefined;
        if (pos >= 2 && candidate[pos - 2] === "-")
            pos -= 2;
        candidate = callFunction(String_substring, candidate, 0, pos);
    }
}
function BestAvailableLocale(availableLocales, locale) {
    return BestAvailableLocaleHelper(availableLocales, locale, true);
}
function BestAvailableLocaleIgnoringDefault(availableLocales, locale) {
    return BestAvailableLocaleHelper(availableLocales, locale, false);
}
function LookupMatcher(availableLocales, requestedLocales) {
    var result = new Record();
    for (var i = 0; i < requestedLocales.length; i++) {
        var locale = requestedLocales[i];
        var noExtensionsLocale = removeUnicodeExtensions(locale);
        var availableLocale = BestAvailableLocale(availableLocales, noExtensionsLocale);
        if (availableLocale !== undefined) {
            result.locale = availableLocale;
            if (locale !== noExtensionsLocale)
                result.extension = getUnicodeExtensions(locale);
            return result;
        }
    }
    result.locale = DefaultLocale();
    return result;
}
function BestFitMatcher(availableLocales, requestedLocales) {
    return LookupMatcher(availableLocales, requestedLocales);
}
function UnicodeExtensionValue(extension, key) {
    ;;
    ;
                                                     ;
    ;;
    ;;
    var size = extension.length;
    var searchValue = "-" + key + "-";
    var pos = callFunction(std_String_indexOf, extension, searchValue);
    if (pos !== -1) {
        var start = pos + 4;
        var end = start;
        var k = start;
        while (true) {
            var e = callFunction(std_String_indexOf, extension, "-", k);
            var len = e === -1 ? size - k : e - k;
            if (len === 2)
                break;
            if (e === -1) {
                end = size;
                break;
            }
            end = e;
            k = e + 1;
        }
        return callFunction(String_substring, extension, start, end);
    }
    searchValue = "-" + key;
    if (callFunction(std_String_endsWith, extension, searchValue))
        return "";
}
function ResolveLocale(availableLocales, requestedLocales, options, relevantExtensionKeys, localeData) {
    var matcher = options.localeMatcher;
    var r = (matcher === "lookup")
            ? LookupMatcher(availableLocales, requestedLocales)
            : BestFitMatcher(availableLocales, requestedLocales);
    var foundLocale = r.locale;
    var extension = r.extension;
    var result = new Record();
    result.dataLocale = foundLocale;
    var supportedExtension = "-u";
    var localeDataProvider = localeData();
    for (var i = 0; i < relevantExtensionKeys.length; i++) {
        var key = relevantExtensionKeys[i];
        var keyLocaleData = undefined;
        var value = undefined;
        var supportedExtensionAddition = "";
        if (extension !== undefined) {
            var requestedValue = UnicodeExtensionValue(extension, key);
            if (requestedValue !== undefined) {
                keyLocaleData = callFunction(localeDataProvider[key], null, foundLocale);
                if (requestedValue !== "") {
                    if (callFunction(ArrayIndexOf, keyLocaleData, requestedValue) !== -1) {
                        value = requestedValue;
                        supportedExtensionAddition = "-" + key + "-" + value;
                    }
                } else {
                    if (callFunction(ArrayIndexOf, keyLocaleData, "true") !== -1)
                        value = "true";
                }
            }
        }
        var optionsValue = options[key];
        ;
                                                   ;
        if (optionsValue !== undefined && optionsValue !== value) {
            if (keyLocaleData === undefined)
                keyLocaleData = callFunction(localeDataProvider[key], null, foundLocale);
            if (callFunction(ArrayIndexOf, keyLocaleData, optionsValue) !== -1) {
                value = optionsValue;
                supportedExtensionAddition = "";
            }
        }
        if (value === undefined) {
            value = keyLocaleData === undefined
                    ? callFunction(localeDataProvider.default[key], null, foundLocale)
                    : keyLocaleData[0];
        }
        ;;
        result[key] = value;
        supportedExtension += supportedExtensionAddition;
    }
    if (supportedExtension.length > 2) {
        ;
                                                                     ;
        var privateIndex = callFunction(std_String_indexOf, foundLocale, "-x-");
        if (privateIndex === -1) {
            foundLocale += supportedExtension;
        } else {
            var preExtension = callFunction(String_substring, foundLocale, 0, privateIndex);
            var postExtension = callFunction(String_substring, foundLocale, privateIndex);
            foundLocale = preExtension + supportedExtension + postExtension;
        }
        ;;
        ;;
    }
    result.locale = foundLocale;
    return result;
}
function LookupSupportedLocales(availableLocales, requestedLocales) {
    var subset = [];
    for (var i = 0; i < requestedLocales.length; i++) {
        var locale = requestedLocales[i];
        var noExtensionsLocale = removeUnicodeExtensions(locale);
        var availableLocale = BestAvailableLocale(availableLocales, noExtensionsLocale);
        if (availableLocale !== undefined)
            _DefineDataProperty(subset, subset.length, locale);
    }
    return subset;
}
function BestFitSupportedLocales(availableLocales, requestedLocales) {
    return LookupSupportedLocales(availableLocales, requestedLocales);
}
function SupportedLocales(availableLocales, requestedLocales, options) {
    var matcher;
    if (options !== undefined) {
        options = ToObject(options);
        matcher = options.localeMatcher;
        if (matcher !== undefined) {
            matcher = ToString(matcher);
            if (matcher !== "lookup" && matcher !== "best fit")
                ThrowRangeError(421, matcher);
        }
    }
    var subset = (matcher === undefined || matcher === "best fit")
                 ? BestFitSupportedLocales(availableLocales, requestedLocales)
                 : LookupSupportedLocales(availableLocales, requestedLocales);
    for (var i = 0; i < subset.length; i++) {
        _DefineDataProperty(subset, i, subset[i],
                            0x01 | 0x10 | 0x20);
    }
    _DefineDataProperty(subset, "length", subset.length,
                        0x08 | 0x10 | 0x20);
    return subset;
}
function GetOption(options, property, type, values, fallback) {
    var value = options[property];
    if (value !== undefined) {
        if (type === "boolean")
            value = ToBoolean(value);
        else if (type === "string")
            value = ToString(value);
        else
            ;;
        if (values !== undefined && callFunction(ArrayIndexOf, values, value) === -1)
            ThrowRangeError(422, property, value);
        return value;
    }
    return fallback;
}
function DefaultNumberOption(value, minimum, maximum, fallback) {
    ;;
    ;;
    ;;
    ;;
    if (value !== undefined) {
        value = ToNumber(value);
        if (Number_isNaN(value) || value < minimum || value > maximum)
            ThrowRangeError(416, value);
        return std_Math_floor(value) | 0;
    }
    return fallback;
}
function GetNumberOption(options, property, minimum, maximum, fallback) {
    return DefaultNumberOption(options[property], minimum, maximum, fallback);
}
var intlFallbackSymbolHolder = { value: undefined };
function intlFallbackSymbol() {
    var fallbackSymbol = intlFallbackSymbolHolder.value;
    if (!fallbackSymbol) {
        fallbackSymbol = std_Symbol("IntlLegacyConstructedSymbol");
        intlFallbackSymbolHolder.value = fallbackSymbol;
    }
    return fallbackSymbol;
}
function initializeIntlObject(obj, type, lazyData) {
    ;;
    ;
                                                ;
    ;;
    var internals = std_Object_create(null);
    internals.type = type;
    internals.lazyData = lazyData;
    internals.internalProps = null;
    ;
                                                ;
    UnsafeSetReservedSlot(obj, 0, internals);
}
function setInternalProperties(internals, internalProps) {
    ;;
    ;;
    internals.internalProps = internalProps;
    internals.lazyData = null;
}
function maybeInternalProperties(internals) {
    ;;
    var lazyData = internals.lazyData;
    if (lazyData)
        return null;
    ;;
    return internals.internalProps;
}
function getIntlObjectInternals(obj) {
    ;;
    ;
                                                                ;
    var internals = UnsafeGetReservedSlot(obj, 0);
    ;;
    ;;
    ;
                                                ;
    ;;
    ;;
    return internals;
}
function getInternals(obj) {
    var internals = getIntlObjectInternals(obj);
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    var type = internals.type;
    if (type === "Collator")
        internalProps = resolveCollatorInternals(internals.lazyData);
    else if (type === "DateTimeFormat")
        internalProps = resolveDateTimeFormatInternals(internals.lazyData);
    else if (type === "NumberFormat")
        internalProps = resolveNumberFormatInternals(internals.lazyData);
    else
        internalProps = resolvePluralRulesInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
var currencyDigits = {
    BHD: 3,
    BIF: 0,
    CLF: 4,
    CLP: 0,
    DJF: 0,
    GNF: 0,
    IQD: 3,
    ISK: 0,
    JOD: 3,
    JPY: 0,
    KMF: 0,
    KRW: 0,
    KWD: 3,
    LYD: 3,
    OMR: 3,
    PYG: 0,
    RWF: 0,
    TND: 3,
    UGX: 0,
    UYI: 0,
    VND: 0,
    VUV: 0,
    XAF: 0,
    XOF: 0,
    XPF: 0,
};
function resolveDateTimeFormatInternals(lazyDateTimeFormatData) {
    ;;
    var internalProps = std_Object_create(null);
    var DateTimeFormat = dateTimeFormatInternalProperties;
    var localeData = DateTimeFormat.localeData;
    var r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                          lazyDateTimeFormatData.requestedLocales,
                          lazyDateTimeFormatData.localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);
    internalProps.locale = r.locale;
    internalProps.calendar = r.ca;
    internalProps.numberingSystem = r.nu;
    var dataLocale = r.dataLocale;
    internalProps.timeZone = lazyDateTimeFormatData.timeZone;
    var formatOpt = lazyDateTimeFormatData.formatOpt;
    if (r.hc !== null && formatOpt.hour12 === undefined)
        formatOpt.hourCycle = r.hc;
    var pattern;
    if (lazyDateTimeFormatData.mozExtensions) {
        if (lazyDateTimeFormatData.patternOption !== undefined) {
            pattern = lazyDateTimeFormatData.patternOption;
            internalProps.patternOption = lazyDateTimeFormatData.patternOption;
        } else if (lazyDateTimeFormatData.dateStyle || lazyDateTimeFormatData.timeStyle) {
            pattern = intl_patternForStyle(dataLocale,
              lazyDateTimeFormatData.dateStyle, lazyDateTimeFormatData.timeStyle,
              lazyDateTimeFormatData.timeZone);
            internalProps.dateStyle = lazyDateTimeFormatData.dateStyle;
            internalProps.timeStyle = lazyDateTimeFormatData.timeStyle;
        } else {
            pattern = toBestICUPattern(dataLocale, formatOpt);
        }
        internalProps.mozExtensions = true;
    } else {
      pattern = toBestICUPattern(dataLocale, formatOpt);
    }
    if (formatOpt.hourCycle !== undefined)
        pattern = replaceHourRepresentation(pattern, formatOpt.hourCycle);
    internalProps.pattern = pattern;
    return internalProps;
}
function replaceHourRepresentation(pattern, hourCycle) {
    var hour;
    switch (hourCycle) {
      case "h11":
        hour = "K";
        break;
      case "h12":
        hour = "h";
        break;
      case "h23":
        hour = "H";
        break;
      case "h24":
        hour = "k";
        break;
    }
    ;;
    var resultPattern = "";
    var inQuote = false;
    for (var i = 0; i < pattern.length; i++) {
        var ch = pattern[i];
        if (ch === "'") {
            inQuote = !inQuote;
        } else if (!inQuote && (ch === "h" || ch === "H" || ch === "k" || ch === "K")) {
            ch = hour;
        }
        resultPattern += ch;
    }
    return resultPattern;
}
function getDateTimeFormatInternals(obj) {
    ;;
    ;;
    var internals = getIntlObjectInternals(obj);
    ;;
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    internalProps = resolveDateTimeFormatInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
function UnwrapDateTimeFormat(dtf, methodName) {
    if (IsObject(dtf) && (GuardToDateTimeFormat(dtf)) === null && dtf instanceof GetDateTimeFormatConstructor())
        dtf = dtf[intlFallbackSymbol()];
    if (!IsObject(dtf) || (dtf = GuardToDateTimeFormat(dtf)) === null) {
        ThrowTypeError(414, "DateTimeFormat", methodName,
                       "DateTimeFormat");
    }
    return dtf;
}
function CanonicalizeTimeZoneName(timeZone) {
    ;;
    ;;
    ;;
    var ianaTimeZone = intl_canonicalizeTimeZone(timeZone);
    ;;
    ;;
    if (ianaTimeZone === "Etc/UTC" || ianaTimeZone === "Etc/GMT") {
        if (timeZone === "Etc/UCT" || timeZone === "UCT")
            ianaTimeZone = "Etc/UCT";
        else
            ianaTimeZone = "UTC";
    }
    return ianaTimeZone;
}
var timeZoneCache = {
    icuDefaultTimeZone: undefined,
    defaultTimeZone: undefined,
};
function DefaultTimeZone() {
    if (intl_isDefaultTimeZone(timeZoneCache.icuDefaultTimeZone))
        return timeZoneCache.defaultTimeZone;
    var icuDefaultTimeZone = intl_defaultTimeZone();
    var timeZone = intl_IsValidTimeZoneName(icuDefaultTimeZone);
    if (timeZone === null) {
        const msPerHour = 60 * 60 * 1000;
        var offset = intl_defaultTimeZoneOffset();
        ;
                                                                               ;
        var offsetHours = offset / msPerHour, offsetHoursFraction = offset % msPerHour;
        if (offsetHoursFraction === 0) {
            timeZone = "Etc/GMT" + (offsetHours < 0 ? "+" : "-") + std_Math_abs(offsetHours);
            timeZone = intl_IsValidTimeZoneName(timeZone);
        }
        if (timeZone === null)
            timeZone = "UTC";
    }
    var defaultTimeZone = CanonicalizeTimeZoneName(timeZone);
    timeZoneCache.defaultTimeZone = defaultTimeZone;
    timeZoneCache.icuDefaultTimeZone = icuDefaultTimeZone;
    return defaultTimeZone;
}
function InitializeDateTimeFormat(dateTimeFormat, thisValue, locales, options, mozExtensions) {
    ;;
    ;
                                                                     ;
    var lazyDateTimeFormatData = std_Object_create(null);
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyDateTimeFormatData.requestedLocales = requestedLocales;
    options = ToDateTimeOptions(options, "any", "date");
    var localeOpt = new Record();
    lazyDateTimeFormatData.localeOpt = localeOpt;
    var localeMatcher =
        GetOption(options, "localeMatcher", "string", ["lookup", "best fit"],
                  "best fit");
    localeOpt.localeMatcher = localeMatcher;
    var hr12 = GetOption(options, "hour12", "boolean", undefined, undefined);
    var hc = GetOption(options, "hourCycle", "string", ["h11", "h12", "h23", "h24"], undefined);
    if (hr12 !== undefined) {
        hc = null;
    }
    localeOpt.hc = hc;
    var tz = options.timeZone;
    if (tz !== undefined) {
        tz = ToString(tz);
        var timeZone = intl_IsValidTimeZoneName(tz);
        if (timeZone === null)
            ThrowRangeError(423, tz);
        tz = CanonicalizeTimeZoneName(timeZone);
    } else {
        tz = DefaultTimeZone();
    }
    lazyDateTimeFormatData.timeZone = tz;
    var formatOpt = new Record();
    lazyDateTimeFormatData.formatOpt = formatOpt;
    lazyDateTimeFormatData.mozExtensions = mozExtensions;
    if (mozExtensions) {
        let pattern = GetOption(options, "pattern", "string", undefined, undefined);
        lazyDateTimeFormatData.patternOption = pattern;
        let dateStyle = GetOption(options, "dateStyle", "string", ["full", "long", "medium", "short"], undefined);
        lazyDateTimeFormatData.dateStyle = dateStyle;
        let timeStyle = GetOption(options, "timeStyle", "string", ["full", "long", "medium", "short"], undefined);
        lazyDateTimeFormatData.timeStyle = timeStyle;
    }
    formatOpt.weekday = GetOption(options, "weekday", "string", ["narrow", "short", "long"],
                                  undefined);
    formatOpt.era = GetOption(options, "era", "string", ["narrow", "short", "long"], undefined);
    formatOpt.year = GetOption(options, "year", "string", ["2-digit", "numeric"], undefined);
    formatOpt.month = GetOption(options, "month", "string",
                                ["2-digit", "numeric", "narrow", "short", "long"], undefined);
    formatOpt.day = GetOption(options, "day", "string", ["2-digit", "numeric"], undefined);
    formatOpt.hour = GetOption(options, "hour", "string", ["2-digit", "numeric"], undefined);
    formatOpt.minute = GetOption(options, "minute", "string", ["2-digit", "numeric"], undefined);
    formatOpt.second = GetOption(options, "second", "string", ["2-digit", "numeric"], undefined);
    formatOpt.timeZoneName = GetOption(options, "timeZoneName", "string", ["short", "long"],
                                       undefined);
    var formatMatcher =
        GetOption(options, "formatMatcher", "string", ["basic", "best fit"],
                  "best fit");
    void formatMatcher;
    if (hr12 !== undefined)
        formatOpt.hour12 = hr12;
    initializeIntlObject(dateTimeFormat, "DateTimeFormat", lazyDateTimeFormatData);
    if (dateTimeFormat !== thisValue && IsObject(thisValue) &&
        thisValue instanceof GetDateTimeFormatConstructor())
    {
        _DefineDataProperty(thisValue, intlFallbackSymbol(), dateTimeFormat,
                            0x08 | 0x10 | 0x20);
        return thisValue;
    }
    return dateTimeFormat;
}
function toBestICUPattern(locale, options) {
    var skeleton = "";
    switch (options.weekday) {
    case "narrow":
        skeleton += "EEEEE";
        break;
    case "short":
        skeleton += "E";
        break;
    case "long":
        skeleton += "EEEE";
    }
    switch (options.era) {
    case "narrow":
        skeleton += "GGGGG";
        break;
    case "short":
        skeleton += "G";
        break;
    case "long":
        skeleton += "GGGG";
        break;
    }
    switch (options.year) {
    case "2-digit":
        skeleton += "yy";
        break;
    case "numeric":
        skeleton += "y";
        break;
    }
    switch (options.month) {
    case "2-digit":
        skeleton += "MM";
        break;
    case "numeric":
        skeleton += "M";
        break;
    case "narrow":
        skeleton += "MMMMM";
        break;
    case "short":
        skeleton += "MMM";
        break;
    case "long":
        skeleton += "MMMM";
        break;
    }
    switch (options.day) {
    case "2-digit":
        skeleton += "dd";
        break;
    case "numeric":
        skeleton += "d";
        break;
    }
    var hourSkeletonChar = "j";
    if (options.hour12 !== undefined) {
        if (options.hour12)
            hourSkeletonChar = "h";
        else
            hourSkeletonChar = "H";
    } else {
        switch (options.hourCycle) {
        case "h11":
        case "h12":
            hourSkeletonChar = "h";
            break;
        case "h23":
        case "h24":
            hourSkeletonChar = "H";
            break;
        }
    }
    switch (options.hour) {
    case "2-digit":
        skeleton += hourSkeletonChar + hourSkeletonChar;
        break;
    case "numeric":
        skeleton += hourSkeletonChar;
        break;
    }
    switch (options.minute) {
    case "2-digit":
        skeleton += "mm";
        break;
    case "numeric":
        skeleton += "m";
        break;
    }
    switch (options.second) {
    case "2-digit":
        skeleton += "ss";
        break;
    case "numeric":
        skeleton += "s";
        break;
    }
    switch (options.timeZoneName) {
    case "short":
        skeleton += "z";
        break;
    case "long":
        skeleton += "zzzz";
        break;
    }
    return intl_patternForSkeleton(locale, skeleton);
}
function ToDateTimeOptions(options, required, defaults) {
    ;;
    ;;
    if (options === undefined)
        options = null;
    else
        options = ToObject(options);
    options = std_Object_create(options);
    var needDefaults = true;
    if ((required === "date" || required === "any") &&
        (options.weekday !== undefined || options.year !== undefined ||
         options.month !== undefined || options.day !== undefined))
    {
        needDefaults = false;
    }
    if ((required === "time" || required === "any") &&
        (options.hour !== undefined || options.minute !== undefined ||
         options.second !== undefined))
    {
        needDefaults = false;
    }
    if (needDefaults && (defaults === "date" || defaults === "all")) {
        _DefineDataProperty(options, "year", "numeric");
        _DefineDataProperty(options, "month", "numeric");
        _DefineDataProperty(options, "day", "numeric");
    }
    if (needDefaults && (defaults === "time" || defaults === "all")) {
        _DefineDataProperty(options, "hour", "numeric");
        _DefineDataProperty(options, "minute", "numeric");
        _DefineDataProperty(options, "second", "numeric");
    }
    return options;
}
function Intl_DateTimeFormat_supportedLocalesOf(locales ) {
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var availableLocales = callFunction(dateTimeFormatInternalProperties.availableLocales,
                                        dateTimeFormatInternalProperties);
    var requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}
var dateTimeFormatInternalProperties = {
    localeData: dateTimeFormatLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        locales = intl_DateTimeFormat_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: ["ca", "nu", "hc"]
};
function dateTimeFormatLocaleData() {
    return {
        ca: intl_availableCalendars,
        nu: getNumberingSystems,
        hc: () => {
            return [null, "h11", "h12", "h23", "h24"];
        },
        default: {
            ca: intl_defaultCalendar,
            nu: intl_numberingSystem,
            hc: () => {
                return null;
            }
        }
    };
}
function dateTimeFormatFormatToBind(date) {
    var dtf = this;
    ;;
    ;;
    var x = (date === undefined) ? std_Date_now() : ToNumber(date);
    return intl_FormatDateTime(dtf, x, false);
}
function Intl_DateTimeFormat_format_get() {
    var dtf = UnwrapDateTimeFormat(this, "format");
    var internals = getDateTimeFormatInternals(dtf);
    if (internals.boundFormat === undefined) {
        var F = callFunction(FunctionBind, dateTimeFormatFormatToBind, dtf);
        internals.boundFormat = F;
    }
    return internals.boundFormat;
}
_SetCanonicalName(Intl_DateTimeFormat_format_get, "get format");
function Intl_DateTimeFormat_formatToParts(date) {
    var dtf = this;
    if (!IsObject(dtf) || (dtf = GuardToDateTimeFormat(dtf)) == null) {
        ThrowTypeError(414, "DateTimeFormat", "formatToParts",
                       "DateTimeFormat");
    }
    getDateTimeFormatInternals(dtf);
    var x = (date === undefined) ? std_Date_now() : ToNumber(date);
    return intl_FormatDateTime(dtf, x, true);
}
function Intl_DateTimeFormat_resolvedOptions() {
    var dtf = UnwrapDateTimeFormat(this, "resolvedOptions");
    var internals = getDateTimeFormatInternals(dtf);
    var result = {
        locale: internals.locale,
        calendar: internals.calendar,
        numberingSystem: internals.numberingSystem,
        timeZone: internals.timeZone,
    };
    if (internals.mozExtensions) {
        if (internals.patternOption !== undefined) {
            result.pattern = internals.pattern;
        } else if (internals.dateStyle || internals.timeStyle) {
            result.dateStyle = internals.dateStyle;
            result.timeStyle = internals.timeStyle;
        }
    }
    resolveICUPattern(internals.pattern, result);
    return result;
}
var icuPatternCharToComponent = {
    E: "weekday",
    G: "era",
    y: "year",
    M: "month",
    L: "month",
    d: "day",
    h: "hour",
    H: "hour",
    k: "hour",
    K: "hour",
    m: "minute",
    s: "second",
    z: "timeZoneName",
    v: "timeZoneName",
    V: "timeZoneName"
};
function resolveICUPattern(pattern, result) {
    ;;
    var i = 0;
    while (i < pattern.length) {
        var c = pattern[i++];
        if (c === "'") {
            while (i < pattern.length && pattern[i] !== "'")
                i++;
            i++;
        } else {
            var count = 1;
            while (i < pattern.length && pattern[i] === c) {
                i++;
                count++;
            }
            var value;
            switch (c) {
            case "G":
            case "E":
            case "z":
            case "v":
            case "V":
                if (count <= 3)
                    value = "short";
                else if (count === 4)
                    value = "long";
                else
                    value = "narrow";
                break;
            case "y":
            case "d":
            case "h":
            case "H":
            case "m":
            case "s":
            case "k":
            case "K":
                if (count === 2)
                    value = "2-digit";
                else
                    value = "numeric";
                break;
            case "M":
            case "L":
                if (count === 1)
                    value = "numeric";
                else if (count === 2)
                    value = "2-digit";
                else if (count === 3)
                    value = "short";
                else if (count === 4)
                    value = "long";
                else
                    value = "narrow";
                break;
            default:
            }
            if (hasOwn(c, icuPatternCharToComponent))
                _DefineDataProperty(result, icuPatternCharToComponent[c], value);
            switch (c) {
            case "h":
                _DefineDataProperty(result, "hourCycle", "h12");
                _DefineDataProperty(result, "hour12", true);
                break;
            case "K":
                _DefineDataProperty(result, "hourCycle", "h11");
                _DefineDataProperty(result, "hour12", true);
                break;
            case "H":
                _DefineDataProperty(result, "hourCycle", "h23");
                _DefineDataProperty(result, "hour12", false);
                break;
            case "k":
                _DefineDataProperty(result, "hourCycle", "h24");
                _DefineDataProperty(result, "hour12", false);
                break;
            }
        }
    }
}
function Intl_getCanonicalLocales(locales) {
    return CanonicalizeLocaleList(locales);
}
function Intl_getCalendarInfo(locales) {
    const requestedLocales = CanonicalizeLocaleList(locales);
    const DateTimeFormat = dateTimeFormatInternalProperties;
    const localeData = DateTimeFormat.localeData;
    const localeOpt = new Record();
    localeOpt.localeMatcher = "best fit";
    const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                            requestedLocales,
                            localeOpt,
                            DateTimeFormat.relevantExtensionKeys,
                            localeData);
    const result = intl_GetCalendarInfo(r.locale);
    _DefineDataProperty(result, "calendar", r.ca);
    _DefineDataProperty(result, "locale", r.locale);
    return result;
}
function Intl_getDisplayNames(locales, options) {
    const requestedLocales = CanonicalizeLocaleList(locales);
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);
    const DateTimeFormat = dateTimeFormatInternalProperties;
    const localeData = DateTimeFormat.localeData;
    const localeOpt = new Record();
    localeOpt.localeMatcher = "best fit";
    const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                          requestedLocales,
                          localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);
    const style = GetOption(options, "style", "string", ["long", "short", "narrow"], "long");
    let keys = options.keys;
    if (keys === undefined) {
        keys = [];
    } else if (!IsObject(keys)) {
        ThrowTypeError(417);
    }
    let processedKeys = [];
    let len = ToLength(keys.length);
    for (let i = 0; i < len; i++) {
        _DefineDataProperty(processedKeys, i, ToString(keys[i]));
    }
    const names = intl_ComputeDisplayNames(r.locale, style, processedKeys);
    const values = {};
    for (let i = 0; i < len; i++) {
        const key = processedKeys[i];
        const name = names[i];
        ;;
        ;;
        _DefineDataProperty(values, key, name);
    }
    const result = { locale: r.locale, style, values };
    return result;
}
function Intl_getLocaleInfo(locales) {
  const requestedLocales = CanonicalizeLocaleList(locales);
  const DateTimeFormat = dateTimeFormatInternalProperties;
  const localeData = DateTimeFormat.localeData;
  const localeOpt = new Record();
  localeOpt.localeMatcher = "best fit";
  const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                          requestedLocales,
                          localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);
  return intl_GetLocaleInfo(r.locale);
}
var langTagMappings = {
    "art-lojban": "jbo",
    "cel-gaulish": "cel-gaulish",
    "en-gb-oed": "en-GB-oxendict",
    "i-ami": "ami",
    "i-bnn": "bnn",
    "i-default": "i-default",
    "i-enochian": "i-enochian",
    "i-hak": "hak",
    "i-klingon": "tlh",
    "i-lux": "lb",
    "i-mingo": "i-mingo",
    "i-navajo": "nv",
    "i-pwn": "pwn",
    "i-tao": "tao",
    "i-tay": "tay",
    "i-tsu": "tsu",
    "ja-latn-hepburn-heploc": "ja-Latn-alalc97",
    "no-bok": "nb",
    "no-nyn": "nn",
    "sgn-be-fr": "sfb",
    "sgn-be-nl": "vgt",
    "sgn-br": "bzs",
    "sgn-ch-de": "sgg",
    "sgn-co": "csn",
    "sgn-de": "gsg",
    "sgn-dk": "dsl",
    "sgn-es": "ssp",
    "sgn-fr": "fsl",
    "sgn-gb": "bfi",
    "sgn-gr": "gss",
    "sgn-ie": "isg",
    "sgn-it": "ise",
    "sgn-jp": "jsl",
    "sgn-mx": "mfs",
    "sgn-ni": "ncs",
    "sgn-nl": "dse",
    "sgn-no": "nsl",
    "sgn-pt": "psr",
    "sgn-se": "swl",
    "sgn-us": "ase",
    "sgn-za": "sfs",
    "zh-cmn": "cmn",
    "zh-cmn-hans": "cmn-Hans",
    "zh-cmn-hant": "cmn-Hant",
    "zh-gan": "gan",
    "zh-guoyu": "cmn",
    "zh-hakka": "hak",
    "zh-min": "zh-min",
    "zh-min-nan": "nan",
    "zh-wuu": "wuu",
    "zh-xiang": "hsn",
    "zh-yue": "yue",
};
var languageMappings = {
    "aam": "aas",
    "adp": "dz",
    "aue": "ktz",
    "ayx": "nun",
    "bgm": "bcg",
    "bjd": "drl",
    "ccq": "rki",
    "cjr": "mom",
    "cka": "cmr",
    "cmk": "xch",
    "coy": "pij",
    "cqu": "quh",
    "drh": "khk",
    "drw": "prs",
    "gav": "dev",
    "gfx": "vaj",
    "ggn": "gvr",
    "gti": "nyc",
    "guv": "duz",
    "hrr": "jal",
    "ibi": "opa",
    "ilw": "gal",
    "in": "id",
    "iw": "he",
    "jeg": "oyb",
    "ji": "yi",
    "jw": "jv",
    "kgc": "tdf",
    "kgh": "kml",
    "koj": "kwv",
    "krm": "bmf",
    "ktr": "dtp",
    "kvs": "gdj",
    "kwq": "yam",
    "kxe": "tvd",
    "kzj": "dtp",
    "kzt": "dtp",
    "lii": "raq",
    "lmm": "rmx",
    "meg": "cir",
    "mo": "ro",
    "mst": "mry",
    "mwj": "vaj",
    "myt": "mry",
    "nad": "xny",
    "nnx": "ngv",
    "nts": "pij",
    "oun": "vaj",
    "pcr": "adx",
    "pmc": "huw",
    "pmu": "phr",
    "ppa": "bfy",
    "ppr": "lcq",
    "pry": "prt",
    "puz": "pub",
    "sca": "hle",
    "skk": "oyb",
    "tdu": "dtp",
    "thc": "tpo",
    "thx": "oyb",
    "tie": "ras",
    "tkk": "twm",
    "tlw": "weo",
    "tmp": "tyj",
    "tne": "kak",
    "tnf": "prs",
    "tsf": "taj",
    "uok": "ema",
    "xba": "cax",
    "xia": "acn",
    "xkh": "waw",
    "xsj": "suj",
    "ybd": "rki",
    "yma": "lrr",
    "ymt": "mtm",
    "yos": "zom",
    "yuu": "yug",
};
var regionMappings = {
    "BU": "MM",
    "DD": "DE",
    "FX": "FR",
    "TP": "TL",
    "YD": "YE",
    "ZR": "CD",
};
var extlangMappings = {
    "aao": "ar",
    "abh": "ar",
    "abv": "ar",
    "acm": "ar",
    "acq": "ar",
    "acw": "ar",
    "acx": "ar",
    "acy": "ar",
    "adf": "ar",
    "ads": "sgn",
    "aeb": "ar",
    "aec": "ar",
    "aed": "sgn",
    "aen": "sgn",
    "afb": "ar",
    "afg": "sgn",
    "ajp": "ar",
    "apc": "ar",
    "apd": "ar",
    "arb": "ar",
    "arq": "ar",
    "ars": "ar",
    "ary": "ar",
    "arz": "ar",
    "ase": "sgn",
    "asf": "sgn",
    "asp": "sgn",
    "asq": "sgn",
    "asw": "sgn",
    "auz": "ar",
    "avl": "ar",
    "ayh": "ar",
    "ayl": "ar",
    "ayn": "ar",
    "ayp": "ar",
    "bbz": "ar",
    "bfi": "sgn",
    "bfk": "sgn",
    "bjn": "ms",
    "bog": "sgn",
    "bqn": "sgn",
    "bqy": "sgn",
    "btj": "ms",
    "bve": "ms",
    "bvl": "sgn",
    "bvu": "ms",
    "bzs": "sgn",
    "cdo": "zh",
    "cds": "sgn",
    "cjy": "zh",
    "cmn": "zh",
    "coa": "ms",
    "cpx": "zh",
    "csc": "sgn",
    "csd": "sgn",
    "cse": "sgn",
    "csf": "sgn",
    "csg": "sgn",
    "csl": "sgn",
    "csn": "sgn",
    "csq": "sgn",
    "csr": "sgn",
    "czh": "zh",
    "czo": "zh",
    "doq": "sgn",
    "dse": "sgn",
    "dsl": "sgn",
    "dup": "ms",
    "ecs": "sgn",
    "esl": "sgn",
    "esn": "sgn",
    "eso": "sgn",
    "eth": "sgn",
    "fcs": "sgn",
    "fse": "sgn",
    "fsl": "sgn",
    "fss": "sgn",
    "gan": "zh",
    "gds": "sgn",
    "gom": "kok",
    "gse": "sgn",
    "gsg": "sgn",
    "gsm": "sgn",
    "gss": "sgn",
    "gus": "sgn",
    "hab": "sgn",
    "haf": "sgn",
    "hak": "zh",
    "hds": "sgn",
    "hji": "ms",
    "hks": "sgn",
    "hos": "sgn",
    "hps": "sgn",
    "hsh": "sgn",
    "hsl": "sgn",
    "hsn": "zh",
    "icl": "sgn",
    "iks": "sgn",
    "ils": "sgn",
    "inl": "sgn",
    "ins": "sgn",
    "ise": "sgn",
    "isg": "sgn",
    "isr": "sgn",
    "jak": "ms",
    "jax": "ms",
    "jcs": "sgn",
    "jhs": "sgn",
    "jls": "sgn",
    "jos": "sgn",
    "jsl": "sgn",
    "jus": "sgn",
    "kgi": "sgn",
    "knn": "kok",
    "kvb": "ms",
    "kvk": "sgn",
    "kvr": "ms",
    "kxd": "ms",
    "lbs": "sgn",
    "lce": "ms",
    "lcf": "ms",
    "liw": "ms",
    "lls": "sgn",
    "lsg": "sgn",
    "lsl": "sgn",
    "lso": "sgn",
    "lsp": "sgn",
    "lst": "sgn",
    "lsy": "sgn",
    "ltg": "lv",
    "lvs": "lv",
    "lzh": "zh",
    "max": "ms",
    "mdl": "sgn",
    "meo": "ms",
    "mfa": "ms",
    "mfb": "ms",
    "mfs": "sgn",
    "min": "ms",
    "mnp": "zh",
    "mqg": "ms",
    "mre": "sgn",
    "msd": "sgn",
    "msi": "ms",
    "msr": "sgn",
    "mui": "ms",
    "mzc": "sgn",
    "mzg": "sgn",
    "mzy": "sgn",
    "nan": "zh",
    "nbs": "sgn",
    "ncs": "sgn",
    "nsi": "sgn",
    "nsl": "sgn",
    "nsp": "sgn",
    "nsr": "sgn",
    "nzs": "sgn",
    "okl": "sgn",
    "orn": "ms",
    "ors": "ms",
    "pel": "ms",
    "pga": "ar",
    "pgz": "sgn",
    "pks": "sgn",
    "prl": "sgn",
    "prz": "sgn",
    "psc": "sgn",
    "psd": "sgn",
    "pse": "ms",
    "psg": "sgn",
    "psl": "sgn",
    "pso": "sgn",
    "psp": "sgn",
    "psr": "sgn",
    "pys": "sgn",
    "rms": "sgn",
    "rsl": "sgn",
    "rsm": "sgn",
    "sdl": "sgn",
    "sfb": "sgn",
    "sfs": "sgn",
    "sgg": "sgn",
    "sgx": "sgn",
    "shu": "ar",
    "slf": "sgn",
    "sls": "sgn",
    "sqk": "sgn",
    "sqs": "sgn",
    "ssh": "ar",
    "ssp": "sgn",
    "ssr": "sgn",
    "svk": "sgn",
    "swc": "sw",
    "swh": "sw",
    "swl": "sgn",
    "syy": "sgn",
    "szs": "sgn",
    "tmw": "ms",
    "tse": "sgn",
    "tsm": "sgn",
    "tsq": "sgn",
    "tss": "sgn",
    "tsy": "sgn",
    "tza": "sgn",
    "ugn": "sgn",
    "ugy": "sgn",
    "ukl": "sgn",
    "uks": "sgn",
    "urk": "ms",
    "uzn": "uz",
    "uzs": "uz",
    "vgt": "sgn",
    "vkk": "ms",
    "vkt": "ms",
    "vsi": "sgn",
    "vsl": "sgn",
    "vsv": "sgn",
    "wbs": "sgn",
    "wuu": "zh",
    "xki": "sgn",
    "xml": "sgn",
    "xmm": "ms",
    "xms": "sgn",
    "ygs": "sgn",
    "yhs": "sgn",
    "ysl": "sgn",
    "yue": "zh",
    "zib": "sgn",
    "zlm": "ms",
    "zmi": "ms",
    "zsl": "sgn",
    "zsm": "ms",
};
var numberFormatInternalProperties = {
    localeData: numberFormatLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        locales = intl_NumberFormat_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: ["nu"]
};
function resolveNumberFormatInternals(lazyNumberFormatData) {
    ;;
    var internalProps = std_Object_create(null);
    var NumberFormat = numberFormatInternalProperties;
    var localeData = NumberFormat.localeData;
    var r = ResolveLocale(callFunction(NumberFormat.availableLocales, NumberFormat),
                          lazyNumberFormatData.requestedLocales,
                          lazyNumberFormatData.opt,
                          NumberFormat.relevantExtensionKeys,
                          localeData);
    internalProps.locale = r.locale;
    internalProps.numberingSystem = r.nu;
    var style = lazyNumberFormatData.style;
    internalProps.style = style;
    if (style === "currency") {
        internalProps.currency = lazyNumberFormatData.currency;
        internalProps.currencyDisplay = lazyNumberFormatData.currencyDisplay;
    }
    internalProps.minimumIntegerDigits = lazyNumberFormatData.minimumIntegerDigits;
    internalProps.minimumFractionDigits = lazyNumberFormatData.minimumFractionDigits;
    internalProps.maximumFractionDigits = lazyNumberFormatData.maximumFractionDigits;
    if ("minimumSignificantDigits" in lazyNumberFormatData) {
        ;;
        internalProps.minimumSignificantDigits = lazyNumberFormatData.minimumSignificantDigits;
        internalProps.maximumSignificantDigits = lazyNumberFormatData.maximumSignificantDigits;
    }
    internalProps.useGrouping = lazyNumberFormatData.useGrouping;
    return internalProps;
}
function getNumberFormatInternals(obj) {
    ;;
    ;;
    var internals = getIntlObjectInternals(obj);
    ;;
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    internalProps = resolveNumberFormatInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
function UnwrapNumberFormat(nf, methodName) {
    if (IsObject(nf) && (GuardToNumberFormat(nf)) === null && nf instanceof GetNumberFormatConstructor())
        nf = nf[intlFallbackSymbol()];
    if (!IsObject(nf) || (nf = GuardToNumberFormat(nf)) === null)
        ThrowTypeError(414, "NumberFormat", methodName, "NumberFormat");
    return nf;
}
function SetNumberFormatDigitOptions(lazyData, options, mnfdDefault, mxfdDefault) {
    ;;
    ;;
    ;;
    ;;
    const mnid = GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1);
    const mnfd = GetNumberOption(options, "minimumFractionDigits", 0, 20, mnfdDefault);
    const mxfdActualDefault = std_Math_max(mnfd, mxfdDefault);
    const mxfd = GetNumberOption(options, "maximumFractionDigits", mnfd, 20, mxfdActualDefault);
    let mnsd = options.minimumSignificantDigits;
    let mxsd = options.maximumSignificantDigits;
    lazyData.minimumIntegerDigits = mnid;
    lazyData.minimumFractionDigits = mnfd;
    lazyData.maximumFractionDigits = mxfd;
    if (mnsd !== undefined || mxsd !== undefined) {
        mnsd = DefaultNumberOption(mnsd, 1, 21, 1);
        mxsd = DefaultNumberOption(mxsd, mnsd, 21, 21);
        lazyData.minimumSignificantDigits = mnsd;
        lazyData.maximumSignificantDigits = mxsd;
    }
}
function toASCIIUpperCase(s) {
    ;;
    var result = "";
    for (var i = 0; i < s.length; i++) {
        var c = callFunction(std_String_charCodeAt, s, i);
        result += (0x61 <= c && c <= 0x7A)
                  ? callFunction(std_String_fromCharCode, null, c & ~0x20)
                  : s[i];
    }
    return result;
}
function IsWellFormedCurrencyCode(currency) {
    ;;
    return currency.length === 3 && IsASCIIAlphaString(currency);
}
function InitializeNumberFormat(numberFormat, thisValue, locales, options) {
    ;;
    ;;
    var lazyNumberFormatData = std_Object_create(null);
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyNumberFormatData.requestedLocales = requestedLocales;
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);
    var opt = new Record();
    lazyNumberFormatData.opt = opt;
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;
    var style = GetOption(options, "style", "string", ["decimal", "percent", "currency"], "decimal");
    lazyNumberFormatData.style = style;
    var c = GetOption(options, "currency", "string", undefined, undefined);
    if (c !== undefined && !IsWellFormedCurrencyCode(c))
        ThrowRangeError(415, c);
    var cDigits;
    if (style === "currency") {
        if (c === undefined)
            ThrowTypeError(424);
        c = toASCIIUpperCase(c);
        lazyNumberFormatData.currency = c;
        cDigits = CurrencyDigits(c);
    }
    var cd = GetOption(options, "currencyDisplay", "string", ["code", "symbol", "name"], "symbol");
    if (style === "currency")
        lazyNumberFormatData.currencyDisplay = cd;
    var mnfdDefault, mxfdDefault;
    if (style === "currency") {
        mnfdDefault = cDigits;
        mxfdDefault = cDigits;
    } else {
        mnfdDefault = 0;
        mxfdDefault = style === "percent" ? 0 : 3;
    }
    SetNumberFormatDigitOptions(lazyNumberFormatData, options, mnfdDefault, mxfdDefault);
    var g = GetOption(options, "useGrouping", "boolean", undefined, true);
    lazyNumberFormatData.useGrouping = g;
    initializeIntlObject(numberFormat, "NumberFormat", lazyNumberFormatData);
    if (numberFormat !== thisValue && IsObject(thisValue) &&
        thisValue instanceof GetNumberFormatConstructor())
    {
        _DefineDataProperty(thisValue, intlFallbackSymbol(), numberFormat,
                            0x08 | 0x10 | 0x20);
        return thisValue;
    }
    return numberFormat;
}
function CurrencyDigits(currency) {
    ;;
    ;;
    ;;
    if (hasOwn(currency, currencyDigits))
        return currencyDigits[currency];
    return 2;
}
function Intl_NumberFormat_supportedLocalesOf(locales ) {
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var availableLocales = callFunction(numberFormatInternalProperties.availableLocales,
                                        numberFormatInternalProperties);
    var requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}
function getNumberingSystems(locale) {
    var defaultNumberingSystem = intl_numberingSystem(locale);
    return [
        defaultNumberingSystem,
        "arab", "arabext", "bali", "beng", "deva",
        "fullwide", "gujr", "guru", "hanidec", "khmr",
        "knda", "laoo", "latn", "limb", "mlym",
        "mong", "mymr", "orya", "tamldec", "telu",
        "thai", "tibt"
    ];
}
function numberFormatLocaleData() {
    return {
        nu: getNumberingSystems,
        default: {
            nu: intl_numberingSystem,
        }
    };
}
function numberFormatFormatToBind(value) {
    var nf = this;
    ;;
    ;;
    var x = ToNumber(value);
    return intl_FormatNumber(nf, x, false);
}
function Intl_NumberFormat_format_get() {
    var nf = UnwrapNumberFormat(this, "format");
    var internals = getNumberFormatInternals(nf);
    if (internals.boundFormat === undefined) {
        var F = callFunction(FunctionBind, numberFormatFormatToBind, nf);
        internals.boundFormat = F;
    }
    return internals.boundFormat;
}
_SetCanonicalName(Intl_NumberFormat_format_get, "get format");
function Intl_NumberFormat_formatToParts(value) {
    var nf = this;
    if (!IsObject(nf) || (nf = GuardToNumberFormat(nf)) === null) {
        ThrowTypeError(414, "NumberFormat", "formatToParts",
                       "NumberFormat");
    }
    getNumberFormatInternals(nf);
    var x = ToNumber(value);
    return intl_FormatNumber(nf, x, true);
}
function Intl_NumberFormat_resolvedOptions() {
    var nf = UnwrapNumberFormat(this, "resolvedOptions");
    var internals = getNumberFormatInternals(nf);
    var result = {
        locale: internals.locale,
        numberingSystem: internals.numberingSystem,
        style: internals.style,
        minimumIntegerDigits: internals.minimumIntegerDigits,
        minimumFractionDigits: internals.minimumFractionDigits,
        maximumFractionDigits: internals.maximumFractionDigits,
        useGrouping: internals.useGrouping
    };
    ;
                                                         ;
    ;
                                                                ;
    if (hasOwn("currency", internals)) {
        _DefineDataProperty(result, "currency", internals.currency);
        _DefineDataProperty(result, "currencyDisplay", internals.currencyDisplay);
    }
    ;
                                                                                         ;
    if (hasOwn("minimumSignificantDigits", internals)) {
        _DefineDataProperty(result, "minimumSignificantDigits",
                            internals.minimumSignificantDigits);
        _DefineDataProperty(result, "maximumSignificantDigits",
                            internals.maximumSignificantDigits);
    }
    return result;
}
var pluralRulesInternalProperties = {
    localeData: pluralRulesLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        locales = intl_PluralRules_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: [],
};
function pluralRulesLocaleData() {
    return {};
}
function resolvePluralRulesInternals(lazyPluralRulesData) {
    ;;
    var internalProps = std_Object_create(null);
    var PluralRules = pluralRulesInternalProperties;
    var localeData = PluralRules.localeData;
    const r = ResolveLocale(callFunction(PluralRules.availableLocales, PluralRules),
                            lazyPluralRulesData.requestedLocales,
                            lazyPluralRulesData.opt,
                            PluralRules.relevantExtensionKeys,
                            localeData);
    internalProps.locale = r.locale;
    internalProps.type = lazyPluralRulesData.type;
    internalProps.minimumIntegerDigits = lazyPluralRulesData.minimumIntegerDigits;
    internalProps.minimumFractionDigits = lazyPluralRulesData.minimumFractionDigits;
    internalProps.maximumFractionDigits = lazyPluralRulesData.maximumFractionDigits;
    if ("minimumSignificantDigits" in lazyPluralRulesData) {
        ;;
        internalProps.minimumSignificantDigits = lazyPluralRulesData.minimumSignificantDigits;
        internalProps.maximumSignificantDigits = lazyPluralRulesData.maximumSignificantDigits;
    }
    internalProps.pluralCategories = null;
    return internalProps;
}
function getPluralRulesInternals(obj) {
    ;;
    ;;
    var internals = getIntlObjectInternals(obj);
    ;;
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    internalProps = resolvePluralRulesInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
function InitializePluralRules(pluralRules, locales, options) {
    ;;
    ;;
    const lazyPluralRulesData = std_Object_create(null);
    let requestedLocales = CanonicalizeLocaleList(locales);
    lazyPluralRulesData.requestedLocales = requestedLocales;
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);
    let opt = new Record();
    lazyPluralRulesData.opt = opt;
    let matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;
    const type = GetOption(options, "type", "string", ["cardinal", "ordinal"], "cardinal");
    lazyPluralRulesData.type = type;
    SetNumberFormatDigitOptions(lazyPluralRulesData, options, 0, 3);
    initializeIntlObject(pluralRules, "PluralRules", lazyPluralRulesData);
}
function Intl_PluralRules_supportedLocalesOf(locales ) {
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var availableLocales = callFunction(pluralRulesInternalProperties.availableLocales,
                                        pluralRulesInternalProperties);
    let requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}
function Intl_PluralRules_select(value) {
    let pluralRules = this;
    if (!IsObject(pluralRules) || (pluralRules = GuardToPluralRules(pluralRules)) === null)
        ThrowTypeError(414, "PluralRules", "select", "PluralRules");
    getPluralRulesInternals(pluralRules);
    let n = ToNumber(value);
    return intl_SelectPluralRule(pluralRules, n);
}
function Intl_PluralRules_resolvedOptions() {
    var pluralRules = this;
    if (!IsObject(pluralRules) || (pluralRules = GuardToPluralRules(pluralRules)) === null) {
        ThrowTypeError(414, "PluralRules", "resolvedOptions",
                       "PluralRules");
    }
    var internals = getPluralRulesInternals(pluralRules);
    var internalsPluralCategories = internals.pluralCategories;
    if (internalsPluralCategories === null) {
        internalsPluralCategories = intl_GetPluralCategories(pluralRules);
        internals.pluralCategories = internalsPluralCategories;
    }
    var pluralCategories = [];
    for (var i = 0; i < internalsPluralCategories.length; i++)
        _DefineDataProperty(pluralCategories, i, internalsPluralCategories[i]);
    var result = {
        locale: internals.locale,
        type: internals.type,
        pluralCategories,
        minimumIntegerDigits: internals.minimumIntegerDigits,
        minimumFractionDigits: internals.minimumFractionDigits,
        maximumFractionDigits: internals.maximumFractionDigits,
    };
    ;
                                                                                         ;
    if (hasOwn("minimumSignificantDigits", internals)) {
        _DefineDataProperty(result, "minimumSignificantDigits",
                            internals.minimumSignificantDigits);
        _DefineDataProperty(result, "maximumSignificantDigits",
                            internals.maximumSignificantDigits);
    }
    return result;
}
var relativeTimeFormatInternalProperties = {
    localeData: relativeTimeFormatLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        locales = intl_RelativeTimeFormat_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: [],
};
function relativeTimeFormatLocaleData() {
    return {};
}
function resolveRelativeTimeFormatInternals(lazyRelativeTimeFormatData) {
    ;;
    var internalProps = std_Object_create(null);
    var RelativeTimeFormat = relativeTimeFormatInternalProperties;
    const r = ResolveLocale(callFunction(RelativeTimeFormat.availableLocales, RelativeTimeFormat),
                            lazyRelativeTimeFormatData.requestedLocales,
                            lazyRelativeTimeFormatData.opt,
                            RelativeTimeFormat.relevantExtensionKeys,
                            RelativeTimeFormat.localeData);
    internalProps.locale = r.locale;
    internalProps.style = lazyRelativeTimeFormatData.style;
    internalProps.numeric = lazyRelativeTimeFormatData.numeric;
    return internalProps;
}
function getRelativeTimeFormatInternals(obj, methodName) {
    ;;
    ;;
    var internals = getIntlObjectInternals(obj);
    ;;
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;
    internalProps = resolveRelativeTimeFormatInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}
function InitializeRelativeTimeFormat(relativeTimeFormat, locales, options) {
    ;
                                                                ;
    ;
                                                                             ;
    const lazyRelativeTimeFormatData = std_Object_create(null);
    let requestedLocales = CanonicalizeLocaleList(locales);
    lazyRelativeTimeFormatData.requestedLocales = requestedLocales;
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);
    let opt = new Record();
    let matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;
    lazyRelativeTimeFormatData.opt = opt;
    const style = GetOption(options, "style", "string", ["long", "short", "narrow"], "long");
    lazyRelativeTimeFormatData.style = style;
    const numeric = GetOption(options, "numeric", "string", ["always", "auto"], "always");
    lazyRelativeTimeFormatData.numeric = numeric;
    initializeIntlObject(relativeTimeFormat, "RelativeTimeFormat", lazyRelativeTimeFormatData);
}
function Intl_RelativeTimeFormat_supportedLocalesOf(locales ) {
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var availableLocales = callFunction(relativeTimeFormatInternalProperties.availableLocales,
                                        relativeTimeFormatInternalProperties);
    let requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}
function Intl_RelativeTimeFormat_format(value, unit) {
    let relativeTimeFormat = this;
    if (!IsObject(relativeTimeFormat) || (relativeTimeFormat = GuardToRelativeTimeFormat(relativeTimeFormat)) === null)
        ThrowTypeError(414, "RelativeTimeFormat", "format", "RelativeTimeFormat");
    var internals = getRelativeTimeFormatInternals(relativeTimeFormat);
    let t = ToNumber(value);
    let u = ToString(unit);
    switch (u) {
      case "second":
      case "minute":
      case "hour":
      case "day":
      case "week":
      case "month":
      case "quarter":
      case "year":
        break;
      default:
        ThrowRangeError(422, "unit", u);
    }
    return intl_FormatRelativeTime(relativeTimeFormat, t, u, internals.numeric);
}
function Intl_RelativeTimeFormat_resolvedOptions() {
    var relativeTimeFormat;
    if (!IsObject(this) || (relativeTimeFormat = GuardToRelativeTimeFormat(this)) === null) {
        ThrowTypeError(414, "RelativeTimeFormat", "resolvedOptions",
                       "RelativeTimeFormat");
    }
    var internals = getRelativeTimeFormatInternals(relativeTimeFormat, "resolvedOptions");
    var result = {
        locale: internals.locale,
        style: internals.style,
        numeric: internals.numeric,
    };
    return result;
}
function IteratorIdentity() {
    return this;
}
function MapConstructorInit(iterable) {
    var map = this;
    var adder = map.set;
    if (!IsCallable(adder))
        ThrowTypeError(9, typeof adder);
    for (var nextItem of allowContentIter(iterable)) {
        if (!IsObject(nextItem))
            ThrowTypeError(30, "Map");
        callContentFunction(adder, map, nextItem[0], nextItem[1]);
    }
}
function MapForEach(callbackfn, thisArg = undefined) {
    var M = this;
    if (!IsObject(M) || (M = GuardToMapObject(M)) === null)
        return callFunction(CallMapMethodIfWrapped, this, callbackfn, thisArg, "MapForEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var entries = callFunction(std_Map_iterator, M);
    var mapIterationResultPair = iteratorTemp.mapIterationResultPair;
    if (!mapIterationResultPair) {
        mapIterationResultPair = iteratorTemp.mapIterationResultPair =
            _CreateMapIterationResultPair();
    }
    while (true) {
        var done = _GetNextMapEntryForIterator(entries, mapIterationResultPair);
        if (done)
            break;
        var key = mapIterationResultPair[0];
        var value = mapIterationResultPair[1];
        mapIterationResultPair[0] = null;
        mapIterationResultPair[1] = null;
        callContentFunction(callbackfn, thisArg, value, key, M);
    }
}
function MapEntries() {
    return callFunction(std_Map_iterator, this);
}
_SetCanonicalName(MapEntries, "entries");
var iteratorTemp = { mapIterationResultPair: null };
function MapIteratorNext() {
    var O = this;
    if (!IsObject(O) || (O = GuardToMapIterator(O)) === null)
        return callFunction(CallMapIteratorMethodIfWrapped, this, "MapIteratorNext");
    var mapIterationResultPair = iteratorTemp.mapIterationResultPair;
    if (!mapIterationResultPair) {
        mapIterationResultPair = iteratorTemp.mapIterationResultPair =
            _CreateMapIterationResultPair();
    }
    var retVal = {value: undefined, done: true};
    var done = _GetNextMapEntryForIterator(O, mapIterationResultPair);
    if (!done) {
        var itemKind = UnsafeGetInt32FromReservedSlot(O, 2);
        var result;
        if (itemKind === 0) {
            result = mapIterationResultPair[0];
        } else if (itemKind === 1) {
            result = mapIterationResultPair[1];
        } else {
            ;;
            result = [mapIterationResultPair[0], mapIterationResultPair[1]];
        }
        mapIterationResultPair[0] = null;
        mapIterationResultPair[1] = null;
        retVal.value = result;
        retVal.done = false;
    }
    return retVal;
}
function MapSpecies() {
    return this;
}
_SetCanonicalName(MapSpecies, "get [Symbol.species]");
function CallModuleResolveHook(module, specifier, expectedMinimumStatus)
{
    let requestedModule = HostResolveImportedModule(module, specifier);
    if (requestedModule.status < expectedMinimumStatus)
        ThrowInternalError(492);
    return requestedModule;
}
function ModuleGetExportedNames(exportStarSet = [])
{
    if (!IsObject(this) || !IsModule(this)) {
        return callFunction(CallModuleMethodIfWrapped, this, exportStarSet,
                            "ModuleGetExportedNames");
    }
    let module = this;
    if (callFunction(ArrayIncludes, exportStarSet, module))
        return [];
    _DefineDataProperty(exportStarSet, exportStarSet.length, module);
    let exportedNames = [];
    let namesCount = 0;
    let localExportEntries = module.localExportEntries;
    for (let i = 0; i < localExportEntries.length; i++) {
        let e = localExportEntries[i];
        _DefineDataProperty(exportedNames, namesCount++, e.exportName);
    }
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        _DefineDataProperty(exportedNames, namesCount++, e.exportName);
    }
    let starExportEntries = module.starExportEntries;
    for (let i = 0; i < starExportEntries.length; i++) {
        let e = starExportEntries[i];
        let requestedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                    1);
        let starNames = callFunction(requestedModule.getExportedNames, requestedModule,
                                     exportStarSet);
        for (let j = 0; j < starNames.length; j++) {
            let n = starNames[j];
            if (n !== "default" && !callFunction(ArrayIncludes, exportedNames, n))
                _DefineDataProperty(exportedNames, namesCount++, n);
        }
    }
    return exportedNames;
}
function ModuleSetStatus(module, newStatus)
{
    ;
                                                      ;
    ;
                                                                ;
    UnsafeSetReservedSlot(module, 3, newStatus);
}
function ModuleResolveExport(exportName, resolveSet = [])
{
    if (!IsObject(this) || !IsModule(this)) {
        return callFunction(CallModuleMethodIfWrapped, this, exportName, resolveSet,
                            "ModuleResolveExport");
    }
    let module = this;
    for (let i = 0; i < resolveSet.length; i++) {
        let r = resolveSet[i];
        if (r.module === module && r.exportName === exportName) {
            return null;
        }
    }
    _DefineDataProperty(resolveSet, resolveSet.length, {module, exportName});
    let localExportEntries = module.localExportEntries;
    for (let i = 0; i < localExportEntries.length; i++) {
        let e = localExportEntries[i];
        if (exportName === e.exportName)
            return {module, bindingName: e.localName};
    }
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        if (exportName === e.exportName) {
            let importedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                       0);
            return callFunction(importedModule.resolveExport, importedModule, e.importName,
                                resolveSet);
        }
    }
    if (exportName === "default") {
        return null;
    }
    let starResolution = null;
    let starExportEntries = module.starExportEntries;
    for (let i = 0; i < starExportEntries.length; i++) {
        let e = starExportEntries[i];
        let importedModule = CallModuleResolveHook(module, e.moduleRequest,
                                                   0);
        let resolution = callFunction(importedModule.resolveExport, importedModule, exportName,
                                      resolveSet);
        if (resolution === "ambiguous")
            return resolution;
        if (resolution !== null) {
            if (starResolution === null) {
                starResolution = resolution;
            } else {
                if (resolution.module !== starResolution.module ||
                    resolution.bindingName !== starResolution.bindingName)
                {
                    return "ambiguous";
                }
            }
        }
    }
    return starResolution;
}
function IsResolvedBinding(resolution)
{
    ;
                                          ;
    return typeof resolution === "object" && resolution !== null;
}
function GetModuleNamespace(module)
{
    ;;
    ;
                                                    ;
    let namespace = module.namespace;
    if (typeof namespace === "undefined") {
        let exportedNames = callFunction(module.getExportedNames, module);
        let unambiguousNames = [];
        for (let i = 0; i < exportedNames.length; i++) {
            let name = exportedNames[i];
            let resolution = callFunction(module.resolveExport, module, name);
            if (IsResolvedBinding(resolution))
                _DefineDataProperty(unambiguousNames, unambiguousNames.length, name);
        }
        namespace = ModuleNamespaceCreate(module, unambiguousNames);
    }
    return namespace;
}
function ModuleNamespaceCreate(module, exports)
{
    callFunction(ArraySort, exports);
    let ns = NewModuleNamespace(module, exports);
    for (let i = 0; i < exports.length; i++) {
        let name = exports[i];
        let binding = callFunction(module.resolveExport, module, name);
        ;;
        AddModuleNamespaceBinding(ns, name, binding.module, binding.bindingName);
    }
    return ns;
}
function GetModuleEnvironment(module)
{
    ;;
    ;
                                                                       ;
    let env = UnsafeGetReservedSlot(module, 1);
    ;
                                                               ;
    return env;
}
function CountArrayValues(array, value)
{
    let count = 0;
    for (let i = 0; i < array.length; i++) {
        if (array[i] === value)
            count++;
    }
    return count;
}
function ArrayContains(array, value)
{
    for (let i = 0; i < array.length; i++) {
        if (array[i] === value)
            return true;
    }
    return false;
}
function HandleModuleInstantiationFailure(module)
{
    ModuleSetStatus(module, 0);
    UnsafeSetReservedSlot(module, 13, undefined);
    UnsafeSetReservedSlot(module, 14, undefined);
}
function ModuleInstantiate()
{
    if (!IsObject(this) || !IsModule(this))
        return callFunction(CallModuleMethodIfWrapped, this, "ModuleInstantiate");
    let module = this;
    if (module.status === 1 ||
        module.status === 3)
    {
        ThrowInternalError(492);
    }
    let stack = [];
    try {
        InnerModuleInstantiation(module, stack, 0);
    } catch (error) {
        for (let i = 0; i < stack.length; i++) {
            let m = stack[i];
            ;
                                                                               ;
            HandleModuleInstantiationFailure(m);
        }
        if (stack.length === 0)
            HandleModuleInstantiationFailure(module);
        ;
                                                                           ;
        throw error;
    }
    ;
                                                              ;
    ;
                                                                  ;
    return undefined;
}
_SetCanonicalName(ModuleInstantiate, "ModuleInstantiate");
function InnerModuleInstantiation(module, stack, index)
{
    if (module.status === 1 ||
        module.status === 2 ||
        module.status === 4 ||
        module.status === 5)
    {
        return index;
    }
    ;
                                                                ;
    ModuleSetStatus(module, 1);
    UnsafeSetReservedSlot(module, 13, index);
    UnsafeSetReservedSlot(module, 14, index);
    index++;
    _DefineDataProperty(stack, stack.length, module);
    let requestedModules = module.requestedModules;
    for (let i = 0; i < requestedModules.length; i++) {
        let required = requestedModules[i].moduleSpecifier;
        let requiredModule = CallModuleResolveHook(module, required, 0);
        index = InnerModuleInstantiation(requiredModule, stack, index);
        ;
                                                                           ;
        ;
                                                                                              ;
        ;;
        ;;
        if (requiredModule.status === 1) {
            UnsafeSetReservedSlot(module, 14,
                                  std_Math_min(module.dfsAncestorIndex,
                                               requiredModule.dfsAncestorIndex));
        }
    }
    ModuleDeclarationEnvironmentSetup(module);
    ;
                                                                    ;
    ;
                                    ;
    if (module.dfsAncestorIndex === module.dfsIndex) {
        let requiredModule;
        do {
            requiredModule = callFunction(std_Array_pop, stack);
            ModuleSetStatus(requiredModule, 2);
        } while (requiredModule !== module);
    }
    return index;
}
function ModuleDeclarationEnvironmentSetup(module)
{
    let indirectExportEntries = module.indirectExportEntries;
    for (let i = 0; i < indirectExportEntries.length; i++) {
        let e = indirectExportEntries[i];
        let resolution = callFunction(module.resolveExport, module, e.exportName);
        if (!IsResolvedBinding(resolution)) {
            ThrowResolutionError(module, resolution, "indirectExport", e.exportName,
                                 e.lineNumber, e.columnNumber);
        }
    }
    let env = GetModuleEnvironment(module);
    let importEntries = module.importEntries;
    for (let i = 0; i < importEntries.length; i++) {
        let imp = importEntries[i];
        let importedModule = CallModuleResolveHook(module, imp.moduleRequest,
                                                   1);
        if (imp.importName === "*") {
            let namespace = GetModuleNamespace(importedModule);
            CreateNamespaceBinding(env, imp.localName, namespace);
        } else {
            let resolution = callFunction(importedModule.resolveExport, importedModule,
                                          imp.importName);
            if (!IsResolvedBinding(resolution)) {
                ThrowResolutionError(module, resolution, "import", imp.importName,
                                     imp.lineNumber, imp.columnNumber);
            }
            CreateImportBinding(env, imp.localName, resolution.module, resolution.bindingName);
        }
    }
    InstantiateModuleFunctionDeclarations(module);
}
function ThrowResolutionError(module, resolution, kind, name, line, column)
{
    ;
                                                              ;
    ;
                                                     ;
    ;
                                                                                ;
    let ambiguous = resolution === "ambiguous";
    let errorNumber;
    if (kind === "import")
        errorNumber = ambiguous ? 489 : 488;
    else
        errorNumber = ambiguous ? 487 : 486;
    let message = GetErrorMessage(errorNumber) + ": " + name;
    let error = CreateModuleSyntaxError(module, line, column, message);
    throw error;
}
function GetModuleEvaluationError(module)
{
    ;
                                                           ;
    ;
                                                           ;
    return UnsafeGetReservedSlot(module, 4);
}
function RecordModuleEvaluationError(module, error)
{
    ;
                                                              ;
    if (module.status === 5) {
        return;
    }
    ModuleSetStatus(module, 5);
    UnsafeSetReservedSlot(module, 4, error);
}
function ModuleEvaluate()
{
    if (!IsObject(this) || !IsModule(this))
        return callFunction(CallModuleMethodIfWrapped, this, "ModuleEvaluate");
    let module = this;
    if (module.status !== 2 &&
        module.status !== 4 &&
        module.status !== 5)
    {
        ThrowInternalError(492);
    }
    let stack = [];
    try {
        InnerModuleEvaluation(module, stack, 0);
    } catch (error) {
        for (let i = 0; i < stack.length; i++) {
            let m = stack[i];
            ;
                                                               ;
            RecordModuleEvaluationError(m, error);
        }
        if (stack.length === 0)
            RecordModuleEvaluationError(module, error);
        ;
                                                           ;
        throw error;
    }
    ;
                                                           ;
    ;
                                                               ;
    return undefined;
}
_SetCanonicalName(ModuleEvaluate, "ModuleEvaluate");
function InnerModuleEvaluation(module, stack, index)
{
    if (module.status === 5)
        throw GetModuleEvaluationError(module);
    if (module.status === 4)
        return index;
    if (module.status === 3)
        return index;
    ;
                                                       ;
    ModuleSetStatus(module, 3);
    UnsafeSetReservedSlot(module, 13, index);
    UnsafeSetReservedSlot(module, 14, index);
    index++;
    _DefineDataProperty(stack, stack.length, module);
    let requestedModules = module.requestedModules;
    for (let i = 0; i < requestedModules.length; i++) {
        let required = requestedModules[i].moduleSpecifier;
        let requiredModule =
            CallModuleResolveHook(module, required, 2);
        index = InnerModuleEvaluation(requiredModule, stack, index);
        ;
                                                              ;
        ;
                                                                                            ;
        ;;
        ;;
        if (requiredModule.status === 3) {
            UnsafeSetReservedSlot(module, 14,
                                  std_Math_min(module.dfsAncestorIndex,
                                               requiredModule.dfsAncestorIndex));
        }
    }
    ExecuteModule(module);
    ;
                                                                    ;
    ;
                                    ;
    if (module.dfsAncestorIndex === module.dfsIndex) {
        let requiredModule;
        do {
            requiredModule = callFunction(std_Array_pop, stack);
            ModuleSetStatus(requiredModule, 4);
        } while (requiredModule !== module);
    }
    return index;
}
var numberFormatCache = new Record();
function Number_toLocaleString() {
    var x = callFunction(std_Number_valueOf, this);
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;
    var numberFormat;
    if (locales === undefined && options === undefined) {
        if (!IsRuntimeDefaultLocale(numberFormatCache.runtimeDefaultLocale)) {
            numberFormatCache.numberFormat = intl_NumberFormat(locales, options);
            numberFormatCache.runtimeDefaultLocale = RuntimeDefaultLocale();
        }
        numberFormat = numberFormatCache.numberFormat;
    } else {
        numberFormat = intl_NumberFormat(locales, options);
    }
    return intl_FormatNumber(numberFormat, x, false);
}
function Number_isFinite(num) {
    if (typeof num !== "number")
        return false;
    return num - num === 0;
}
function Number_isNaN(num) {
    if (typeof num !== "number")
        return false;
    return num !== num;
}
function Number_isInteger(number) {
    if (typeof number !== "number")
        return false;
    if (number === -(2 ** 31))
        return true;
    var absNumber = std_Math_abs(number);
    var integer = std_Math_floor(absNumber);
    if (absNumber - integer !== 0)
        return false;
    return true;
}
function Number_isSafeInteger(number) {
    if (typeof number !== "number")
        return false;
    if (number === -(2 ** 31))
        return true;
    var absNumber = std_Math_abs(number);
    var integer = std_Math_floor(absNumber);
    if (absNumber - integer !== 0)
        return false;
    if (integer <= (2 ** 53) - 1)
        return true;
    return false;
}
function Global_isNaN(number) {
    return Number_isNaN(ToNumber(number));
}
function Global_isFinite(number) {
    return Number_isFinite(ToNumber(number));
}
function ObjectGetOwnPropertyDescriptors(O) {
    var obj = ToObject(O);
    var keys = OwnPropertyKeys(obj);
    var descriptors = {};
    for (var index = 0, len = keys.length; index < len; index++) {
        var key = keys[index];
        var desc = ObjectGetOwnPropertyDescriptor(obj, key);
        if (typeof desc !== "undefined")
            _DefineDataProperty(descriptors, key, desc);
    }
    return descriptors;
}
function ObjectGetPrototypeOf(obj) {
    return std_Reflect_getPrototypeOf(ToObject(obj));
}
function ObjectIsExtensible(obj) {
    return IsObject(obj) && std_Reflect_isExtensible(obj);
}
function Object_toLocaleString() {
    var O = this;
    return callContentFunction(O.toString, O);
}
function Object_valueOf() {
    return ToObject(this);
}
function Object_hasOwnProperty(V) {
    return hasOwn(V, this);
}
function ObjectDefineSetter(name, setter) {
    var object = ToObject(this);
    if (!IsCallable(setter))
        ThrowTypeError(22, "setter");
    var key = (typeof name !== "string" && typeof name !== "number" && typeof name !== "symbol" ? ToPropertyKey(name) : name);
    _DefineProperty(object, key, 0x200 | 0x01 | 0x02,
                    null, setter, true);
}
function ObjectDefineGetter(name, getter) {
    var object = ToObject(this);
    if (!IsCallable(getter))
        ThrowTypeError(22, "getter");
    var key = (typeof name !== "string" && typeof name !== "number" && typeof name !== "symbol" ? ToPropertyKey(name) : name);
    _DefineProperty(object, key, 0x200 | 0x01 | 0x02,
                    getter, null, true);
}
function ObjectLookupSetter(name) {
    var object = ToObject(this);
    var key = (typeof name !== "string" && typeof name !== "number" && typeof name !== "symbol" ? ToPropertyKey(name) : name);
    do {
        var desc = GetOwnPropertyDescriptorToArray(object, key);
        if (desc) {
            if (desc[0] & 0x200)
                return desc[2];
            return undefined;
        }
        object = std_Reflect_getPrototypeOf(object);
    } while (object !== null);
}
function ObjectLookupGetter(name) {
    var object = ToObject(this);
    var key = (typeof name !== "string" && typeof name !== "number" && typeof name !== "symbol" ? ToPropertyKey(name) : name);
    do {
        var desc = GetOwnPropertyDescriptorToArray(object, key);
        if (desc) {
            if (desc[0] & 0x200)
                return desc[1];
            return undefined;
        }
        object = std_Reflect_getPrototypeOf(object);
    } while (object !== null);
}
function ObjectGetOwnPropertyDescriptor(obj, propertyKey) {
    var desc = GetOwnPropertyDescriptorToArray(obj, propertyKey);
    if (!desc)
        return undefined;
    var attrsAndKind = desc[0];
    if (attrsAndKind & 0x100) {
        return {
            value: desc[1],
            writable: !!(attrsAndKind & 0x04),
            enumerable: !!(attrsAndKind & 0x01),
            configurable: !!(attrsAndKind & 0x02),
        };
    }
    ;;
    return {
        get: desc[1],
        set: desc[2],
        enumerable: !!(attrsAndKind & 0x01),
        configurable: !!(attrsAndKind & 0x02),
    };
}
function ObjectOrReflectDefineProperty(obj, propertyKey, attributes, strict) {
    if (!IsObject(obj))
        ThrowTypeError(40, DecompileArg(0, obj));
    propertyKey = (typeof propertyKey !== "string" && typeof propertyKey !== "number" && typeof propertyKey !== "symbol" ? ToPropertyKey(propertyKey) : propertyKey);
    if (!IsObject(attributes))
        ThrowArgTypeNotObject(0, attributes);
    var attrs = 0, hasValue = false;
    var value, getter = null, setter = null;
    if ("enumerable" in attributes)
        attrs |= attributes.enumerable ? 0x01 : 0x08;
    if ("configurable" in attributes)
        attrs |= attributes.configurable ? 0x02 : 0x10;
    if ("value" in attributes) {
        attrs |= 0x100;
        value = attributes.value;
        hasValue = true;
    }
    if ("writable" in attributes) {
        attrs |= 0x100;
        attrs |= attributes.writable ? 0x04 : 0x20;
    }
    if ("get" in attributes) {
        attrs |= 0x200;
        getter = attributes.get;
        if (!IsCallable(getter) && getter !== undefined)
            ThrowTypeError(50, "get");
    }
    if ("set" in attributes) {
        attrs |= 0x200;
        setter = attributes.set;
        if (!IsCallable(setter) && setter !== undefined)
            ThrowTypeError(50, "set");
    }
    if (attrs & 0x200) {
        if (attrs & 0x100)
            ThrowTypeError(44);
        return _DefineProperty(obj, propertyKey, attrs, getter, setter, strict);
    }
    if (hasValue) {
        if (strict) {
            if ((attrs & (0x01 | 0x02 | 0x04)) ===
                (0x01 | 0x02 | 0x04))
            {
                _DefineDataProperty(obj, propertyKey, value);
                return true;
            }
        }
        return _DefineProperty(obj, propertyKey, attrs, value, null, strict);
    }
    return _DefineProperty(obj, propertyKey, attrs, undefined, undefined, strict);
}
function ObjectDefineProperty(obj, propertyKey, attributes) {
    ObjectOrReflectDefineProperty(obj, propertyKey, attributes, true);
    return obj;
}
function Promise_catch(onRejected) {
    return callContentFunction(this.then, this, undefined, onRejected);
}
function Promise_finally(onFinally) {
    var promise = this;
    if (!IsObject(promise))
        ThrowTypeError(3, "Promise", "finally", "value");
    var C = SpeciesConstructor(promise, GetBuiltinConstructor("Promise"));
    ;;
    var thenFinally, catchFinally;
    if (!IsCallable(onFinally)) {
        thenFinally = onFinally;
        catchFinally = onFinally;
    } else {
        (thenFinally) = function(value) {
            var result = onFinally();
            var promise = PromiseResolve(C, result);
            return callContentFunction(promise.then, promise, function() { return value; });
        };
        (catchFinally) = function(reason) {
            var result = onFinally();
            var promise = PromiseResolve(C, result);
            return callContentFunction(promise.then, promise, function() { throw reason; });
        };
    }
    return callContentFunction(promise.then, promise, thenFinally, catchFinally);
}
function CreateListFromArrayLikeForArgs(obj) {
    ;;
    var len = ToLength(obj.length);
    if (len > (500 * 1000))
        ThrowRangeError(97);
    var list = std_Array(len);
    for (var i = 0; i < len; i++)
        _DefineDataProperty(list, i, obj[i]);
    return list;
}
function Reflect_apply(target, thisArgument, argumentsList) {
    if (!IsCallable(target))
        ThrowTypeError(9, DecompileArg(0, target));
    if (!IsObject(argumentsList)) {
        ThrowTypeError(42, "`argumentsList`", "Reflect.apply",
                       ToSource(argumentsList));
    }
    return callFunction(std_Function_apply, target, thisArgument, argumentsList);
}
function Reflect_construct(target, argumentsList ) {
    if (!IsConstructor(target))
        ThrowTypeError(10, DecompileArg(0, target));
    var newTarget;
    if (arguments.length > 2) {
        newTarget = arguments[2];
        if (!IsConstructor(newTarget))
            ThrowTypeError(10, DecompileArg(2, newTarget));
    } else {
        newTarget = target;
    }
    if (!IsObject(argumentsList)) {
        ThrowTypeError(42, "`argumentsList`", "Reflect.construct",
                       ToSource(argumentsList));
    }
    var args = (IsPackedArray(argumentsList) && argumentsList.length <= (500 * 1000))
               ? argumentsList
               : CreateListFromArrayLikeForArgs(argumentsList);
    switch (args.length) {
      case 0:
        return constructContentFunction(target, newTarget);
      case 1:
        return constructContentFunction(target, newTarget, args[0]);
      case 2:
        return constructContentFunction(target, newTarget, args[0], args[1]);
      case 3:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2]);
      case 4:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3]);
      case 5:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4]);
      case 6:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5]);
      case 7:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
      case 8:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
      case 9:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
      case 10:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
      case 11:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
      case 12:
        return constructContentFunction(target, newTarget, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
      default:
        return _ConstructFunction(target, newTarget, args);
    }
}
function Reflect_defineProperty(obj, propertyKey, attributes) {
    return ObjectOrReflectDefineProperty(obj, propertyKey, attributes, false);
}
function Reflect_getOwnPropertyDescriptor(target, propertyKey) {
    if (!IsObject(target))
        ThrowTypeError(40, DecompileArg(0, target));
    return ObjectGetOwnPropertyDescriptor(target, propertyKey);
}
function Reflect_has(target, propertyKey) {
    if (!IsObject(target)) {
        ThrowTypeError(42, "`target`", "Reflect.has",
                       ToSource(target));
    }
    return propertyKey in target;
}
function Reflect_get(target, propertyKey ) {
    if (!IsObject(target)) {
        ThrowTypeError(42, "`target`", "Reflect.get",
                       ToSource(target));
    }
    if (arguments.length > 2) {
        return getPropertySuper(target, propertyKey, arguments[2]);
    }
    return target[propertyKey];
}
function RegExpFlagsGetter() {
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(40, R === null ? "null" : typeof R);
    var result = "";
    if (R.global)
        result += "g";
    if (R.ignoreCase)
        result += "i";
    if (R.multiline)
        result += "m";
    if (R.unicode)
         result += "u";
    if (R.sticky)
        result += "y";
    return result;
}
_SetCanonicalName(RegExpFlagsGetter, "get flags");
function RegExpToString()
{
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(40, R === null ? "null" : typeof R);
    var pattern = ToString(R.source);
    var flags = ToString(R.flags);
    return "/" + pattern + "/" + flags;
}
_SetCanonicalName(RegExpToString, "toString");
function AdvanceStringIndex(S, index) {
    ;;
    ;;
    var length = S.length;
    if (index + 1 >= length)
        return index + 1;
    var first = callFunction(std_String_charCodeAt, S, index);
    if (first < 0xD800 || first > 0xDBFF)
        return index + 1;
    var second = callFunction(std_String_charCodeAt, S, index + 1);
    if (second < 0xDC00 || second > 0xDFFF)
        return index + 1;
    return index + 2;
}
function RegExpMatch(string) {
    var rx = this;
    if (!IsObject(rx))
        ThrowTypeError(40, rx === null ? "null" : typeof rx);
    var S = ToString(string);
    if (IsRegExpMethodOptimizable(rx)) {
        var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
        var global = !!(flags & 0x02);
        if (global) {
            var fullUnicode = !!(flags & 0x10);
            return RegExpGlobalMatchOpt(rx, S, fullUnicode);
        }
        return RegExpBuiltinExec(rx, S, false);
    }
    return RegExpMatchSlowPath(rx, S);
}
function RegExpMatchSlowPath(rx, S) {
    if (!rx.global)
        return RegExpExec(rx, S, false);
    var fullUnicode = !!rx.unicode;
    rx.lastIndex = 0;
    var A = [];
    var n = 0;
    while (true) {
        var result = RegExpExec(rx, S, false);
        if (result === null)
          return (n === 0) ? null : A;
        var matchStr = ToString(result[0]);
        _DefineDataProperty(A, n, matchStr);
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }
        n++;
    }
}
function RegExpGlobalMatchOpt(rx, S, fullUnicode) {
    var lastIndex = 0;
    rx.lastIndex = 0;
    var A = [];
    var n = 0;
    var lengthS = S.length;
    while (true) {
        var result = RegExpMatcher(rx, S, lastIndex);
        if (result === null)
            return (n === 0) ? null : A;
        lastIndex = result.index + result[0].length;
        var matchStr = result[0];
        _DefineDataProperty(A, n, matchStr);
        if (matchStr === "") {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                return A;
        }
        n++;
    }
}
function IsRegExpMethodOptimizable(rx) {
    if (!IsRegExpObject(rx))
        return false;
    var RegExpProto = GetBuiltinPrototype("RegExp");
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpInstanceOptimizable(rx, RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}
function RegExpReplace(string, replaceValue) {
    var rx = this;
    if (!IsObject(rx))
        ThrowTypeError(40, rx === null ? "null" : typeof rx);
    var S = ToString(string);
    var lengthS = S.length;
    var functionalReplace = IsCallable(replaceValue);
    var firstDollarIndex = -1;
    if (!functionalReplace) {
        replaceValue = ToString(replaceValue);
        if (replaceValue.length > 1)
            firstDollarIndex = GetFirstDollarIndex(replaceValue);
    }
    if (IsRegExpMethodOptimizable(rx)) {
        var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
        var global = !!(flags & 0x02);
        if (global) {
            if (functionalReplace) {
                if (lengthS > 5000) {
                    var elemBase = GetElemBaseForLambda(replaceValue);
                    if (IsObject(elemBase)) {
                        return RegExpGlobalReplaceOptElemBase(rx, S, lengthS, replaceValue, flags,
                                                              elemBase);
                    }
                }
                return RegExpGlobalReplaceOptFunc(rx, S, lengthS, replaceValue, flags);
            }
            if (firstDollarIndex !== -1) {
                return RegExpGlobalReplaceOptSubst(rx, S, lengthS, replaceValue, flags,
                                                   firstDollarIndex);
            }
            if (lengthS < 0x7fff)
                return RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue, flags);
            return RegExpGlobalReplaceOpt(rx, S, lengthS, replaceValue, flags);
        }
        if (functionalReplace)
            return RegExpLocalReplaceOptFunc(rx, S, lengthS, replaceValue);
        if (firstDollarIndex !== -1)
            return RegExpLocalReplaceOptSubst(rx, S, lengthS, replaceValue, firstDollarIndex);
        if (lengthS < 0x7fff)
            return RegExpLocalReplaceOptShort(rx, S, lengthS, replaceValue);
        return RegExpLocalReplaceOpt(rx, S, lengthS, replaceValue);
    }
    return RegExpReplaceSlowPath(rx, S, lengthS, replaceValue,
                                 functionalReplace, firstDollarIndex);
}
function RegExpReplaceSlowPath(rx, S, lengthS, replaceValue,
                               functionalReplace, firstDollarIndex)
{
    var global = !!rx.global;
    var fullUnicode = false;
    if (global) {
        fullUnicode = !!rx.unicode;
        rx.lastIndex = 0;
    }
    var results = [];
    var nResults = 0;
    while (true) {
        var result = RegExpExec(rx, S, false);
        if (result === null)
            break;
        _DefineDataProperty(results, nResults++, result);
        if (!global)
            break;
        var matchStr = ToString(result[0]);
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }
    }
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    for (var i = 0; i < nResults; i++) {
        result = results[i];
        var nCaptures = std_Math_max(ToLength(result.length) - 1, 0);
        var matched = ToString(result[0]);
        var matchLength = matched.length;
        var position = std_Math_max(std_Math_min(ToInteger(result.index), lengthS), 0);
        var n, capN, replacement;
        if (functionalReplace || firstDollarIndex !== -1) {
            replacement = RegExpGetComplexReplacement(result, matched, S, position,
                                                      nCaptures, replaceValue,
                                                      functionalReplace, firstDollarIndex);
        } else {
            for (n = 1; n <= nCaptures; n++) {
                capN = result[n];
                if (capN !== undefined)
                    ToString(capN);
            }
            replacement = replaceValue;
        }
        if (position >= nextSourcePosition) {
            accumulatedResult += Substring(S, nextSourcePosition,
                                           position - nextSourcePosition) + replacement;
            nextSourcePosition = position + matchLength;
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpGetComplexReplacement(result, matched, S, position,
                                     nCaptures, replaceValue,
                                     functionalReplace, firstDollarIndex)
{
    var captures = [];
    var capturesLength = 0;
    _DefineDataProperty(captures, capturesLength++, matched);
    for (var n = 1; n <= nCaptures; n++) {
        var capN = result[n];
        if (capN !== undefined)
            capN = ToString(capN);
        _DefineDataProperty(captures, capturesLength++, capN);
    }
    if (functionalReplace) {
        switch (nCaptures) {
          case 0:
            return ToString(replaceValue(captures[0], position, S));
          case 1:
            return ToString(replaceValue(captures[0], captures[1], position, S));
          case 2:
            return ToString(replaceValue(captures[0], captures[1], captures[2], position, S));
          case 3:
            return ToString(replaceValue(captures[0], captures[1], captures[2], captures[3], position, S));
          case 4:
            return ToString(replaceValue(captures[0], captures[1], captures[2], captures[3], captures[4], position, S));
          default:
            _DefineDataProperty(captures, capturesLength++, position);
            _DefineDataProperty(captures, capturesLength++, S);
            return ToString(callFunction(std_Function_apply, replaceValue, undefined, captures));
        }
    }
    return RegExpGetSubstitution(captures, S, position, replaceValue, firstDollarIndex);
}
function RegExpGetFunctionalReplacement(result, S, position, replaceValue) {
    ;;
    var nCaptures = result.length - 1;
    switch (nCaptures) {
      case 0:
        return ToString(replaceValue(result[0], position, S));
      case 1:
        return ToString(replaceValue(result[0], result[1], position, S));
      case 2:
        return ToString(replaceValue(result[0], result[1], result[2], position, S));
      case 3:
        return ToString(replaceValue(result[0], result[1], result[2], result[3], position, S));
      case 4:
        return ToString(replaceValue(result[0], result[1], result[2], result[3], result[4], position, S));
    }
    var captures = [];
    for (var n = 0; n <= nCaptures; n++) {
        ;
                                                                  ;
        _DefineDataProperty(captures, n, result[n]);
    }
    _DefineDataProperty(captures, nCaptures + 1, position);
    _DefineDataProperty(captures, nCaptures + 2, S);
    return ToString(callFunction(std_Function_apply, replaceValue, undefined, captures));
}
function RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue, flags)
{
    var fullUnicode = !!(flags & 0x10);
    var lastIndex = 0;
    rx.lastIndex = 0;
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    while (true) {
        var result = RegExpSearcher(rx, S, lastIndex);
        if (result === -1)
            break;
        var position = result & 0x7fff;
        lastIndex = (result >> 15) & 0x7fff;
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replaceValue;
        nextSourcePosition = lastIndex;
        if (lastIndex === position) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpGlobalReplaceOpt(rx, S, lengthS, replaceValue, flags
                  )
{
    var fullUnicode = !!(flags & 0x10);
    var lastIndex = 0;
    rx.lastIndex = 0;
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    while (true) {
        var result = RegExpMatcher(rx, S, lastIndex);
        if (result === null)
            break;
        ;;
        var matched = result[0];
        var matchLength = matched.length | 0;
        var position = result.index | 0;
        lastIndex = position + matchLength;
        var replacement;
        replacement = replaceValue;
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replacement;
        nextSourcePosition = lastIndex;
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
            lastIndex |= 0;
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpGlobalReplaceOptFunc(rx, S, lengthS, replaceValue, flags
                  )
{
    var fullUnicode = !!(flags & 0x10);
    var lastIndex = 0;
    rx.lastIndex = 0;
    var originalSource = UnsafeGetStringFromReservedSlot(rx, 1);
    var originalFlags = flags;
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    while (true) {
        var result = RegExpMatcher(rx, S, lastIndex);
        if (result === null)
            break;
        ;;
        var matched = result[0];
        var matchLength = matched.length | 0;
        var position = result.index | 0;
        lastIndex = position + matchLength;
        var replacement;
        replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replacement;
        nextSourcePosition = lastIndex;
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
            lastIndex |= 0;
        }
        if (UnsafeGetStringFromReservedSlot(rx, 1) !== originalSource ||
            UnsafeGetInt32FromReservedSlot(rx, 2) !== originalFlags)
        {
            rx = regexp_construct_raw_flags(originalSource, originalFlags);
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpGlobalReplaceOptElemBase(rx, S, lengthS, replaceValue, flags
                   , elemBase
                  )
{
    var fullUnicode = !!(flags & 0x10);
    var lastIndex = 0;
    rx.lastIndex = 0;
    var originalSource = UnsafeGetStringFromReservedSlot(rx, 1);
    var originalFlags = flags;
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    while (true) {
        var result = RegExpMatcher(rx, S, lastIndex);
        if (result === null)
            break;
        ;;
        var matched = result[0];
        var matchLength = matched.length | 0;
        var position = result.index | 0;
        lastIndex = position + matchLength;
        var replacement;
        if (IsObject(elemBase)) {
            var prop = GetStringDataProperty(elemBase, matched);
            if (prop !== undefined) {
                ;
                                                                                        ;
                replacement = prop;
            } else {
                elemBase = undefined;
            }
        }
        if (!IsObject(elemBase))
            replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replacement;
        nextSourcePosition = lastIndex;
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
            lastIndex |= 0;
        }
        if (UnsafeGetStringFromReservedSlot(rx, 1) !== originalSource ||
            UnsafeGetInt32FromReservedSlot(rx, 2) !== originalFlags)
        {
            rx = regexp_construct_raw_flags(originalSource, originalFlags);
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpGlobalReplaceOptSubst(rx, S, lengthS, replaceValue, flags
                   , firstDollarIndex
                  )
{
    var fullUnicode = !!(flags & 0x10);
    var lastIndex = 0;
    rx.lastIndex = 0;
    var accumulatedResult = "";
    var nextSourcePosition = 0;
    while (true) {
        var result = RegExpMatcher(rx, S, lastIndex);
        if (result === null)
            break;
        ;;
        var matched = result[0];
        var matchLength = matched.length | 0;
        var position = result.index | 0;
        lastIndex = position + matchLength;
        var replacement;
        replacement = RegExpGetSubstitution(result, S, position, replaceValue, firstDollarIndex);
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replacement;
        nextSourcePosition = lastIndex;
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
            lastIndex |= 0;
        }
    }
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpLocalReplaceOpt(rx, S, lengthS, replaceValue
                  )
{
    var lastIndex = ToLength(rx.lastIndex);
    var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
    var globalOrSticky = !!(flags & (0x02 | 0x08));
    if (globalOrSticky) {
        if (lastIndex > lengthS) {
            if (globalOrSticky)
                rx.lastIndex = 0;
            return S;
        }
    } else {
        lastIndex = 0;
    }
    var result = RegExpMatcher(rx, S, lastIndex);
    if (result === null) {
        if (globalOrSticky)
            rx.lastIndex = 0;
        return S;
    }
    ;;
    var matched = result[0];
    var matchLength = matched.length;
    var position = result.index;
    var nextSourcePosition = position + matchLength;
    if (globalOrSticky)
       rx.lastIndex = nextSourcePosition;
    var replacement;
    replacement = replaceValue;
    var accumulatedResult = Substring(S, 0, position) + replacement;
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpLocalReplaceOptShort(rx, S, lengthS, replaceValue
                  )
{
    var lastIndex = ToLength(rx.lastIndex);
    var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
    var globalOrSticky = !!(flags & (0x02 | 0x08));
    if (globalOrSticky) {
        if (lastIndex > lengthS) {
            if (globalOrSticky)
                rx.lastIndex = 0;
            return S;
        }
    } else {
        lastIndex = 0;
    }
    var result = RegExpSearcher(rx, S, lastIndex);
    if (result === -1) {
        if (globalOrSticky)
            rx.lastIndex = 0;
        return S;
    }
    var position = result & 0x7fff;
    var nextSourcePosition = (result >> 15) & 0x7fff;
    if (globalOrSticky)
       rx.lastIndex = nextSourcePosition;
    var replacement;
    replacement = replaceValue;
    var accumulatedResult = Substring(S, 0, position) + replacement;
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpLocalReplaceOptFunc(rx, S, lengthS, replaceValue
                  )
{
    var lastIndex = ToLength(rx.lastIndex);
    var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
    var globalOrSticky = !!(flags & (0x02 | 0x08));
    if (globalOrSticky) {
        if (lastIndex > lengthS) {
            if (globalOrSticky)
                rx.lastIndex = 0;
            return S;
        }
    } else {
        lastIndex = 0;
    }
    var result = RegExpMatcher(rx, S, lastIndex);
    if (result === null) {
        if (globalOrSticky)
            rx.lastIndex = 0;
        return S;
    }
    ;;
    var matched = result[0];
    var matchLength = matched.length;
    var position = result.index;
    var nextSourcePosition = position + matchLength;
    if (globalOrSticky)
       rx.lastIndex = nextSourcePosition;
    var replacement;
    replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
    var accumulatedResult = Substring(S, 0, position) + replacement;
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpLocalReplaceOptSubst(rx, S, lengthS, replaceValue
                   , firstDollarIndex
                  )
{
    var lastIndex = ToLength(rx.lastIndex);
    var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
    var globalOrSticky = !!(flags & (0x02 | 0x08));
    if (globalOrSticky) {
        if (lastIndex > lengthS) {
            if (globalOrSticky)
                rx.lastIndex = 0;
            return S;
        }
    } else {
        lastIndex = 0;
    }
    var result = RegExpMatcher(rx, S, lastIndex);
    if (result === null) {
        if (globalOrSticky)
            rx.lastIndex = 0;
        return S;
    }
    ;;
    var matched = result[0];
    var matchLength = matched.length;
    var position = result.index;
    var nextSourcePosition = position + matchLength;
    if (globalOrSticky)
       rx.lastIndex = nextSourcePosition;
    var replacement;
    replacement = RegExpGetSubstitution(result, S, position, replaceValue, firstDollarIndex);
    var accumulatedResult = Substring(S, 0, position) + replacement;
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
function RegExpSearch(string) {
    var rx = this;
    if (!IsObject(rx))
        ThrowTypeError(40, rx === null ? "null" : typeof rx);
    var S = ToString(string);
    var previousLastIndex = rx.lastIndex;
    var lastIndexIsZero = SameValue(previousLastIndex, 0);
    if (!lastIndexIsZero)
        rx.lastIndex = 0;
    if (IsRegExpMethodOptimizable(rx) && S.length < 0x7fff) {
        var result = RegExpSearcher(rx, S, 0);
        if (!lastIndexIsZero) {
            rx.lastIndex = previousLastIndex;
        } else {
            var flags = UnsafeGetInt32FromReservedSlot(rx, 2);
            if (flags & (0x02 | 0x08))
                rx.lastIndex = previousLastIndex;
        }
        if (result === -1)
            return -1;
        return result & 0x7fff;
    }
    return RegExpSearchSlowPath(rx, S, previousLastIndex);
}
function RegExpSearchSlowPath(rx, S, previousLastIndex) {
    var result = RegExpExec(rx, S, false);
    var currentLastIndex = rx.lastIndex;
    if (!SameValue(currentLastIndex, previousLastIndex))
        rx.lastIndex = previousLastIndex;
    if (result === null)
        return -1;
    return result.index;
}
function IsRegExpSplitOptimizable(rx, C) {
    if (!IsRegExpObject(rx))
        return false;
    var RegExpCtor = GetBuiltinConstructor("RegExp");
    if (C !== RegExpCtor)
        return false;
    var RegExpProto = RegExpCtor.prototype;
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpInstanceOptimizable(rx, RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}
function RegExpSplit(string, limit) {
    var rx = this;
    if (!IsObject(rx))
        ThrowTypeError(40, rx === null ? "null" : typeof rx);
    var S = ToString(string);
    var C = SpeciesConstructor(rx, GetBuiltinConstructor("RegExp"));
    var optimizable = IsRegExpSplitOptimizable(rx, C) &&
                      (limit === undefined || typeof limit == "number");
    var flags, unicodeMatching, splitter;
    if (optimizable) {
        flags = UnsafeGetInt32FromReservedSlot(rx, 2);
        unicodeMatching = !!(flags & (0x10));
        if (flags & 0x08) {
            var source = UnsafeGetStringFromReservedSlot(rx, 1);
            splitter = regexp_construct_raw_flags(source, flags & ~0x08);
        } else {
            splitter = rx;
        }
    } else {
        flags = ToString(rx.flags);
        unicodeMatching = callFunction(std_String_includes, flags, "u");
        var newFlags;
        if (callFunction(std_String_includes, flags, "y"))
            newFlags = flags;
        else
            newFlags = flags + "y";
        splitter = new C(rx, newFlags);
    }
    var A = [];
    var lengthA = 0;
    var lim;
    if (limit === undefined)
        lim = 0xffffffff;
    else
        lim = limit >>> 0;
    var p = 0;
    if (lim === 0)
        return A;
    var size = S.length;
    if (size === 0) {
        var z;
        if (optimizable)
            z = RegExpMatcher(splitter, S, 0);
        else
            z = RegExpExec(splitter, S, false);
        if (z !== null)
            return A;
        _DefineDataProperty(A, 0, S);
        return A;
    }
    var q = p;
    while (q < size) {
        var e;
        if (optimizable) {
            z = RegExpMatcher(splitter, S, q);
            if (z === null)
                break;
            q = z.index;
            if (q >= size)
                break;
            e = q + z[0].length;
        } else {
            splitter.lastIndex = q;
            z = RegExpExec(splitter, S, false);
            if (z === null) {
                q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
                continue;
            }
            e = ToLength(splitter.lastIndex);
        }
        if (e === p) {
            q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
            continue;
        }
        _DefineDataProperty(A, lengthA, Substring(S, p, q - p));
        lengthA++;
        if (lengthA === lim)
            return A;
        p = e;
        var numberOfCaptures = std_Math_max(ToLength(z.length) - 1, 0);
        var i = 1;
        while (i <= numberOfCaptures) {
            _DefineDataProperty(A, lengthA, z[i]);
            i++;
            lengthA++;
            if (lengthA === lim)
                return A;
        }
        q = p;
    }
    if (p >= size)
        _DefineDataProperty(A, lengthA, "");
    else
        _DefineDataProperty(A, lengthA, Substring(S, p, size - p));
    return A;
}
function RegExp_prototype_Exec(string) {
    var R = this;
    if (!IsObject(R) || !IsRegExpObject(R))
        return callFunction(CallRegExpMethodIfWrapped, R, string, "RegExp_prototype_Exec");
    var S = ToString(string);
    return RegExpBuiltinExec(R, S, false);
}
function RegExpExec(R, S, forTest) {
    var exec = R.exec;
    if (exec === RegExp_prototype_Exec || !IsCallable(exec)) {
        return RegExpBuiltinExec(R, S, forTest);
    }
    var result = callContentFunction(exec, R, S);
    if (typeof result !== "object")
        ThrowTypeError(428);
    return forTest ? result !== null : result;
}
function RegExpBuiltinExec(R, S, forTest) {
    if (!IsRegExpObject(R))
        return UnwrapAndCallRegExpBuiltinExec(R, S, forTest);
    var lastIndex = ToLength(R.lastIndex);
    var flags = UnsafeGetInt32FromReservedSlot(R, 2);
    var globalOrSticky = !!(flags & (0x02 | 0x08));
    if (!globalOrSticky) {
        lastIndex = 0;
    } else {
        if (lastIndex > S.length) {
            if (globalOrSticky)
                R.lastIndex = 0;
            return forTest ? false : null;
        }
    }
    if (forTest) {
        var endIndex = RegExpTester(R, S, lastIndex);
        if (endIndex == -1) {
            if (globalOrSticky)
                R.lastIndex = 0;
            return false;
        }
        if (globalOrSticky)
            R.lastIndex = endIndex;
        return true;
    }
    var result = RegExpMatcher(R, S, lastIndex);
    if (result === null) {
        if (globalOrSticky)
            R.lastIndex = 0;
    } else {
        if (globalOrSticky)
            R.lastIndex = result.index + result[0].length;
    }
    return result;
}
function UnwrapAndCallRegExpBuiltinExec(R, S, forTest) {
    return callFunction(CallRegExpMethodIfWrapped, R, S, forTest, "CallRegExpBuiltinExec");
}
function CallRegExpBuiltinExec(S, forTest) {
    return RegExpBuiltinExec(this, S, forTest);
}
function RegExpTest(string) {
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(40, R === null ? "null" : typeof R);
    var S = ToString(string);
    return RegExpExec(R, S, true);
}
function RegExpSpecies() {
    return this;
}
_SetCanonicalName(RegExpSpecies, "get [Symbol.species]");
function StringProtoHasNoMatch() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_match in StringProto);
}
function IsStringMatchOptimizable() {
    var RegExpProto = GetBuiltinPrototype("RegExp");
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec &&
           RegExpProto[std_match] === RegExpMatch;
}
function String_match(regexp) {
    RequireObjectCoercible(this);
    var isPatternString = (typeof regexp === "string");
    if (!(isPatternString && StringProtoHasNoMatch()) && regexp !== undefined && regexp !== null) {
        var matcher = GetMethod(regexp, std_match);
        if (matcher !== undefined)
            return callContentFunction(matcher, regexp, this);
    }
    var S = ToString(this);
    if (isPatternString && IsStringMatchOptimizable()) {
        var flatResult = FlatStringMatch(S, regexp);
        if (flatResult !== undefined)
            return flatResult;
    }
    var rx = RegExpCreate(regexp);
    if (IsStringMatchOptimizable())
        return RegExpMatcher(rx, S, 0);
    return callContentFunction(GetMethod(rx, std_match), rx, S);
}
function String_generic_match(thisValue, regexp) {
    WarnDeprecatedStringMethod(8, "match");
    if (thisValue === undefined)
        ThrowTypeError(39, 0, "String.match");
    return callFunction(String_match, thisValue, regexp);
}
function String_pad(maxLength, fillString, padEnd) {
    RequireObjectCoercible(this);
    let str = ToString(this);
    let intMaxLength = ToLength(maxLength);
    let strLen = str.length;
    if (intMaxLength <= strLen)
        return str;
    let filler = fillString === undefined ? " " : ToString(fillString);
    if (filler === "")
        return str;
    if (intMaxLength > ((1 << 28) - 1))
        ThrowRangeError(86);
    let fillLen = intMaxLength - strLen;
    let truncatedStringFiller = callFunction(String_repeat, filler,
                                             (fillLen / filler.length) | 0);
    truncatedStringFiller += Substring(filler, 0, fillLen % filler.length);
    if (padEnd === true)
        return str + truncatedStringFiller;
    return truncatedStringFiller + str;
}
function String_pad_start(maxLength, fillString = " ") {
    return callFunction(String_pad, this, maxLength, fillString, false);
}
function String_pad_end(maxLength, fillString = " ") {
    return callFunction(String_pad, this, maxLength, fillString, true);
}
function StringProtoHasNoReplace() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_replace in StringProto);
}
function Substring(str, from, length) {
    ;;
    ;;
    ;;
    return SubstringKernel(str, from | 0, length | 0);
}
function String_replace(searchValue, replaceValue) {
    RequireObjectCoercible(this);
    if (!(typeof searchValue === "string" && StringProtoHasNoReplace()) &&
        searchValue !== undefined && searchValue !== null)
    {
        var replacer = GetMethod(searchValue, std_replace);
        if (replacer !== undefined)
            return callContentFunction(replacer, searchValue, this, replaceValue);
    }
    var string = ToString(this);
    var searchString = ToString(searchValue);
    if (typeof replaceValue === "string") {
        return StringReplaceString(string, searchString, replaceValue);
    }
    if (!IsCallable(replaceValue)) {
        return StringReplaceString(string, searchString, ToString(replaceValue));
    }
    var pos = callFunction(std_String_indexOf, string, searchString);
    if (pos === -1)
        return string;
    var replStr = ToString(callContentFunction(replaceValue, undefined, searchString, pos, string));
    var tailPos = pos + searchString.length;
    var newString;
    if (pos === 0)
        newString = "";
    else
        newString = Substring(string, 0, pos);
    newString += replStr;
    var stringLength = string.length;
    if (tailPos < stringLength)
        newString += Substring(string, tailPos, stringLength - tailPos);
    return newString;
}
function String_generic_replace(thisValue, searchValue, replaceValue) {
    WarnDeprecatedStringMethod(10, "replace");
    if (thisValue === undefined)
        ThrowTypeError(39, 0, "String.replace");
    return callFunction(String_replace, thisValue, searchValue, replaceValue);
}
function StringProtoHasNoSearch() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_search in StringProto);
}
function IsStringSearchOptimizable() {
    var RegExpProto = GetBuiltinPrototype("RegExp");
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec &&
           RegExpProto[std_search] === RegExpSearch;
}
function String_search(regexp) {
    RequireObjectCoercible(this);
    var isPatternString = (typeof regexp === "string");
    if (!(isPatternString && StringProtoHasNoSearch()) && regexp !== undefined && regexp !== null) {
        var searcher = GetMethod(regexp, std_search);
        if (searcher !== undefined)
            return callContentFunction(searcher, regexp, this);
    }
    var string = ToString(this);
    if (isPatternString && IsStringSearchOptimizable()) {
        var flatResult = FlatStringSearch(string, regexp);
        if (flatResult !== -2)
            return flatResult;
    }
    var rx = RegExpCreate(regexp);
    return callContentFunction(GetMethod(rx, std_search), rx, string);
}
function String_generic_search(thisValue, regexp) {
    WarnDeprecatedStringMethod(11, "search");
    if (thisValue === undefined)
        ThrowTypeError(39, 0, "String.search");
    return callFunction(String_search, thisValue, regexp);
}
function StringProtoHasNoSplit() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_split in StringProto);
}
function String_split(separator, limit) {
    RequireObjectCoercible(this);
    if (typeof this === "string") {
        if (StringProtoHasNoSplit()) {
            if (typeof separator === "string") {
                if (limit === undefined) {
                    return StringSplitString(this, separator);
                }
            }
        }
    }
    if (!(typeof separator == "string" && StringProtoHasNoSplit()) &&
        separator !== undefined && separator !== null)
    {
        var splitter = GetMethod(separator, std_split);
        if (splitter !== undefined)
            return callContentFunction(splitter, separator, this, limit);
    }
    var S = ToString(this);
    var R;
    if (limit !== undefined) {
        var lim = limit >>> 0;
        R = ToString(separator);
        if (lim === 0)
            return [];
        if (separator === undefined)
            return [S];
        return StringSplitStringLimit(S, R, lim);
    }
    R = ToString(separator);
    if (separator === undefined)
        return [S];
    return StringSplitString(S, R);
}
function String_generic_split(thisValue, separator, limit) {
    WarnDeprecatedStringMethod(13, "split");
    if (thisValue === undefined)
        ThrowTypeError(39, 0, "String.split");
    return callFunction(String_split, thisValue, separator, limit);
}
function String_substring(start, end) {
    RequireObjectCoercible(this);
    var str = ToString(this);
    var len = str.length;
    var intStart = ToInteger(start);
    var intEnd = (end === undefined) ? len : ToInteger(end);
    var finalStart = std_Math_min(std_Math_max(intStart, 0), len);
    var finalEnd = std_Math_min(std_Math_max(intEnd, 0), len);
    var from, to;
    if (finalStart < finalEnd) {
        from = finalStart;
        to = finalEnd;
    } else {
        from = finalEnd;
        to = finalStart;
    }
    return SubstringKernel(str, from | 0, (to - from) | 0);
}
function String_static_substring(string, start, end) {
    WarnDeprecatedStringMethod(16, "substring");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.substring");
    return callFunction(String_substring, string, start, end);
}
function String_substr(start, length) {
    RequireObjectCoercible(this);
    var str = ToString(this);
    var intStart = ToInteger(start);
    var size = str.length;
    var end = (length === undefined) ? size : ToInteger(length);
    if (intStart < 0)
        intStart = std_Math_max(intStart + size, 0);
    var resultLength = std_Math_min(std_Math_max(end, 0), size - intStart);
    if (resultLength <= 0)
        return "";
    return SubstringKernel(str, intStart | 0, resultLength | 0);
}
function String_static_substr(string, start, length) {
    WarnDeprecatedStringMethod(15, "substr");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.substr");
    return callFunction(String_substr, string, start, length);
}
function String_slice(start, end) {
    RequireObjectCoercible(this);
    var str = ToString(this);
    var len = str.length;
    var intStart = ToInteger(start);
    var intEnd = (end === undefined) ? len : ToInteger(end);
    var from = (intStart < 0) ? std_Math_max(len + intStart, 0) : std_Math_min(intStart, len);
    var to = (intEnd < 0) ? std_Math_max(len + intEnd, 0) : std_Math_min(intEnd, len);
    var span = std_Math_max(to - from, 0);
    return SubstringKernel(str, from | 0, span | 0);
}
function String_static_slice(string, start, end) {
    WarnDeprecatedStringMethod(12, "slice");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.slice");
    return callFunction(String_slice, string, start, end);
}
function String_codePointAt(pos) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    var position = ToInteger(pos);
    var size = S.length;
    if (position < 0 || position >= size)
        return undefined;
    var first = callFunction(std_String_charCodeAt, S, position);
    if (first < 0xD800 || first > 0xDBFF || position + 1 === size)
        return first;
    var second = callFunction(std_String_charCodeAt, S, position + 1);
    if (second < 0xDC00 || second > 0xDFFF)
        return first;
    return (first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000;
}
function String_repeat(count) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    var n = ToInteger(count);
    if (n < 0)
        ThrowRangeError(84);
    if (!(n * S.length <= ((1 << 28) - 1)))
        ThrowRangeError(86);
    ;
                                                        ;
    n = n & ((1 << 28) - 1);
    var T = "";
    for (;;) {
        if (n & 1)
            T += S;
        n >>= 1;
        if (n)
            S += S;
        else
            break;
    }
    return T;
}
function String_iterator() {
    RequireObjectCoercible(this);
    var S = ToString(this);
    var iterator = NewStringIterator();
    UnsafeSetReservedSlot(iterator, 0, S);
    UnsafeSetReservedSlot(iterator, 1, 0);
    return iterator;
}
function StringIteratorNext() {
    var obj;
    if (!IsObject(this) || (obj = GuardToStringIterator(this)) === null) {
        return callFunction(CallStringIteratorMethodIfWrapped, this,
                            "StringIteratorNext");
    }
    var S = UnsafeGetStringFromReservedSlot(obj, 0);
    var index = UnsafeGetInt32FromReservedSlot(obj, 1);
    var size = S.length;
    var result = { value: undefined, done: false };
    if (index >= size) {
        result.done = true;
        return result;
    }
    var charCount = 1;
    var first = callFunction(std_String_charCodeAt, S, index);
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < size) {
        var second = callFunction(std_String_charCodeAt, S, index + 1);
        if (second >= 0xDC00 && second <= 0xDFFF) {
            first = (first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000;
            charCount = 2;
        }
    }
    UnsafeSetReservedSlot(obj, 1, index + charCount);
    result.value = callFunction(std_String_fromCodePoint, null, first & 0x1fffff);
    return result;
}
var collatorCache = new Record();
function String_localeCompare(that) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    var That = ToString(that);
    var locales = arguments.length > 1 ? arguments[1] : undefined;
    var options = arguments.length > 2 ? arguments[2] : undefined;
    var collator;
    if (locales === undefined && options === undefined) {
        if (!IsRuntimeDefaultLocale(collatorCache.runtimeDefaultLocale)) {
            collatorCache.collator = intl_Collator(locales, options);
            collatorCache.runtimeDefaultLocale = RuntimeDefaultLocale();
        }
        collator = collatorCache.collator;
    } else {
        collator = intl_Collator(locales, options);
    }
    return intl_CompareStrings(collator, S, That);
}
function String_toLocaleLowerCase() {
    RequireObjectCoercible(this);
    var string = ToString(this);
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var requestedLocale;
    if (locales === undefined) {
        requestedLocale = undefined;
    } else if (typeof locales === "string") {
        requestedLocale = ValidateAndCanonicalizeLanguageTag(locales);
    } else {
        var requestedLocales = CanonicalizeLocaleList(locales);
        requestedLocale = requestedLocales.length > 0 ? requestedLocales[0] : undefined;
    }
    if (string.length === 0)
        return "";
    if (requestedLocale === undefined)
        requestedLocale = DefaultLocale();
    return intl_toLocaleLowerCase(string, requestedLocale);
}
function String_toLocaleUpperCase() {
    RequireObjectCoercible(this);
    var string = ToString(this);
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var requestedLocale;
    if (locales === undefined) {
        requestedLocale = undefined;
    } else if (typeof locales === "string") {
        requestedLocale = ValidateAndCanonicalizeLanguageTag(locales);
    } else {
        var requestedLocales = CanonicalizeLocaleList(locales);
        requestedLocale = requestedLocales.length > 0 ? requestedLocales[0] : undefined;
    }
    if (string.length === 0)
        return "";
    if (requestedLocale === undefined)
        requestedLocale = DefaultLocale();
    return intl_toLocaleUpperCase(string, requestedLocale);
}
function String_static_raw(callSite ) {
    var cooked = ToObject(callSite);
    var raw = ToObject(cooked.raw);
    var literalSegments = ToLength(raw.length);
    if (literalSegments === 0)
        return "";
    if (literalSegments === 1)
        return ToString(raw[0]);
    var resultString = ToString(raw[0]);
    for (var nextIndex = 1; nextIndex < literalSegments; nextIndex++) {
        if (nextIndex < arguments.length)
            resultString += ToString(arguments[nextIndex]);
        resultString += ToString(raw[nextIndex]);
    }
    return resultString;
}
function String_static_localeCompare(str1, str2) {
    WarnDeprecatedStringMethod(7, "localeCompare");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.localeCompare");
    var locales = arguments.length > 2 ? arguments[2] : undefined;
    var options = arguments.length > 3 ? arguments[3] : undefined;
    return callFunction(std_String_localeCompare, str1, str2, locales, options);
}
function String_big() {
    RequireObjectCoercible(this);
    return "<big>" + ToString(this) + "</big>";
}
function String_blink() {
    RequireObjectCoercible(this);
    return "<blink>" + ToString(this) + "</blink>";
}
function String_bold() {
    RequireObjectCoercible(this);
    return "<b>" + ToString(this) + "</b>";
}
function String_fixed() {
    RequireObjectCoercible(this);
    return "<tt>" + ToString(this) + "</tt>";
}
function String_italics() {
    RequireObjectCoercible(this);
    return "<i>" + ToString(this) + "</i>";
}
function String_small() {
    RequireObjectCoercible(this);
    return "<small>" + ToString(this) + "</small>";
}
function String_strike() {
    RequireObjectCoercible(this);
    return "<strike>" + ToString(this) + "</strike>";
}
function String_sub() {
    RequireObjectCoercible(this);
    return "<sub>" + ToString(this) + "</sub>";
}
function String_sup() {
    RequireObjectCoercible(this);
    return "<sup>" + ToString(this) + "</sup>";
}
function EscapeAttributeValue(v) {
    var inputStr = ToString(v);
    var inputLen = inputStr.length;
    var outputStr = "";
    var chunkStart = 0;
    for (var i = 0; i < inputLen; i++) {
        if (inputStr[i] === '"') {
            outputStr += callFunction(String_substring, inputStr, chunkStart, i) + "&quot;";
            chunkStart = i + 1;
        }
    }
    if (chunkStart === 0)
        return inputStr;
    if (chunkStart < inputLen)
        outputStr += callFunction(String_substring, inputStr, chunkStart);
    return outputStr;
}
function String_anchor(name) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<a name="' + EscapeAttributeValue(name) + '">' + S + "</a>";
}
function String_fontcolor(color) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<font color="' + EscapeAttributeValue(color) + '">' + S + "</font>";
}
function String_fontsize(size) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<font size="' + EscapeAttributeValue(size) + '">' + S + "</font>";
}
function String_link(url) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<a href="' + EscapeAttributeValue(url) + '">' + S + "</a>";
}
function String_static_toLowerCase(string) {
    WarnDeprecatedStringMethod(17, "toLowerCase");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.toLowerCase");
    return callFunction(std_String_toLowerCase, string);
}
function String_static_toUpperCase(string) {
    WarnDeprecatedStringMethod(20, "toUpperCase");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.toUpperCase");
    return callFunction(std_String_toUpperCase, string);
}
function String_static_charAt(string, pos) {
    WarnDeprecatedStringMethod(0, "charAt");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.charAt");
    return callFunction(std_String_charAt, string, pos);
}
function String_static_charCodeAt(string, pos) {
    WarnDeprecatedStringMethod(1, "charCodeAt");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.charCodeAt");
    return callFunction(std_String_charCodeAt, string, pos);
}
function String_static_includes(string, searchString) {
    WarnDeprecatedStringMethod(4, "includes");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.includes");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_includes, string, searchString, position);
}
function String_static_indexOf(string, searchString) {
    WarnDeprecatedStringMethod(5, "indexOf");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.indexOf");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_indexOf, string, searchString, position);
}
function String_static_lastIndexOf(string, searchString) {
    WarnDeprecatedStringMethod(6, "lastIndexOf");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.lastIndexOf");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_lastIndexOf, string, searchString, position);
}
function String_static_startsWith(string, searchString) {
    WarnDeprecatedStringMethod(14, "startsWith");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.startsWith");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_startsWith, string, searchString, position);
}
function String_static_endsWith(string, searchString) {
    WarnDeprecatedStringMethod(3, "endsWith");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.endsWith");
    var endPosition = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_endsWith, string, searchString, endPosition);
}
function String_static_trim(string) {
    WarnDeprecatedStringMethod(21, "trim");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.trim");
    return callFunction(std_String_trim, string);
}
function String_static_trimLeft(string) {
    WarnDeprecatedStringMethod(22, "trimLeft");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.trimLeft");
    return callFunction(std_String_trimStart, string);
}
function String_static_trimRight(string) {
    WarnDeprecatedStringMethod(23, "trimRight");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.trimRight");
    return callFunction(std_String_trimEnd, string);
}
function String_static_toLocaleLowerCase(string) {
    WarnDeprecatedStringMethod(18, "toLocaleLowerCase");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.toLocaleLowerCase");
    return callFunction(std_String_toLocaleLowerCase, string);
}
function String_static_toLocaleUpperCase(string) {
    WarnDeprecatedStringMethod(19, "toLocaleUpperCase");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.toLocaleUpperCase");
    return callFunction(std_String_toLocaleUpperCase, string);
}
function String_static_concat(string, arg1) {
    WarnDeprecatedStringMethod(2, "concat");
    if (arguments.length < 1)
        ThrowTypeError(39, 0, "String.concat");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, std_String_concat, string, args);
}
function SetConstructorInit(iterable) {
    var set = this;
    var adder = set.add;
    if (!IsCallable(adder))
        ThrowTypeError(9, typeof adder);
    for (var nextValue of allowContentIter(iterable))
        callContentFunction(adder, set, nextValue);
}
function SetForEach(callbackfn, thisArg = undefined) {
    var S = this;
    if (!IsObject(S) || (S = GuardToSetObject(S)) === null)
        return callFunction(CallSetMethodIfWrapped, this, callbackfn, thisArg, "SetForEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var values = callFunction(std_Set_iterator, S);
    var setIterationResult = setIteratorTemp.setIterationResult;
    if (!setIterationResult)
        setIterationResult = setIteratorTemp.setIterationResult = _CreateSetIterationResult();
    while (true) {
        var done = _GetNextSetEntryForIterator(values, setIterationResult);
        if (done)
            break;
        var value = setIterationResult[0];
        setIterationResult[0] = null;
        callContentFunction(callbackfn, thisArg, value, value, S);
    }
}
function SetValues() {
    return callFunction(std_Set_iterator, this);
}
_SetCanonicalName(SetValues, "values");
function SetSpecies() {
    return this;
}
_SetCanonicalName(SetSpecies, "get [Symbol.species]");
var setIteratorTemp = { setIterationResult: null };
function SetIteratorNext() {
    var O = this;
    if (!IsObject(O) || (O = GuardToSetIterator(O)) === null)
        return callFunction(CallSetIteratorMethodIfWrapped, this, "SetIteratorNext");
    var setIterationResult = setIteratorTemp.setIterationResult;
    if (!setIterationResult)
        setIterationResult = setIteratorTemp.setIterationResult = _CreateSetIterationResult();
    var retVal = {value: undefined, done: true};
    var done = _GetNextSetEntryForIterator(O, setIterationResult);
    if (!done) {
        var itemKind = UnsafeGetInt32FromReservedSlot(O, 2);
        var result;
        if (itemKind === 1) {
            result = setIterationResult[0];
        } else {
            ;;
            result = [setIterationResult[0], setIterationResult[0]];
        }
        setIterationResult[0] = null;
        retVal.value = result;
        retVal.done = false;
    }
    return retVal;
}
function CountingSort(array, len, signed, comparefn) {
    ;;
    if (len < 128) {
        QuickSort(array, len, comparefn);
        return array;
    }
    var min = 0;
    if (signed) {
        min = -128;
    }
    var buffer = [
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    ];
    for (var i = 0; i < len; i++) {
        var val = array[i];
        buffer[val - min]++;
    }
    var val = -1;
    for (var i = 0; i < len;) {
        var j;
        do {
            j = buffer[++val];
        } while (j === 0);
        for (; j > 0; j--)
            array[i++] = val + min;
    }
    return array;
}
function ByteAtCol(x, pos) {
    return (x >> (pos * 8)) & 0xFF;
}
function SortByColumn(array, len, aux, col, counts) {
    const R = 256;
    ;;
    for (let r = 0; r < R + 1; r++) {
        counts[r] = 0;
    }
    for (let i = 0; i < len; i++) {
        let val = array[i];
        let b = ByteAtCol(val, col);
        counts[b + 1]++;
    }
    for (let r = 0; r < R; r++) {
        counts[r + 1] += counts[r];
    }
    for (let i = 0; i < len; i++) {
        let val = array[i];
        let b = ByteAtCol(val, col);
        aux[counts[b]++] = val;
    }
    for (let i = 0; i < len; i++) {
        array[i] = aux[i];
    }
}
function RadixSort(array, len, buffer, nbytes, signed, floating, comparefn) {
    ;;
    if (len < 512) {
        QuickSort(array, len, comparefn);
        return array;
    }
    let aux = [];
    for (let i = 0; i < len; i++)
        _DefineDataProperty(aux, i, 0);
    let view = array;
    let signMask = 1 << nbytes * 8 - 1;
    if (floating) {
        if (buffer === null) {
            buffer = callFunction(std_TypedArray_buffer, array);
            ;;
        }
        let offset = IsTypedArray(array)
                     ? TypedArrayByteOffset(array)
                     : callFunction(CallTypedArrayMethodIfWrapped, array, array,
                                    "TypedArrayByteOffset");
        view = new Int32Array(buffer, offset, len);
        for (let i = 0; i < len; i++) {
            if (view[i] & signMask) {
                if ((view[i] & 0x7F800000) !== 0x7F800000 || (view[i] & 0x007FFFFF) === 0) {
                    view[i] ^= 0xFFFFFFFF;
                }
            } else {
                view[i] ^= signMask;
            }
        }
    } else if (signed) {
        for (let i = 0; i < len; i++) {
            view[i] ^= signMask;
        }
    }
    let counts = [
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,
    ];
    for (let col = 0; col < nbytes; col++) {
        SortByColumn(view, len, aux, col, counts);
    }
    if (floating) {
        for (let i = 0; i < len; i++) {
            if (view[i] & signMask) {
                view[i] ^= signMask;
            } else {
                view[i] ^= 0xFFFFFFFF;
            }
        }
    } else if (signed) {
        for (let i = 0; i < len; i++) {
            view[i] ^= signMask;
        }
    }
    return array;
}
function InsertionSort(array, from, to, comparefn) {
    let item, swap, i, j;
    for (i = from + 1; i <= to; i++) {
        item = array[i];
        for (j = i - 1; j >= from; j--) {
            swap = array[j];
            if (comparefn(swap, item) <= 0)
                break;
            array[j + 1] = swap;
        }
        array[j + 1] = item;
    }
}
function SwapArrayElements(array, i, j) {
    var swap = array[i];
    array[i] = array[j];
    array[j] = swap;
}
function Merge(list, start, mid, end, lBuffer, rBuffer, comparefn) {
    var i, j, k;
    var sizeLeft = mid - start + 1;
    var sizeRight = end - mid;
    for (i = 0; i < sizeLeft; i++)
        lBuffer[i] = list[start + i];
    for (j = 0; j < sizeRight; j++)
        rBuffer[j] = list[mid + 1 + j];
    i = 0;
    j = 0;
    k = start;
    while (i < sizeLeft && j < sizeRight) {
        if (comparefn(lBuffer[i], rBuffer[j]) <= 0) {
            list[k] = lBuffer[i];
            i++;
        } else {
            list[k] = rBuffer[j];
            j++;
        }
        k++;
    }
    while (i < sizeLeft) {
        list[k] = lBuffer[i];
        i++;
        k++;
    }
    while (j < sizeRight) {
        list[k] = rBuffer[j];
        j++;
        k++;
    }
}
function MoveHoles(sparse, sparseLen, dense, denseLen) {
    for (var i = 0; i < denseLen; i++)
        sparse[i] = dense[i];
    for (var j = denseLen; j < sparseLen; j++)
        delete sparse[j];
}
function MergeSort(array, len, comparefn) {
    var denseList = [];
    var denseLen = 0;
    for (var i = 0; i < len; i++) {
        if (i in array)
            _DefineDataProperty(denseList, denseLen++, array[i]);
    }
    if (denseLen < 1)
        return array;
    if (denseLen < 24) {
        InsertionSort(denseList, 0, denseLen - 1, comparefn);
        MoveHoles(array, len, denseList, denseLen);
        return array;
    }
    var lBuffer = new List();
    var rBuffer = new List();
    var mid, end;
    for (var windowSize = 1; windowSize < denseLen; windowSize = 2 * windowSize) {
        for (var start = 0; start < denseLen - 1; start += 2 * windowSize) {
            ;;
            mid = start + windowSize - 1;
            end = start + 2 * windowSize - 1;
            end = end < denseLen - 1 ? end : denseLen - 1;
            if (mid > end)
                continue;
            Merge(denseList, start, mid, end, lBuffer, rBuffer, comparefn);
        }
    }
    MoveHoles(array, len, denseList, denseLen);
    return array;
}
function Partition(array, from, to, comparefn) {
    ;;
    var medianIndex = from + ((to - from) >> 1);
    var i = from + 1;
    var j = to;
    SwapArrayElements(array, medianIndex, i);
    if (comparefn(array[from], array[to]) > 0)
        SwapArrayElements(array, from, to);
    if (comparefn(array[i], array[to]) > 0)
        SwapArrayElements(array, i, to);
    if (comparefn(array[from], array[i]) > 0)
        SwapArrayElements(array, from, i);
    var pivotIndex = i;
    for (;;) {
        do i++; while (comparefn(array[i], array[pivotIndex]) < 0);
        do j--; while (comparefn(array[j], array[pivotIndex]) > 0);
        if (i > j)
            break;
        SwapArrayElements(array, i, j);
    }
    SwapArrayElements(array, pivotIndex, j);
    return j;
}
function QuickSort(array, len, comparefn) {
    ;;
    var stack = new List();
    var top = 0;
    var start = 0;
    var end = len - 1;
    var pivotIndex, leftLen, rightLen;
    for (;;) {
        if (end - start <= 23) {
            InsertionSort(array, start, end, comparefn);
            if (top < 1)
                break;
            end = stack[--top];
            start = stack[--top];
        } else {
            pivotIndex = Partition(array, start, end, comparefn);
            leftLen = (pivotIndex - 1) - start;
            rightLen = end - (pivotIndex + 1);
            if (rightLen > leftLen) {
                stack[top++] = start;
                stack[top++] = pivotIndex - 1;
                start = pivotIndex + 1;
            } else {
                stack[top++] = pivotIndex + 1;
                stack[top++] = end;
                end = pivotIndex - 1;
            }
        }
    }
    return array;
}
function ViewedArrayBufferIfReified(tarray) {
    ;;
    var buf = UnsafeGetReservedSlot(tarray, 0);
    ;
                                             ;
    return buf;
}
function IsDetachedBuffer(buffer) {
    if (buffer === null)
        return false;
    ;
                                                        ;
    if (IsSharedArrayBuffer(buffer))
        return false;
    var flags = UnsafeGetInt32FromReservedSlot(buffer, 3);
    return (flags & 0x4) !== 0;
}
function TypedArrayLengthMethod() {
    return TypedArrayLength(this);
}
function GetAttachedArrayBuffer(tarray) {
    var buffer = ViewedArrayBufferIfReified(tarray);
    if (IsDetachedBuffer(buffer))
        ThrowTypeError(461);
    return buffer;
}
function GetAttachedArrayBufferMethod() {
    return GetAttachedArrayBuffer(this);
}
function IsTypedArrayEnsuringArrayBuffer(arg) {
    if (IsObject(arg) && IsTypedArray(arg)) {
        GetAttachedArrayBuffer(arg);
        return true;
    }
    callFunction(CallTypedArrayMethodIfWrapped, arg, "GetAttachedArrayBufferMethod");
    return false;
}
function ValidateTypedArray(obj, error) {
    if (IsObject(obj)) {
        if (IsTypedArray(obj)) {
            GetAttachedArrayBuffer(obj);
            return true;
        }
        if (IsPossiblyWrappedTypedArray(obj)) {
            if (PossiblyWrappedTypedArrayHasDetachedBuffer(obj))
                ThrowTypeError(461);
            return false;
        }
    }
    ThrowTypeError(error);
}
function TypedArrayCreateWithLength(constructor, length) {
    var newTypedArray = new constructor(length);
    var isTypedArray = ValidateTypedArray(newTypedArray, 464);
    var len;
    if (isTypedArray) {
        len = TypedArrayLength(newTypedArray);
    } else {
        len = callFunction(CallTypedArrayMethodIfWrapped, newTypedArray,
                           "TypedArrayLengthMethod");
    }
    if (len < length)
        ThrowTypeError(465, length, len);
    return newTypedArray;
}
function TypedArrayCreateWithBuffer(constructor, buffer, byteOffset, length) {
    var newTypedArray = new constructor(buffer, byteOffset, length);
    ValidateTypedArray(newTypedArray, 464);
    return newTypedArray;
}
function TypedArraySpeciesCreateWithLength(exemplar, length) {
    var defaultConstructor = _ConstructorForTypedArray(exemplar);
    var C = SpeciesConstructor(exemplar, defaultConstructor);
    return TypedArrayCreateWithLength(C, length);
}
function TypedArraySpeciesCreateWithBuffer(exemplar, buffer, byteOffset, length) {
    var defaultConstructor = _ConstructorForTypedArray(exemplar);
    var C = SpeciesConstructor(exemplar, defaultConstructor);
    return TypedArrayCreateWithBuffer(C, buffer, byteOffset, length);
}
function TypedArrayCopyWithin(target, start, end = undefined) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, target, start, end,
                            "TypedArrayCopyWithin");
    }
    GetAttachedArrayBuffer(this);
    var obj = this;
    var len = TypedArrayLength(obj);
    ;
                                                                              ;
    var relativeTarget = ToInteger(target);
    var to = relativeTarget < 0 ? std_Math_max(len + relativeTarget, 0)
                                : std_Math_min(relativeTarget, len);
    var relativeStart = ToInteger(start);
    var from = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                 : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len
                                        : ToInteger(end);
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);
    var count = std_Math_min(final - from, len - to);
    ;
                                                    ;
    ;
                                                      ;
    ;
                                                       ;
    if (count > 0)
        MoveTypedArrayElements(obj, to | 0, from | 0, count | 0);
    return obj;
}
function TypedArrayEntries() {
    var O = this;
    IsTypedArrayEnsuringArrayBuffer(O);
    return CreateArrayIterator(O, 2);
}
function TypedArrayEvery(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.every");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        var kValue = O[k];
        var testResult = callContentFunction(callbackfn, T, kValue, k, O);
        if (!testResult)
            return false;
    }
    return true;
}
function TypedArrayFill(value, start = 0, end = undefined) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, value, start, end,
                            "TypedArrayFill");
    }
    var O = this;
    var buffer = GetAttachedArrayBuffer(this);
    var len = TypedArrayLength(O);
    value = ToNumber(value);
    var relativeStart = ToInteger(start);
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len : ToInteger(end);
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);
    if (buffer === null) {
        buffer = ViewedArrayBufferIfReified(O);
    }
    if (IsDetachedBuffer(buffer))
        ThrowTypeError(461);
    for (; k < final; k++) {
        O[k] = value;
    }
    return O;
}
function TypedArrayFilter(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.filter");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    var kept = new List();
    var captured = 0;
    for (var k = 0; k < len; k++) {
        var kValue = O[k];
        var selected = ToBoolean(callContentFunction(callbackfn, T, kValue, k, O));
        if (selected) {
            kept[captured++] = kValue;
        }
    }
    var A = TypedArraySpeciesCreateWithLength(O, captured);
    for (var n = 0; n < captured; n++) {
        A[n] = kept[n];
    }
    return A;
}
function TypedArrayFind(predicate ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.find");
    if (!IsCallable(predicate))
        ThrowTypeError(9, DecompileArg(0, predicate));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        var kValue = O[k];
        if (callContentFunction(predicate, T, kValue, k, O))
            return kValue;
    }
    return undefined;
}
function TypedArrayFindIndex(predicate ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.findIndex");
    if (!IsCallable(predicate))
        ThrowTypeError(9, DecompileArg(0, predicate));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        if (callContentFunction(predicate, T, O[k], k, O))
            return k;
    }
    return -1;
}
function TypedArrayForEach(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "TypedArray.prototype.forEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        callContentFunction(callbackfn, T, O[k], k, O);
    }
    return undefined;
}
function TypedArrayIndexOf(searchElement, fromIndex = 0) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, fromIndex,
                            "TypedArrayIndexOf");
    }
    GetAttachedArrayBuffer(this);
    var O = this;
    var len = TypedArrayLength(O);
    if (len === 0)
        return -1;
    var n = ToInteger(fromIndex) + 0;
    if (n >= len)
        return -1;
    var k;
    if (n >= 0) {
        k = n;
    }
    else {
        k = len + n;
        if (k < 0)
            k = 0;
    }
    for (; k < len; k++) {
        if (O[k] === searchElement)
            return k;
    }
    return -1;
}
function TypedArrayJoin(separator) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, separator, "TypedArrayJoin");
    }
    GetAttachedArrayBuffer(this);
    var O = this;
    var len = TypedArrayLength(O);
    var sep = separator === undefined ? "," : ToString(separator);
    if (len === 0)
        return "";
    var element0 = O[0];
    var R = ToString(element0);
    for (var k = 1; k < len; k++) {
        var S = R + sep;
        var element = O[k];
        var next = ToString(element);
        R = S + next;
    }
    return R;
}
function TypedArrayKeys() {
    var O = this;
    IsTypedArrayEnsuringArrayBuffer(O);
    return CreateArrayIterator(O, 0);
}
function TypedArrayLastIndexOf(searchElement ) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        if (arguments.length > 1) {
            return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, arguments[1],
                                "TypedArrayLastIndexOf");
        }
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement,
                            "TypedArrayLastIndexOf");
    }
    GetAttachedArrayBuffer(this);
    var O = this;
    var len = TypedArrayLength(O);
    if (len === 0)
        return -1;
    var n = arguments.length > 1 ? ToInteger(arguments[1]) + 0 : len - 1;
    var k = n >= 0 ? std_Math_min(n, len - 1) : len + n;
    for (; k >= 0; k--) {
        if (O[k] === searchElement)
            return k;
    }
    return -1;
}
function TypedArrayMap(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.map");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    var A = TypedArraySpeciesCreateWithLength(O, len);
    for (var k = 0; k < len; k++) {
        var mappedValue = callContentFunction(callbackfn, T, O[k], k, O);
        A[k] = mappedValue;
    }
    return A;
}
function TypedArrayReduce(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.reduce");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    if (len === 0 && arguments.length === 1)
        ThrowTypeError(37);
    var k = 0;
    var accumulator = arguments.length > 1 ? arguments[1] : O[k++];
    for (; k < len; k++) {
        accumulator = callContentFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }
    return accumulator;
}
function TypedArrayReduceRight(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.reduceRight");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    if (len === 0 && arguments.length === 1)
        ThrowTypeError(37);
    var k = len - 1;
    var accumulator = arguments.length > 1 ? arguments[1] : O[k--];
    for (; k >= 0; k--) {
        accumulator = callContentFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }
    return accumulator;
}
function TypedArrayReverse() {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, "TypedArrayReverse");
    }
    GetAttachedArrayBuffer(this);
    var O = this;
    var len = TypedArrayLength(O);
    var middle = std_Math_floor(len / 2);
    for (var lower = 0; lower !== middle; lower++) {
        var upper = len - lower - 1;
        var lowerValue = O[lower];
        var upperValue = O[upper];
        O[lower] = upperValue;
        O[upper] = lowerValue;
    }
    return O;
}
function SetFromNonTypedArray(target, array, targetOffset, targetLength, targetBuffer) {
    ;
                                                              ;
    var src = ToObject(array);
    var srcLength = ToLength(src.length);
    var limitOffset = targetOffset + srcLength;
    if (limitOffset > targetLength)
        ThrowRangeError(455);
    var k = 0;
    var isShared = targetBuffer !== null && IsSharedArrayBuffer(targetBuffer);
    while (targetOffset < limitOffset) {
        var kNumber = ToNumber(src[k]);
        if (!isShared) {
            if (targetBuffer === null) {
                targetBuffer = ViewedArrayBufferIfReified(target);
            }
            if (IsDetachedBuffer(targetBuffer))
                ThrowTypeError(461);
        }
        target[targetOffset] = kNumber;
        k++;
        targetOffset++;
    }
    return undefined;
}
function SetFromTypedArray(target, typedArray, targetOffset, targetLength) {
    ;
                                                            ;
    var res = SetFromTypedArrayApproach(target, typedArray, targetOffset,
                                        targetLength | 0);
    ;
                                                                         ;
    if (res == 0)
        return undefined;
    if (res === 2) {
        SetDisjointTypedElements(target, targetOffset | 0, typedArray);
        return undefined;
    }
    SetOverlappingTypedElements(target, targetOffset | 0, typedArray);
    return undefined;
}
function TypedArraySet(overloaded, offset = 0) {
    var target = this;
    if (!IsObject(target) || !IsTypedArray(target)) {
        return callFunction(CallTypedArrayMethodIfWrapped,
                            target, overloaded, offset, "TypedArraySet");
    }
    var targetOffset = ToInteger(offset);
    if (targetOffset < 0)
        ThrowRangeError(460, "2");
    var targetBuffer = GetAttachedArrayBuffer(target);
    var targetLength = TypedArrayLength(target);
    if (IsPossiblyWrappedTypedArray(overloaded))
        return SetFromTypedArray(target, overloaded, targetOffset, targetLength);
    return SetFromNonTypedArray(target, overloaded, targetOffset, targetLength, targetBuffer);
}
function TypedArraySlice(start, end) {
    var O = this;
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, start, end, "TypedArraySlice");
    }
    var buffer = GetAttachedArrayBuffer(O);
    var len = TypedArrayLength(O);
    var relativeStart = ToInteger(start);
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len : ToInteger(end);
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);
    var count = std_Math_max(final - k, 0);
    var A = TypedArraySpeciesCreateWithLength(O, count);
    if (count > 0) {
        var n = 0;
        if (buffer === null) {
            buffer = ViewedArrayBufferIfReified(O);
        }
        if (IsDetachedBuffer(buffer))
            ThrowTypeError(461);
        while (k < final) {
            A[n++] = O[k++];
        }
    }
    return A;
}
function TypedArraySome(callbackfn ) {
    var O = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");
    if (arguments.length === 0)
        ThrowTypeError(39, 0, "%TypedArray%.prototype.some");
    if (!IsCallable(callbackfn))
        ThrowTypeError(9, DecompileArg(0, callbackfn));
    var T = arguments.length > 1 ? arguments[1] : void 0;
    for (var k = 0; k < len; k++) {
        var kValue = O[k];
        var testResult = callContentFunction(callbackfn, T, kValue, k, O);
        if (testResult)
            return true;
    }
    return false;
}
function TypedArrayCompare(x, y) {
    ;
                                      ;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    if (x === 0 && y === 0)
        return ((1 / x) > 0 ? 1 : 0) - ((1 / y) > 0 ? 1 : 0);
    if (Number_isNaN(x))
        return Number_isNaN(y) ? 0 : 1;
    return Number_isNaN(y) ? -1 : 0;
}
function TypedArrayCompareInt(x, y) {
    ;
                                      ;
    ;
                                                   ;
    var diff = x - y;
    if (diff)
        return diff;
    return 0;
}
function TypedArraySort(comparefn) {
    if (comparefn !== undefined) {
        if (!IsCallable(comparefn))
            ThrowTypeError(9, DecompileArg(0, comparefn));
    }
    var obj = this;
    var isTypedArray = IsObject(obj) && IsTypedArray(obj);
    var buffer;
    if (isTypedArray) {
        buffer = GetAttachedArrayBuffer(obj);
    } else {
        buffer = callFunction(CallTypedArrayMethodIfWrapped, obj, "GetAttachedArrayBufferMethod");
    }
    var len;
    if (isTypedArray) {
        len = TypedArrayLength(obj);
    } else {
        len = callFunction(CallTypedArrayMethodIfWrapped, obj, "TypedArrayLengthMethod");
    }
    if (len <= 1)
        return obj;
    if (comparefn === undefined) {
        if (IsUint8TypedArray(obj)) {
            return CountingSort(obj, len, false , TypedArrayCompareInt);
        } else if (IsInt8TypedArray(obj)) {
            return CountingSort(obj, len, true , TypedArrayCompareInt);
        } else if (IsUint16TypedArray(obj)) {
            return RadixSort(obj, len, buffer, 2 , false , false , TypedArrayCompareInt);
        } else if (IsInt16TypedArray(obj)) {
            return RadixSort(obj, len, buffer, 2 , true , false , TypedArrayCompareInt);
        } else if (IsUint32TypedArray(obj)) {
            return RadixSort(obj, len, buffer, 4 , false , false , TypedArrayCompareInt);
        } else if (IsInt32TypedArray(obj)) {
            return RadixSort(obj, len, buffer, 4 , true , false , TypedArrayCompareInt);
        } else if (IsFloat32TypedArray(obj)) {
            return RadixSort(obj, len, buffer, 4 , true , true , TypedArrayCompare);
        }
        return QuickSort(obj, len, TypedArrayCompare);
    }
    var wrappedCompareFn = function(x, y) {
        var v = comparefn(x, y);
        var length;
        if (isTypedArray) {
            length = TypedArrayLength(obj);
        } else {
            length = callFunction(CallTypedArrayMethodIfWrapped, obj, "TypedArrayLengthMethod");
        }
        if (length === 0) {
            ;
                                                                                               ;
            ThrowTypeError(461);
        }
        return v;
    };
    return QuickSort(obj, len, wrappedCompareFn);
}
function TypedArrayToLocaleString(locales = undefined, options = undefined) {
    var array = this;
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(array);
    var len;
    if (isTypedArray)
        len = TypedArrayLength(array);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, array, "TypedArrayLengthMethod");
    if (len === 0)
        return "";
    var firstElement = array[0];
    var R = ToString(callContentFunction(firstElement.toLocaleString, firstElement));
    var separator = ",";
    for (var k = 1; k < len; k++) {
        var S = R + separator;
        var nextElement = array[k];
        R = ToString(callContentFunction(nextElement.toLocaleString, nextElement));
        R = S + R;
    }
    return R;
}
function TypedArraySubarray(begin, end) {
    var obj = this;
    if (!IsObject(obj) || !IsTypedArray(obj)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, begin, end,
                            "TypedArraySubarray");
    }
    var buffer = TypedArrayBuffer(obj);
    var srcLength = TypedArrayLength(obj);
    var srcByteOffset = TypedArrayByteOffset(obj);
    var relativeBegin = ToInteger(begin);
    var beginIndex = relativeBegin < 0 ? std_Math_max(srcLength + relativeBegin, 0)
                                       : std_Math_min(relativeBegin, srcLength);
    var relativeEnd = end === undefined ? srcLength : ToInteger(end);
    var endIndex = relativeEnd < 0 ? std_Math_max(srcLength + relativeEnd, 0)
                                   : std_Math_min(relativeEnd, srcLength);
    var newLength = std_Math_max(endIndex - beginIndex, 0);
    var elementShift = TypedArrayElementShift(obj);
    var beginByteOffset = srcByteOffset + (beginIndex << elementShift);
    return TypedArraySpeciesCreateWithBuffer(obj, buffer, beginByteOffset, newLength);
}
function TypedArrayValues() {
    var O = this;
    IsTypedArrayEnsuringArrayBuffer(O);
    return CreateArrayIterator(O, 1);
}
_SetCanonicalName(TypedArrayValues, "values");
function TypedArrayIncludes(searchElement, fromIndex = 0) {
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement,
                            fromIndex, "TypedArrayIncludes");
    }
    GetAttachedArrayBuffer(this);
    var O = this;
    var len = TypedArrayLength(O);
    if (len === 0)
        return false;
    var n = ToInteger(fromIndex);
    var k;
    if (n >= 0) {
        k = n;
    }
    else {
        k = len + n;
        if (k < 0)
            k = 0;
    }
    while (k < len) {
        if (SameValueZero(searchElement, O[k]))
            return true;
        k++;
    }
    return false;
}
function TypedArrayStaticFrom(source, mapfn = undefined, thisArg = undefined) {
    var C = this;
    if (!IsConstructor(C))
        ThrowTypeError(10, typeof C);
    var mapping;
    if (mapfn !== undefined) {
        if (!IsCallable(mapfn))
            ThrowTypeError(9, DecompileArg(1, mapfn));
        mapping = true;
    } else {
        mapping = false;
    }
    var T = thisArg;
    var usingIterator = source[std_iterator];
    if (usingIterator !== undefined && usingIterator !== null) {
        if (!IsCallable(usingIterator))
            ThrowTypeError(53, DecompileArg(0, source));
        var values = IterableToList(source, usingIterator);
        var len = values.length;
        var targetObj = TypedArrayCreateWithLength(C, len);
        for (var k = 0; k < len; k++) {
            var kValue = values[k];
            var mappedValue = mapping ? callContentFunction(mapfn, T, kValue, k) : kValue;
            targetObj[k] = mappedValue;
        }
        return targetObj;
    }
    var arrayLike = ToObject(source);
    var len = ToLength(arrayLike.length);
    var targetObj = TypedArrayCreateWithLength(C, len);
    for (var k = 0; k < len; k++) {
        var kValue = arrayLike[k];
        var mappedValue = mapping ? callContentFunction(mapfn, T, kValue, k) : kValue;
        targetObj[k] = mappedValue;
    }
    return targetObj;
}
function TypedArrayStaticOf( ) {
    var len = arguments.length;
    var items = arguments;
    var C = this;
    if (!IsConstructor(C))
        ThrowTypeError(10, typeof C);
    var newObj = TypedArrayCreateWithLength(C, len);
    for (var k = 0; k < len; k++)
        newObj[k] = items[k];
    return newObj;
}
function TypedArraySpecies() {
    return this;
}
_SetCanonicalName(TypedArraySpecies, "get [Symbol.species]");
function TypedArrayToStringTag() {
    var O = this;
    if (!IsObject(O) || !IsTypedArray(O))
        return undefined;
    return _NameForTypedArray(O);
}
_SetCanonicalName(TypedArrayToStringTag, "get [Symbol.toStringTag]");
function IterableToList(items, method) {
    ;;
    var iterator = callContentFunction(method, items);
    if (!IsObject(iterator))
        ThrowTypeError(55);
    var nextMethod = iterator.next;
    var values = [];
    var i = 0;
    while (true) {
        var next = callContentFunction(nextMethod, iterator);
        if (!IsObject(next))
            ThrowTypeError(56, "next");
        if (next.done)
            break;
        _DefineDataProperty(values, i++, next.value);
    }
    return values;
}
function ArrayBufferSlice(start, end) {
    var O = this;
    if (!IsObject(O) || !IsArrayBuffer(O)) {
        return callFunction(CallArrayBufferMethodIfWrapped, O, start, end,
                            "ArrayBufferSlice");
    }
    if (IsDetachedBuffer(O))
        ThrowTypeError(461);
    var len = ArrayBufferByteLength(O);
    var relativeStart = ToInteger(start);
    var first = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                  : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len
                                        : ToInteger(end);
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);
    var newLen = std_Math_max(final - first, 0);
    var ctor = SpeciesConstructor(O, GetBuiltinConstructor("ArrayBuffer"));
    var new_ = new ctor(newLen);
    var isWrapped = false;
    if (IsArrayBuffer(new_)) {
        if (IsDetachedBuffer(new_))
            ThrowTypeError(461);
    } else {
        if (!IsWrappedArrayBuffer(new_))
            ThrowTypeError(456);
        isWrapped = true;
        if (callFunction(CallArrayBufferMethodIfWrapped, new_, "IsDetachedBufferThis"))
            ThrowTypeError(461);
    }
    if (new_ === O)
        ThrowTypeError(457);
    var actualLen = PossiblyWrappedArrayBufferByteLength(new_);
    if (actualLen < newLen)
        ThrowTypeError(458, newLen, actualLen);
    if (IsDetachedBuffer(O))
        ThrowTypeError(461);
    ArrayBufferCopyData(new_, 0, O, first | 0, newLen | 0, isWrapped);
    return new_;
}
function IsDetachedBufferThis() {
  return IsDetachedBuffer(this);
}
function ArrayBufferSpecies() {
    return this;
}
_SetCanonicalName(ArrayBufferSpecies, "get [Symbol.species]");
function SharedArrayBufferSpecies() {
    return this;
}
_SetCanonicalName(SharedArrayBufferSpecies, "get [Symbol.species]");
function SharedArrayBufferSlice(start, end) {
    var O = this;
    if (!IsObject(O) || !IsSharedArrayBuffer(O)) {
        return callFunction(CallSharedArrayBufferMethodIfWrapped, O, start, end,
                            "SharedArrayBufferSlice");
    }
    var len = SharedArrayBufferByteLength(O);
    var relativeStart = ToInteger(start);
    var first = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                  : std_Math_min(relativeStart, len);
    var relativeEnd = end === undefined ? len
                                        : ToInteger(end);
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);
    var newLen = std_Math_max(final - first, 0);
    var ctor = SpeciesConstructor(O, GetBuiltinConstructor("SharedArrayBuffer"));
    var new_ = new ctor(newLen);
    var isWrapped = false;
    if (!IsSharedArrayBuffer(new_)) {
        if (!IsWrappedSharedArrayBuffer(new_))
            ThrowTypeError(467);
        isWrapped = true;
    }
    if (new_ === O)
        ThrowTypeError(468);
    if (SharedArrayBuffersMemorySame(new_, O))
        ThrowTypeError(468);
    var actualLen = PossiblyWrappedSharedArrayBufferByteLength(new_);
    if (actualLen < newLen)
        ThrowTypeError(469, newLen, actualLen);
    SharedArrayBufferCopyData(new_, 0, O, first | 0, newLen | 0, isWrapped);
    return new_;
}
function TypedObjectGet(descr, typedObj, offset) {
  ;
                                            ;
  if (!TypedObjectIsAttached(typedObj))
    ThrowTypeError(449);
  switch (UnsafeGetInt32FromReservedSlot(descr, 0)) {
  case 1:
    return TypedObjectGetScalar(descr, typedObj, offset);
  case 2:
    return TypedObjectGetReference(descr, typedObj, offset);
  case 5:
    return TypedObjectGetSimd(descr, typedObj, offset);
  case 4:
  case 3:
    return TypedObjectGetDerived(descr, typedObj, offset);
  }
  ;;
  return undefined;
}
function TypedObjectGetDerived(descr, typedObj, offset) {
  ;
                                              ;
  return NewDerivedTypedObject(descr, typedObj, offset);
}
function TypedObjectGetDerivedIf(descr, typedObj, offset, cond) {
  return (cond ? TypedObjectGetDerived(descr, typedObj, offset) : undefined);
}
function TypedObjectGetOpaque(descr, typedObj, offset) {
  ;
                                              ;
  var opaqueTypedObj = NewOpaqueTypedObject(descr);
  AttachTypedObject(opaqueTypedObj, typedObj, offset);
  return opaqueTypedObj;
}
function TypedObjectGetOpaqueIf(descr, typedObj, offset, cond) {
  return (cond ? TypedObjectGetOpaque(descr, typedObj, offset) : undefined);
}
function TypedObjectGetScalar(descr, typedObj, offset) {
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  switch (type) {
  case 0:
    return Load_int8(typedObj, offset);
  case 1:
  case 8:
    return Load_uint8(typedObj, offset);
  case 2:
    return Load_int16(typedObj, offset);
  case 3:
    return Load_uint16(typedObj, offset);
  case 4:
    return Load_int32(typedObj, offset);
  case 5:
    return Load_uint32(typedObj, offset);
  case 6:
    return Load_float32(typedObj, offset);
  case 7:
    return Load_float64(typedObj, offset);
  }
  ;;
  return undefined;
}
function TypedObjectGetReference(descr, typedObj, offset) {
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  switch (type) {
  case 0:
    return Load_Any(typedObj, offset);
  case 1:
    return Load_Object(typedObj, offset);
  case 2:
    return Load_string(typedObj, offset);
  }
  ;;
  return undefined;
}
function TypedObjectGetSimd(descr, typedObj, offset) {
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  var simdTypeDescr = GetSimdTypeDescr(type);
  switch (type) {
  case 6:
    var x = Load_float32(typedObj, offset + 0);
    var y = Load_float32(typedObj, offset + 4);
    var z = Load_float32(typedObj, offset + 8);
    var w = Load_float32(typedObj, offset + 12);
    return simdTypeDescr(x, y, z, w);
  case 7:
    var x = Load_float64(typedObj, offset + 0);
    var y = Load_float64(typedObj, offset + 8);
    return simdTypeDescr(x, y);
  case 0:
    var s0 = Load_int8(typedObj, offset + 0);
    var s1 = Load_int8(typedObj, offset + 1);
    var s2 = Load_int8(typedObj, offset + 2);
    var s3 = Load_int8(typedObj, offset + 3);
    var s4 = Load_int8(typedObj, offset + 4);
    var s5 = Load_int8(typedObj, offset + 5);
    var s6 = Load_int8(typedObj, offset + 6);
    var s7 = Load_int8(typedObj, offset + 7);
    var s8 = Load_int8(typedObj, offset + 8);
    var s9 = Load_int8(typedObj, offset + 9);
    var s10 = Load_int8(typedObj, offset + 10);
    var s11 = Load_int8(typedObj, offset + 11);
    var s12 = Load_int8(typedObj, offset + 12);
    var s13 = Load_int8(typedObj, offset + 13);
    var s14 = Load_int8(typedObj, offset + 14);
    var s15 = Load_int8(typedObj, offset + 15);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15);
  case 1:
    var s0 = Load_int16(typedObj, offset + 0);
    var s1 = Load_int16(typedObj, offset + 2);
    var s2 = Load_int16(typedObj, offset + 4);
    var s3 = Load_int16(typedObj, offset + 6);
    var s4 = Load_int16(typedObj, offset + 8);
    var s5 = Load_int16(typedObj, offset + 10);
    var s6 = Load_int16(typedObj, offset + 12);
    var s7 = Load_int16(typedObj, offset + 14);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7);
  case 2:
    var x = Load_int32(typedObj, offset + 0);
    var y = Load_int32(typedObj, offset + 4);
    var z = Load_int32(typedObj, offset + 8);
    var w = Load_int32(typedObj, offset + 12);
    return simdTypeDescr(x, y, z, w);
  case 3:
    var s0 = Load_uint8(typedObj, offset + 0);
    var s1 = Load_uint8(typedObj, offset + 1);
    var s2 = Load_uint8(typedObj, offset + 2);
    var s3 = Load_uint8(typedObj, offset + 3);
    var s4 = Load_uint8(typedObj, offset + 4);
    var s5 = Load_uint8(typedObj, offset + 5);
    var s6 = Load_uint8(typedObj, offset + 6);
    var s7 = Load_uint8(typedObj, offset + 7);
    var s8 = Load_uint8(typedObj, offset + 8);
    var s9 = Load_uint8(typedObj, offset + 9);
    var s10 = Load_uint8(typedObj, offset + 10);
    var s11 = Load_uint8(typedObj, offset + 11);
    var s12 = Load_uint8(typedObj, offset + 12);
    var s13 = Load_uint8(typedObj, offset + 13);
    var s14 = Load_uint8(typedObj, offset + 14);
    var s15 = Load_uint8(typedObj, offset + 15);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15);
  case 4:
    var s0 = Load_uint16(typedObj, offset + 0);
    var s1 = Load_uint16(typedObj, offset + 2);
    var s2 = Load_uint16(typedObj, offset + 4);
    var s3 = Load_uint16(typedObj, offset + 6);
    var s4 = Load_uint16(typedObj, offset + 8);
    var s5 = Load_uint16(typedObj, offset + 10);
    var s6 = Load_uint16(typedObj, offset + 12);
    var s7 = Load_uint16(typedObj, offset + 14);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7);
  case 5:
    var x = Load_uint32(typedObj, offset + 0);
    var y = Load_uint32(typedObj, offset + 4);
    var z = Load_uint32(typedObj, offset + 8);
    var w = Load_uint32(typedObj, offset + 12);
    return simdTypeDescr(x, y, z, w);
  case 8:
    var s0 = Load_int8(typedObj, offset + 0);
    var s1 = Load_int8(typedObj, offset + 1);
    var s2 = Load_int8(typedObj, offset + 2);
    var s3 = Load_int8(typedObj, offset + 3);
    var s4 = Load_int8(typedObj, offset + 4);
    var s5 = Load_int8(typedObj, offset + 5);
    var s6 = Load_int8(typedObj, offset + 6);
    var s7 = Load_int8(typedObj, offset + 7);
    var s8 = Load_int8(typedObj, offset + 8);
    var s9 = Load_int8(typedObj, offset + 9);
    var s10 = Load_int8(typedObj, offset + 10);
    var s11 = Load_int8(typedObj, offset + 11);
    var s12 = Load_int8(typedObj, offset + 12);
    var s13 = Load_int8(typedObj, offset + 13);
    var s14 = Load_int8(typedObj, offset + 14);
    var s15 = Load_int8(typedObj, offset + 15);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15);
  case 9:
    var s0 = Load_int16(typedObj, offset + 0);
    var s1 = Load_int16(typedObj, offset + 2);
    var s2 = Load_int16(typedObj, offset + 4);
    var s3 = Load_int16(typedObj, offset + 6);
    var s4 = Load_int16(typedObj, offset + 8);
    var s5 = Load_int16(typedObj, offset + 10);
    var s6 = Load_int16(typedObj, offset + 12);
    var s7 = Load_int16(typedObj, offset + 14);
    return simdTypeDescr(s0, s1, s2, s3, s4, s5, s6, s7);
  case 10:
    var x = Load_int32(typedObj, offset + 0);
    var y = Load_int32(typedObj, offset + 4);
    var z = Load_int32(typedObj, offset + 8);
    var w = Load_int32(typedObj, offset + 12);
    return simdTypeDescr(x, y, z, w);
  case 11:
    var x = Load_int32(typedObj, offset + 0);
    var y = Load_int32(typedObj, offset + 8);
    return simdTypeDescr(x, y);
  }
  ;;
  return undefined;
}
function TypedObjectSet(descr, typedObj, offset, name, fromValue) {
  if (!TypedObjectIsAttached(typedObj))
    ThrowTypeError(449);
  switch (UnsafeGetInt32FromReservedSlot(descr, 0)) {
  case 1:
    TypedObjectSetScalar(descr, typedObj, offset, fromValue);
    return;
  case 2:
    TypedObjectSetReference(descr, typedObj, offset, name, fromValue);
    return;
  case 5:
    TypedObjectSetSimd(descr, typedObj, offset, fromValue);
    return;
  case 4:
    var length = ((UnsafeGetInt32FromReservedSlot(descr, 9)) | 0);
    if (TypedObjectSetArray(descr, length, typedObj, offset, fromValue))
      return;
    break;
  case 3:
    if (!IsObject(fromValue))
      break;
    var fieldNames = UnsafeGetObjectFromReservedSlot(descr, 8);
    var fieldDescrs = UnsafeGetObjectFromReservedSlot(descr, 9);
    var fieldOffsets = UnsafeGetObjectFromReservedSlot(descr, 10);
    for (var i = 0; i < fieldNames.length; i++) {
      var fieldName = fieldNames[i];
      var fieldDescr = fieldDescrs[i];
      var fieldOffset = fieldOffsets[i];
      var fieldValue = fromValue[fieldName];
      TypedObjectSet(fieldDescr, typedObj, offset + fieldOffset, fieldName, fieldValue);
    }
    return;
  }
  ThrowTypeError(11,
                 typeof(fromValue),
                 UnsafeGetStringFromReservedSlot(descr, 1));
}
function TypedObjectSetArray(descr, length, typedObj, offset, fromValue) {
  if (!IsObject(fromValue))
    return false;
  if (fromValue.length !== length)
    return false;
  if (length > 0) {
    var elemDescr = UnsafeGetObjectFromReservedSlot(descr, 8);
    var elemSize = UnsafeGetInt32FromReservedSlot(elemDescr, 3);
    var elemOffset = offset;
    for (var i = 0; i < length; i++) {
      TypedObjectSet(elemDescr, typedObj, elemOffset, null, fromValue[i]);
      elemOffset += elemSize;
    }
  }
  return true;
}
function TypedObjectSetScalar(descr, typedObj, offset, fromValue) {
  ;
                                           ;
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  switch (type) {
  case 0:
    return Store_int8(typedObj, offset,
                      ((fromValue) | 0) & 0xFF);
  case 1:
    return Store_uint8(typedObj, offset,
                       ((fromValue) >>> 0) & 0xFF);
  case 8:
    var v = ClampToUint8(+fromValue);
    return Store_int8(typedObj, offset, v);
  case 2:
    return Store_int16(typedObj, offset,
                       ((fromValue) | 0) & 0xFFFF);
  case 3:
    return Store_uint16(typedObj, offset,
                        ((fromValue) >>> 0) & 0xFFFF);
  case 4:
    return Store_int32(typedObj, offset,
                       ((fromValue) | 0));
  case 5:
    return Store_uint32(typedObj, offset,
                        ((fromValue) >>> 0));
  case 6:
    return Store_float32(typedObj, offset, +fromValue);
  case 7:
    return Store_float64(typedObj, offset, +fromValue);
  }
  ;;
  return undefined;
}
function TypedObjectSetReference(descr, typedObj, offset, name, fromValue) {
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  switch (type) {
  case 0:
    return Store_Any(typedObj, offset, name, fromValue);
  case 1:
    var value = (fromValue === null ? fromValue : ToObject(fromValue));
    return Store_Object(typedObj, offset, name, value);
  case 2:
    return Store_string(typedObj, offset, name, ToString(fromValue));
  }
  ;;
  return undefined;
}
function TypedObjectSetSimd(descr, typedObj, offset, fromValue) {
  if (!IsObject(fromValue) || !ObjectIsTypedObject(fromValue))
    ThrowTypeError(11,
                   typeof(fromValue),
                   UnsafeGetStringFromReservedSlot(descr, 1));
  if (!DescrsEquiv(descr, TypedObjectTypeDescr(fromValue)))
    ThrowTypeError(11,
                   typeof(fromValue),
                   UnsafeGetStringFromReservedSlot(descr, 1));
  var type = UnsafeGetInt32FromReservedSlot(descr, 8);
  switch (type) {
    case 6:
      Store_float32(typedObj, offset + 0, Load_float32(fromValue, 0));
      Store_float32(typedObj, offset + 4, Load_float32(fromValue, 4));
      Store_float32(typedObj, offset + 8, Load_float32(fromValue, 8));
      Store_float32(typedObj, offset + 12, Load_float32(fromValue, 12));
      break;
    case 7:
      Store_float64(typedObj, offset + 0, Load_float64(fromValue, 0));
      Store_float64(typedObj, offset + 8, Load_float64(fromValue, 8));
      break;
    case 0:
    case 8:
      Store_int8(typedObj, offset + 0, Load_int8(fromValue, 0));
      Store_int8(typedObj, offset + 1, Load_int8(fromValue, 1));
      Store_int8(typedObj, offset + 2, Load_int8(fromValue, 2));
      Store_int8(typedObj, offset + 3, Load_int8(fromValue, 3));
      Store_int8(typedObj, offset + 4, Load_int8(fromValue, 4));
      Store_int8(typedObj, offset + 5, Load_int8(fromValue, 5));
      Store_int8(typedObj, offset + 6, Load_int8(fromValue, 6));
      Store_int8(typedObj, offset + 7, Load_int8(fromValue, 7));
      Store_int8(typedObj, offset + 8, Load_int8(fromValue, 8));
      Store_int8(typedObj, offset + 9, Load_int8(fromValue, 9));
      Store_int8(typedObj, offset + 10, Load_int8(fromValue, 10));
      Store_int8(typedObj, offset + 11, Load_int8(fromValue, 11));
      Store_int8(typedObj, offset + 12, Load_int8(fromValue, 12));
      Store_int8(typedObj, offset + 13, Load_int8(fromValue, 13));
      Store_int8(typedObj, offset + 14, Load_int8(fromValue, 14));
      Store_int8(typedObj, offset + 15, Load_int8(fromValue, 15));
      break;
    case 1:
    case 9:
      Store_int16(typedObj, offset + 0, Load_int16(fromValue, 0));
      Store_int16(typedObj, offset + 2, Load_int16(fromValue, 2));
      Store_int16(typedObj, offset + 4, Load_int16(fromValue, 4));
      Store_int16(typedObj, offset + 6, Load_int16(fromValue, 6));
      Store_int16(typedObj, offset + 8, Load_int16(fromValue, 8));
      Store_int16(typedObj, offset + 10, Load_int16(fromValue, 10));
      Store_int16(typedObj, offset + 12, Load_int16(fromValue, 12));
      Store_int16(typedObj, offset + 14, Load_int16(fromValue, 14));
      break;
    case 2:
    case 10:
    case 11:
      Store_int32(typedObj, offset + 0, Load_int32(fromValue, 0));
      Store_int32(typedObj, offset + 4, Load_int32(fromValue, 4));
      Store_int32(typedObj, offset + 8, Load_int32(fromValue, 8));
      Store_int32(typedObj, offset + 12, Load_int32(fromValue, 12));
      break;
    case 3:
      Store_uint8(typedObj, offset + 0, Load_uint8(fromValue, 0));
      Store_uint8(typedObj, offset + 1, Load_uint8(fromValue, 1));
      Store_uint8(typedObj, offset + 2, Load_uint8(fromValue, 2));
      Store_uint8(typedObj, offset + 3, Load_uint8(fromValue, 3));
      Store_uint8(typedObj, offset + 4, Load_uint8(fromValue, 4));
      Store_uint8(typedObj, offset + 5, Load_uint8(fromValue, 5));
      Store_uint8(typedObj, offset + 6, Load_uint8(fromValue, 6));
      Store_uint8(typedObj, offset + 7, Load_uint8(fromValue, 7));
      Store_uint8(typedObj, offset + 8, Load_uint8(fromValue, 8));
      Store_uint8(typedObj, offset + 9, Load_uint8(fromValue, 9));
      Store_uint8(typedObj, offset + 10, Load_uint8(fromValue, 10));
      Store_uint8(typedObj, offset + 11, Load_uint8(fromValue, 11));
      Store_uint8(typedObj, offset + 12, Load_uint8(fromValue, 12));
      Store_uint8(typedObj, offset + 13, Load_uint8(fromValue, 13));
      Store_uint8(typedObj, offset + 14, Load_uint8(fromValue, 14));
      Store_uint8(typedObj, offset + 15, Load_uint8(fromValue, 15));
      break;
    case 4:
      Store_uint16(typedObj, offset + 0, Load_uint16(fromValue, 0));
      Store_uint16(typedObj, offset + 2, Load_uint16(fromValue, 2));
      Store_uint16(typedObj, offset + 4, Load_uint16(fromValue, 4));
      Store_uint16(typedObj, offset + 6, Load_uint16(fromValue, 6));
      Store_uint16(typedObj, offset + 8, Load_uint16(fromValue, 8));
      Store_uint16(typedObj, offset + 10, Load_uint16(fromValue, 10));
      Store_uint16(typedObj, offset + 12, Load_uint16(fromValue, 12));
      Store_uint16(typedObj, offset + 14, Load_uint16(fromValue, 14));
      break;
    case 5:
      Store_uint32(typedObj, offset + 0, Load_uint32(fromValue, 0));
      Store_uint32(typedObj, offset + 4, Load_uint32(fromValue, 4));
      Store_uint32(typedObj, offset + 8, Load_uint32(fromValue, 8));
      Store_uint32(typedObj, offset + 12, Load_uint32(fromValue, 12));
      break;
    default:
      ;;
  }
}
function ConvertAndCopyTo(destDescr,
                          destTypedObj,
                          destOffset,
                          fieldName,
                          fromValue)
{
  ;
                                          ;
  ;
                                               ;
  if (!TypedObjectIsAttached(destTypedObj))
    ThrowTypeError(449);
  TypedObjectSet(destDescr, destTypedObj, destOffset, fieldName, fromValue);
}
function Reify(sourceDescr,
               sourceTypedObj,
               sourceOffset) {
  ;
                               ;
  ;
                                    ;
  if (!TypedObjectIsAttached(sourceTypedObj))
    ThrowTypeError(449);
  return TypedObjectGet(sourceDescr, sourceTypedObj, sourceOffset);
}
function TypeDescrEquivalent(otherDescr) {
  if (!IsObject(this) || !ObjectIsTypeDescr(this))
    ThrowTypeError(447);
  if (!IsObject(otherDescr) || !ObjectIsTypeDescr(otherDescr))
    ThrowTypeError(447);
  return DescrsEquiv(this, otherDescr);
}
function TypedObjectArrayRedimension(newArrayType) {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(447);
  if (!IsObject(newArrayType) || !ObjectIsTypeDescr(newArrayType))
    ThrowTypeError(447);
  var oldArrayType = TypedObjectTypeDescr(this);
  var oldElementType = oldArrayType;
  var oldElementCount = 1;
  if (UnsafeGetInt32FromReservedSlot(oldArrayType, 0) != 4)
    ThrowTypeError(447);
  while (UnsafeGetInt32FromReservedSlot(oldElementType, 0) === 4) {
    oldElementCount *= oldElementType.length;
    oldElementType = oldElementType.elementType;
  }
  var newElementType = newArrayType;
  var newElementCount = 1;
  while (UnsafeGetInt32FromReservedSlot(newElementType, 0) == 4) {
    newElementCount *= newElementType.length;
    newElementType = newElementType.elementType;
  }
  if (oldElementCount !== newElementCount) {
    ThrowTypeError(447);
  }
  if (!DescrsEquiv(oldElementType, newElementType)) {
    ThrowTypeError(447);
  }
  ;
                                      ;
  return NewDerivedTypedObject(newArrayType, this, 0);
}
function SimdProtoString(type) {
  switch (type) {
  case 0:
    return "Int8x16";
  case 1:
    return "Int16x8";
  case 2:
    return "Int32x4";
  case 3:
    return "Uint8x16";
  case 4:
    return "Uint16x8";
  case 5:
    return "Uint32x4";
  case 6:
    return "Float32x4";
  case 7:
    return "Float64x2";
  case 8:
    return "Bool8x16";
  case 9:
    return "Bool16x8";
  case 10:
    return "Bool32x4";
  case 11:
    return "Bool64x2";
  }
  ;;
  return undefined;
}
function SimdTypeToLength(type) {
  switch (type) {
  case 0:
  case 8:
    return 16;
  case 1:
  case 9:
    return 8;
  case 2:
  case 6:
  case 10:
    return 4;
  case 7:
  case 11:
    return 2;
  }
  ;;
  return undefined;
}
function SimdValueOf() {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(3, "SIMD", "valueOf", typeof this);
  var descr = TypedObjectTypeDescr(this);
  if (UnsafeGetInt32FromReservedSlot(descr, 0) != 5)
    ThrowTypeError(3, "SIMD", "valueOf", typeof this);
  ThrowTypeError(453);
}
function SimdToSource() {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(3, "SIMD.*", "toSource", typeof this);
  var descr = TypedObjectTypeDescr(this);
  if (UnsafeGetInt32FromReservedSlot(descr, 0) != 5)
    ThrowTypeError(3, "SIMD.*", "toSource", typeof this);
  return SimdFormatString(descr, this);
}
function SimdToString() {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(3, "SIMD.*", "toString", typeof this);
  var descr = TypedObjectTypeDescr(this);
  if (UnsafeGetInt32FromReservedSlot(descr, 0) != 5)
    ThrowTypeError(3, "SIMD.*", "toString", typeof this);
  return SimdFormatString(descr, this);
}
function SimdFormatString(descr, typedObj) {
  var typerepr = UnsafeGetInt32FromReservedSlot(descr, 8);
  var protoString = SimdProtoString(typerepr);
  switch (typerepr) {
      case 0: {
          var s1 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 7);
          var s9 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 8);
          var s10 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 9);
          var s11 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 10);
          var s12 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 11);
          var s13 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 12);
          var s14 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 13);
          var s15 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 14);
          var s16 = callFunction(std_SIMD_Int8x16_extractLane, null, typedObj, 15);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8}, ${s9}, ${s10}, ${s11}, ${s12}, ${s13}, ${s14}, ${s15}, ${s16})`;
      }
      case 1: {
          var s1 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Int16x8_extractLane, null, typedObj, 7);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8})`;
      }
      case 2: {
          var x = callFunction(std_SIMD_Int32x4_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Int32x4_extractLane, null, typedObj, 1);
          var z = callFunction(std_SIMD_Int32x4_extractLane, null, typedObj, 2);
          var w = callFunction(std_SIMD_Int32x4_extractLane, null, typedObj, 3);
          return `SIMD.${protoString}(${x}, ${y}, ${z}, ${w})`;
      }
      case 3: {
          var s1 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 7);
          var s9 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 8);
          var s10 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 9);
          var s11 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 10);
          var s12 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 11);
          var s13 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 12);
          var s14 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 13);
          var s15 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 14);
          var s16 = callFunction(std_SIMD_Uint8x16_extractLane, null, typedObj, 15);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8}, ${s9}, ${s10}, ${s11}, ${s12}, ${s13}, ${s14}, ${s15}, ${s16})`;
      }
      case 4: {
          var s1 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Uint16x8_extractLane, null, typedObj, 7);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8})`;
      }
      case 5: {
          var x = callFunction(std_SIMD_Uint32x4_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Uint32x4_extractLane, null, typedObj, 1);
          var z = callFunction(std_SIMD_Uint32x4_extractLane, null, typedObj, 2);
          var w = callFunction(std_SIMD_Uint32x4_extractLane, null, typedObj, 3);
          return `SIMD.${protoString}(${x}, ${y}, ${z}, ${w})`;
      }
      case 6: {
          var x = callFunction(std_SIMD_Float32x4_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Float32x4_extractLane, null, typedObj, 1);
          var z = callFunction(std_SIMD_Float32x4_extractLane, null, typedObj, 2);
          var w = callFunction(std_SIMD_Float32x4_extractLane, null, typedObj, 3);
          return `SIMD.${protoString}(${x}, ${y}, ${z}, ${w})`;
      }
      case 7: {
          var x = callFunction(std_SIMD_Float64x2_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Float64x2_extractLane, null, typedObj, 1);
          return `SIMD.${protoString}(${x}, ${y})`;
      }
      case 8: {
          var s1 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 7);
          var s9 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 8);
          var s10 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 9);
          var s11 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 10);
          var s12 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 11);
          var s13 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 12);
          var s14 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 13);
          var s15 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 14);
          var s16 = callFunction(std_SIMD_Bool8x16_extractLane, null, typedObj, 15);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8}, ${s9}, ${s10}, ${s11}, ${s12}, ${s13}, ${s14}, ${s15}, ${s16})`;
      }
      case 9: {
          var s1 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 0);
          var s2 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 1);
          var s3 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 2);
          var s4 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 3);
          var s5 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 4);
          var s6 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 5);
          var s7 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 6);
          var s8 = callFunction(std_SIMD_Bool16x8_extractLane, null, typedObj, 7);
          return `SIMD.${protoString}(${s1}, ${s2}, ${s3}, ${s4}, ${s5}, ${s6}, ${s7}, ${s8})`;
      }
      case 10: {
          var x = callFunction(std_SIMD_Bool32x4_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Bool32x4_extractLane, null, typedObj, 1);
          var z = callFunction(std_SIMD_Bool32x4_extractLane, null, typedObj, 2);
          var w = callFunction(std_SIMD_Bool32x4_extractLane, null, typedObj, 3);
          return `SIMD.${protoString}(${x}, ${y}, ${z}, ${w})`;
      }
      case 11: {
          var x = callFunction(std_SIMD_Bool64x2_extractLane, null, typedObj, 0);
          var y = callFunction(std_SIMD_Bool64x2_extractLane, null, typedObj, 1);
          return `SIMD.${protoString}(${x}, ${y})`;
      }
  }
  ;;
  return "?";
}
function DescrsEquiv(descr1, descr2) {
  ;;
  ;;
  return UnsafeGetStringFromReservedSlot(descr1, 1) === UnsafeGetStringFromReservedSlot(descr2, 1);
}
function DescrToSource() {
  if (!IsObject(this) || !ObjectIsTypeDescr(this))
    ThrowTypeError(3, "Type", "toSource", "value");
  return UnsafeGetStringFromReservedSlot(this, 1);
}
function ArrayShorthand(...dims) {
  if (!IsObject(this) || !ObjectIsTypeDescr(this))
    ThrowTypeError(447);
  var AT = GetTypedObjectModule().ArrayType;
  if (dims.length == 0)
    ThrowTypeError(447);
  var accum = this;
  for (var i = dims.length - 1; i >= 0; i--)
    accum = new AT(accum, dims[i]);
  return accum;
}
function StorageOfTypedObject(obj) {
  if (IsObject(obj)) {
    if (ObjectIsOpaqueTypedObject(obj))
      return null;
    if (ObjectIsTransparentTypedObject(obj)) {
      if (!TypedObjectIsAttached(obj))
          ThrowTypeError(449);
      var descr = TypedObjectTypeDescr(obj);
      var byteLength = UnsafeGetInt32FromReservedSlot(descr, 3);
      return { buffer: TypedObjectBuffer(obj),
               byteLength,
               byteOffset: TypedObjectByteOffset(obj) };
    }
  }
  ThrowTypeError(447);
}
function TypeOfTypedObject(obj) {
  if (IsObject(obj) && ObjectIsTypedObject(obj))
    return TypedObjectTypeDescr(obj);
  var T = GetTypedObjectModule();
  switch (typeof obj) {
    case "object": return T.Object;
    case "function": return T.Object;
    case "string": return T.String;
    case "number": return T.float64;
    case "undefined": return T.Any;
    default: return T.Any;
  }
}
function TypedObjectArrayTypeBuild(a, b, c) {
  if (!IsObject(this) || !ObjectIsTypeDescr(this))
    ThrowTypeError(447);
  var kind = UnsafeGetInt32FromReservedSlot(this, 0);
  switch (kind) {
  case 4:
    if (typeof a === "function")
      return BuildTypedSeqImpl(this, this.length, 1, a);
    else if (typeof a === "number" && typeof b === "function")
      return BuildTypedSeqImpl(this, this.length, a, b);
    else if (typeof a === "number")
      ThrowTypeError(447);
    else
      ThrowTypeError(447);
  default:
    ThrowTypeError(447);
  }
}
function TypedObjectArrayTypeFrom(a, b, c) {
  if (!IsObject(this) || !ObjectIsTypeDescr(this))
    ThrowTypeError(447);
  var untypedInput = !IsObject(a) || !ObjectIsTypedObject(a) ||
                     !TypeDescrIsArrayType(TypedObjectTypeDescr(a));
  if (untypedInput) {
    if (b === 1 && IsCallable(c))
      return MapUntypedSeqImpl(a, this, c);
    if (IsCallable(b))
      return MapUntypedSeqImpl(a, this, b);
    ThrowTypeError(447);
  }
  if (typeof b === "number" && IsCallable(c))
    return MapTypedSeqImpl(a, b, this, c);
  if (IsCallable(b))
    return MapTypedSeqImpl(a, 1, this, b);
  ThrowTypeError(447);
}
function TypedObjectArrayMap(a, b) {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(447);
  var thisType = TypedObjectTypeDescr(this);
  if (!TypeDescrIsArrayType(thisType))
    ThrowTypeError(447);
  if (typeof a === "number" && typeof b === "function")
    return MapTypedSeqImpl(this, a, thisType, b);
  else if (typeof a === "function")
    return MapTypedSeqImpl(this, 1, thisType, a);
  ThrowTypeError(447);
}
function TypedObjectArrayReduce(a, b) {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(447);
  var thisType = TypedObjectTypeDescr(this);
  if (!TypeDescrIsArrayType(thisType))
    ThrowTypeError(447);
  if (a !== undefined && typeof a !== "function")
    ThrowTypeError(447);
  var outputType = thisType.elementType;
  return ReduceTypedSeqImpl(this, outputType, a, b);
}
function TypedObjectArrayFilter(func) {
  if (!IsObject(this) || !ObjectIsTypedObject(this))
    ThrowTypeError(447);
  var thisType = TypedObjectTypeDescr(this);
  if (!TypeDescrIsArrayType(thisType))
    ThrowTypeError(447);
  if (typeof func !== "function")
    ThrowTypeError(447);
  return FilterTypedSeqImpl(this, func);
}
function NUM_BYTES(bits) {
  return (bits + 7) >> 3;
}
function SET_BIT(data, index) {
  var word = index >> 3;
  var mask = 1 << (index & 0x7);
  data[word] |= mask;
}
function GET_BIT(data, index) {
  var word = index >> 3;
  var mask = 1 << (index & 0x7);
  return (data[word] & mask) != 0;
}
function BuildTypedSeqImpl(arrayType, len, depth, func) {
  ;;
  if (depth <= 0 || ((depth) | 0) !== depth) {
    ThrowTypeError(447);
  }
  var {iterationSpace, grainType, totalLength} =
    ComputeIterationSpace(arrayType, depth, len);
  var result = new arrayType();
  var indices = new List();
  indices.length = depth;
  for (var i = 0; i < depth; i++) {
    indices[i] = 0;
  }
  var grainTypeIsComplex = !TypeDescrIsSimpleType(grainType);
  var size = UnsafeGetInt32FromReservedSlot(grainType, 3);
  var outOffset = 0;
  for (i = 0; i < totalLength; i++) {
    var userOutPointer = TypedObjectGetOpaqueIf(grainType, result, outOffset,
                                                grainTypeIsComplex);
    callFunction(std_Array_push, indices, userOutPointer);
    var r = callFunction(std_Function_apply, func, undefined, indices);
    callFunction(std_Array_pop, indices);
    if (r !== undefined)
      TypedObjectSet(grainType, result, outOffset, null, r);
    IncrementIterationSpace(indices, iterationSpace);
    outOffset += size;
  }
  return result;
}
function ComputeIterationSpace(arrayType, depth, len) {
  ;;
  ;;
  ;;
  var iterationSpace = new List();
  iterationSpace.length = depth;
  iterationSpace[0] = len;
  var totalLength = len;
  var grainType = arrayType.elementType;
  for (var i = 1; i < depth; i++) {
    if (TypeDescrIsArrayType(grainType)) {
      var grainLen = grainType.length;
      iterationSpace[i] = grainLen;
      totalLength *= grainLen;
      grainType = grainType.elementType;
    } else {
      ThrowTypeError(447);
    }
  }
  return { iterationSpace, grainType, totalLength };
}
function IncrementIterationSpace(indices, iterationSpace) {
  ;
                                                                  ;
  var n = indices.length - 1;
  while (true) {
    indices[n] += 1;
    if (indices[n] < iterationSpace[n])
      return;
    ;
                                                                     ;
    indices[n] = 0;
    if (n == 0)
      return;
    n -= 1;
  }
}
function MapUntypedSeqImpl(inArray, outputType, maybeFunc) {
  ;;
  ;;
  inArray = ToObject(inArray);
  ;;
  if (!IsCallable(maybeFunc))
    ThrowTypeError(9, DecompileArg(0, maybeFunc));
  var func = maybeFunc;
  var outLength = outputType.length;
  var outGrainType = outputType.elementType;
  var result = new outputType();
  var outUnitSize = UnsafeGetInt32FromReservedSlot(outGrainType, 3);
  var outGrainTypeIsComplex = !TypeDescrIsSimpleType(outGrainType);
  var outOffset = 0;
  for (var i = 0; i < outLength; i++) {
    if (i in inArray) {
      var element = inArray[i];
      var out = TypedObjectGetOpaqueIf(outGrainType, result, outOffset,
                                       outGrainTypeIsComplex);
      var r = func(element, i, inArray, out);
      if (r !== undefined)
        TypedObjectSet(outGrainType, result, outOffset, null, r);
    }
    outOffset += outUnitSize;
  }
  return result;
}
function MapTypedSeqImpl(inArray, depth, outputType, func) {
  ;;
  ;;
  ;;
  if (depth <= 0 || ((depth) | 0) !== depth)
    ThrowTypeError(447);
  var inputType = TypeOfTypedObject(inArray);
  var {iterationSpace: inIterationSpace, grainType: inGrainType} =
    ComputeIterationSpace(inputType, depth, inArray.length);
  if (!IsObject(inGrainType) || !ObjectIsTypeDescr(inGrainType))
    ThrowTypeError(447);
  var {iterationSpace, grainType: outGrainType, totalLength} =
    ComputeIterationSpace(outputType, depth, outputType.length);
  for (var i = 0; i < depth; i++)
    if (inIterationSpace[i] !== iterationSpace[i])
      ThrowTypeError(447);
  var result = new outputType();
  var inGrainTypeIsComplex = !TypeDescrIsSimpleType(inGrainType);
  var outGrainTypeIsComplex = !TypeDescrIsSimpleType(outGrainType);
  var inOffset = 0;
  var outOffset = 0;
  var isDepth1Simple = depth == 1 && !(inGrainTypeIsComplex || outGrainTypeIsComplex);
  var inUnitSize = isDepth1Simple ? 0 : UnsafeGetInt32FromReservedSlot(inGrainType, 3);
  var outUnitSize = isDepth1Simple ? 0 : UnsafeGetInt32FromReservedSlot(outGrainType, 3);
  function DoMapTypedSeqDepth1() {
    for (var i = 0; i < totalLength; i++) {
      var element = TypedObjectGet(inGrainType, inArray, inOffset);
      var out = TypedObjectGetOpaqueIf(outGrainType, result, outOffset,
                                       outGrainTypeIsComplex);
      var r = func(element, i, inArray, out);
      if (r !== undefined)
        TypedObjectSet(outGrainType, result, outOffset, null, r);
      inOffset += inUnitSize;
      outOffset += outUnitSize;
    }
    return result;
  }
  function DoMapTypedSeqDepth1Simple(inArray, totalLength, func, result) {
    for (var i = 0; i < totalLength; i++) {
      var r = func(inArray[i], i, inArray, undefined);
      if (r !== undefined)
        result[i] = r;
    }
    return result;
  }
  function DoMapTypedSeqDepthN() {
    var indices = new List();
    for (var i = 0; i < depth; i++)
        callFunction(std_Array_push, indices, 0);
    for (var i = 0; i < totalLength; i++) {
      var element = TypedObjectGet(inGrainType, inArray, inOffset);
      var out = TypedObjectGetOpaqueIf(outGrainType, result, outOffset,
                                       outGrainTypeIsComplex);
      var args = [element];
      callFunction(std_Function_apply, std_Array_push, args, indices);
      callFunction(std_Array_push, args, inArray, out);
      var r = callFunction(std_Function_apply, func, void 0, args);
      if (r !== undefined)
        TypedObjectSet(outGrainType, result, outOffset, null, r);
      inOffset += inUnitSize;
      outOffset += outUnitSize;
      IncrementIterationSpace(indices, iterationSpace);
    }
    return result;
  }
  if (isDepth1Simple)
    return DoMapTypedSeqDepth1Simple(inArray, totalLength, func, result);
  if (depth == 1)
    return DoMapTypedSeqDepth1();
  return DoMapTypedSeqDepthN();
}
function ReduceTypedSeqImpl(array, outputType, func, initial) {
  ;;
  ;;
  var start, value;
  if (initial === undefined && array.length < 1)
    ThrowTypeError(447);
  if (TypeDescrIsSimpleType(outputType)) {
    if (initial === undefined) {
      start = 1;
      value = array[0];
    } else {
      start = 0;
      value = outputType(initial);
    }
    for (var i = start; i < array.length; i++)
      value = outputType(func(value, array[i]));
  } else {
    if (initial === undefined) {
      start = 1;
      value = new outputType(array[0]);
    } else {
      start = 0;
      value = initial;
    }
    for (var i = start; i < array.length; i++)
      value = func(value, array[i]);
  }
  return value;
}
function FilterTypedSeqImpl(array, func) {
  ;;
  ;;
  var arrayType = TypeOfTypedObject(array);
  if (!TypeDescrIsArrayType(arrayType))
    ThrowTypeError(447);
  var elementType = arrayType.elementType;
  var flags = new Uint8Array(NUM_BYTES(array.length));
  var count = 0;
  var size = UnsafeGetInt32FromReservedSlot(elementType, 3);
  var inOffset = 0;
  for (var i = 0; i < array.length; i++) {
    var v = TypedObjectGet(elementType, array, inOffset);
    if (func(v, i, array)) {
      SET_BIT(flags, i);
      count++;
    }
    inOffset += size;
  }
  var AT = GetTypedObjectModule().ArrayType;
  var resultType = new AT(elementType, count);
  var result = new resultType();
  for (var i = 0, j = 0; i < array.length; i++) {
    if (GET_BIT(flags, i))
      result[j++] = array[i];
  }
  return result;
}
function WeakMapConstructorInit(iterable) {
    var map = this;
    var adder = map.set;
    if (!IsCallable(adder))
        ThrowTypeError(9, typeof adder);
    for (var nextItem of allowContentIter(iterable)) {
        if (!IsObject(nextItem))
            ThrowTypeError(30, "WeakMap");
        callContentFunction(adder, map, nextItem[0], nextItem[1]);
    }
}
function WeakSetConstructorInit(iterable) {
    var set = this;
    var adder = set.add;
    if (!IsCallable(adder))
        ThrowTypeError(9, typeof adder);
    for (var nextValue of allowContentIter(iterable))
        callContentFunction(adder, set, nextValue);
}