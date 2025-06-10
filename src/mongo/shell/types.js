/**
 * Shims and polyfills for various types.
 */

// Date and time types
/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
Timestamp.prototype.tojson = function() {
    return this.toStringIncomparable();
};

Timestamp.prototype.getTime = function() {
    return this.t;
};

Timestamp.prototype.getInc = function() {
    return this.i;
};

Timestamp.prototype.toString = function() {
    // Resmoke overrides `toString` to throw an error to prevent accidental operator
    // comparisons, e.g: >, -, etc...
    return this.toStringIncomparable();
};

Timestamp.prototype.toStringIncomparable = function() {
    return `Timestamp(${this.t}, ${this.i})`;
};

Date.timeFunc = function(theFunc, numTimes = 1, ...args) {
    let start = new Date();
    for (let i = 0; i < numTimes; i++) {
        theFunc.apply(null, args);
    }

    return (new Date()).getTime() - start.getTime();
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
Date.prototype.tojson = function() {
    try {
        // If this === Date.prototype or this is a Date instance created from
        // Object.create(Date.prototype), then the [[DateValue]] internal slot won't be set and will
        // lead to a TypeError. We instead treat it as though the [[DateValue]] internal slot is NaN
        // in order to be consistent with the ES5 behavior in MongoDB 3.2 and earlier.
        this.getTime();
    } catch (e) {
        if (e instanceof TypeError &&
            e.message.includes("Date.prototype.getTime called on incompatible")) {
            return new Date(NaN).tojson();
        }
        throw e;
    }

    const YYYY = this.getUTCFullYear().zeroPad(4);
    const MM = (this.getUTCMonth() + 1).zeroPad(2);
    const DD = this.getUTCDate().zeroPad(2);
    const HH = this.getUTCHours().zeroPad(2);
    const mm = this.getUTCMinutes().zeroPad(2);
    let ss = this.getUTCSeconds().zeroPad(2);

    if (this.getUTCMilliseconds())
        ss += '.' + this.getUTCMilliseconds().zeroPad(3);

    const ofs = 'Z';
    return `ISODate("${YYYY}-${MM}-${DD}T${HH}:${mm}:${ss}${ofs}")`;
};

// eslint-disable-next-line
ISODate = function(isoDateStr) {
    if (!isoDateStr)
        return new Date();

    const isoDateRegex =
        /^(\d{4})-?(\d{2})-?(\d{2})([T ](\d{2})(:?(\d{2})(:?(\d{2}(\.\d+)?))?)?(Z|([+-])(\d{2}):?(\d{2})?)?)?$/;
    let res = isoDateRegex.exec(isoDateStr);

    if (!res)
        throw Error("invalid ISO date: " + isoDateStr);

    /*
     * Note that we use `a || b` instead of `a ?? b` as fallthroughs because a might be NaN,
     * which is falsey but not nullish, and we want to convert those to zeros.
     */
    const year = parseInt(res[1], 10);
    const month = (parseInt(res[2], 10)) - 1;
    const date = parseInt(res[3], 10);
    const hour = parseInt(res[5], 10) || 0;
    const min = parseInt(res[7], 10) || 0;
    const sec = parseInt(res[9]?.substring(0, 2), 10) || 0;
    const ms = Math.round((parseFloat(res[10]) || 0) * 1_000);

    let dateTime = new Date();

    dateTime.setUTCFullYear(year, month, date);
    dateTime.setUTCHours(hour);
    dateTime.setUTCMinutes(min);
    dateTime.setUTCSeconds(sec);
    let time = dateTime.setUTCMilliseconds(ms);

    if (res[11] && res[11] != 'Z') {
        let ofs = 0;
        ofs += (parseInt(res[13], 10) || 0) * 60 * 60 * 1_000;  // hours
        ofs += (parseInt(res[14], 10) || 0) * 60 * 1_000;       // mins
        if (res[12] == '+')                                     // if ahead subtract
            ofs *= -1;

        time += ofs;
    }

    // If we are outside the range 0000-01-01T00:00:00.000Z - 9999-12-31T23:59:59.999Z, abort with
    // error.
    const DATE_RANGE_MIN_MICROSECONDS = -62167219200000;
    const DATE_RANGE_MAX_MICROSECONDS = 253402300799999;

    if (time < DATE_RANGE_MIN_MICROSECONDS || time > DATE_RANGE_MAX_MICROSECONDS)
        throw Error("invalid ISO date: " + isoDateStr);

    return new Date(time);
};

// Regular Expression
RegExp.escape = function(text) {
    // Supported since Firefox 134; polyfill until then.
    // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/RegExp/escape
    return text.replace(/[-[\]{}()*+?.,\\^$|#\s]/g, "\\$&");
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
RegExp.prototype.tojson = RegExp.prototype.toString;

// Array
Array.contains = function(a, x) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.contains must be an array");
    }

    return a.includes(x);
};

Array.unique = function(a) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.unique must be an array");
    }

    return [...new Set(a)];
};

Array.shuffle = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.shuffle must be an array");
    }

    for (let i = 0; i < arr.length - 1; i++) {
        const j = i + Random.randInt(arr.length - i);
        [arr[i], arr[j]] = [arr[j], arr[i]];
    }
    return arr;
};

/**
 * The return value is not always a valid JSON string. See 'tojson()' function comment for details.
 */
Array.tojson = function(a, indent, nolint, depth = 0, sortKeys) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.tojson must be an array");
    }

    if (depth > tojson.MAX_DEPTH) {
        return "[Array]";
    }

    if (a.length == 0) {
        return "[ ]";
    }

    if (globalThis.TestData?.logFormat === "json" && typeof nolint !== "boolean") {
        nolint = true;
        indent = "";
    }

    if (nolint) {
        indent = "";
    } else {
        // add to indent if we are pretty
        indent += "\t";
    }

    let elementSeparator = nolint ? " " : "\n";
    let s = "[" + elementSeparator;

    for (let i = 0; i < a.length; i++) {
        s += indent + tojson(a[i], indent, nolint, depth + 1, sortKeys);
        if (i < a.length - 1) {
            s += "," + elementSeparator;
        }
    }

    // remove from indent if we are pretty
    if (!nolint)
        indent = indent.substring(1);

    s += elementSeparator + indent + "]";
    return s;
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
Set.tojson = function(s, indent, nolint, depth) {
    return `new Set(${Array.tojson(Array.from(s), indent, nolint, depth)})`;
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
Map.tojson = function(m, indent, nolint, depth) {
    return `new Map(${Array.tojson(Array.from(m.entries()), indent, nolint, depth)})`;
};

Array.fetchRefs = function(arr, coll) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.fetchRefs must be an array");
    }

    return arr.filter(z => !coll || coll == z.getCollection()).map(z => z.fetch());
};

Array.sum = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.sum must be an array");
    }

    const L = arr.length;
    if (L == 0)
        return null;

    // prefer native for-loop (instead of reduce) for performance on large arrays
    let s = arr[0];
    for (let i = 1; i < L; i++)
        s += arr[i];
    return s;
};

Array.avg = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.avg must be an array");
    }

    if (arr.length == 0)
        return null;
    return Array.sum(arr) / arr.length;
};

Array.stdDev = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.stdDev must be an array");
    }

    let avg = Array.avg(arr);
    let sum = 0;

    for (let i = 0; i < arr.length; i++) {
        sum += Math.pow(arr[i] - avg, 2);
    }

    return Math.sqrt(sum / arr.length);
};

// Object
Object.extend = function(dst, src, deep) {
    for (let k in src) {
        let v = src[k];
        if (deep && typeof (v) == "object" && v !== null) {
            if (v.constructor === ObjectId) {  // convert ObjectId properly
                eval("v = " + tojson(v));
            } else if ("floatApprox" in v) {  // convert NumberLong properly
                eval("v = " + tojson(v));
            } else if (v.constructor === Date) {  // convert Date properly
                eval("v = " + tojson(v));
            } else {
                v = Object.extend(typeof (v.length) == "number" ? [] : {}, v, true);
            }
        }
        dst[k] = v;
    }
    return dst;
};

Object.merge = function(dst, src, deep) {
    let clone = Object.extend({}, dst, deep);
    return Object.extend(clone, src, deep);
};

// If there is a conflict in values of a key for two objects being merged, the second value will
// override the first one in the merged object
Object.deepMerge = function(...objects) {
    const isObject = obj => obj && typeof obj === 'object';

    // Create new object prev to hold combination of all object fields.
    return objects.reduce((prev, obj = {}) => {
        Object.keys(obj).forEach(key => {
            const pVal = prev[key];  // Get the values for key from the two objects being merged.
            const oVal = obj[key];

            if (Array.isArray(pVal) &&
                Array.isArray(oVal)) {  // If both are arrays then concatenate them into a new
                                        // array and add it to prev.
                prev[key] = pVal.concat(...oVal);
            } else if (isObject(pVal) &&
                       isObject(oVal)) {  // If both are objects then recursively merge again.
                prev[key] = Object.deepMerge(pVal, oVal);
            } else {  // In all other cases set prev[key] to obj[key].
                prev[key] = oVal;
            }
        });

        return prev;
    }, {});
};

Object.keySet = function(o) {
    let ret = new Array();
    for (let i in o) {
        if (!(i in o.__proto__ && o[i] === o.__proto__[i])) {
            ret.push(i);
        }
    }
    return ret;
};

// String

// always provide ltrim and rtrim for backwards compatibility
String.prototype.ltrim = String.prototype.trimStart;
String.prototype.rtrim = String.prototype.trimEnd;

// Returns a copy padded with the provided character _chr_ so it becomes (at least) _length_
// characters long.
// No truncation is performed if the string is already longer than _length_.
// @param length minimum length of the returned string
// @param right if falsy add leading whitespace, otherwise add trailing whitespace
// @param chr character to be used for padding, defaults to whitespace
// @return the padded string
String.prototype.pad = function(length, right, chr) {
    return right ? this.padEnd(length, chr) : this.padStart(length, chr);
};

// Number
Number.prototype.toPercentStr = function() {
    return (this * 100).toFixed(2) + "%";
};

Number.prototype.zeroPad = function(width) {
    return ('' + this).pad(width, false, '0');
};

// NumberLong

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
NumberLong.prototype.tojson = function() {
    return this.toString();
};

// NumberInt

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
NumberInt.prototype.tojson = function() {
    return this.toString();
};

// NumberDecimal

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
NumberDecimal.prototype.tojson = function() {
    return this.toString();
};

NumberDecimal.prototype.equals = function(other) {
    return numberDecimalsEqual(this, other);
};

// ObjectId

ObjectId.prototype.toString = function() {
    return "ObjectId(" + tojson(this.str) + ")";
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
ObjectId.prototype.tojson = function() {
    return this.toString();
};

ObjectId.prototype.valueOf = function() {
    return this.str;
};

ObjectId.prototype.isObjectId = true;

ObjectId.prototype.getTimestamp = function() {
    return new Date(parseInt(this.valueOf().slice(0, 8), 16) * 1_000);
};

ObjectId.prototype.equals = function(other) {
    return this.str == other.str;
};

// Creates an ObjectId from a Date.
// Based on solution discussed here:
//     http://stackoverflow.com/questions/8749971/can-i-query-mongodb-objectid-by-date
ObjectId.fromDate = function(source) {
    if (!source) {
        throw Error("date missing or undefined");
    }

    // Extract Date from input.
    // If input is a string, assume ISO date string and
    // create a Date from the string.
    if (!(source instanceof Date)) {
        throw Error("Cannot create ObjectId from " + typeof (source) + ": " + tojson(source));
    }

    // Convert date object to seconds since Unix epoch.
    let seconds = Math.floor(source.getTime() / 1_000);

    // Generate hex timestamp with padding.
    let hexTimestamp = seconds.toString(16).pad(8, false, '0') + "0000000000000000";

    // Create an ObjectId with hex timestamp.
    let objectId = ObjectId(hexTimestamp);

    return objectId;
};

// DBPointer

DBPointer.prototype.fetch = function() {
    assert(this.ns, "need a ns");
    assert(this.id, "need an id");
    return globalThis.db[this.ns].findOne({_id: this.id});
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
DBPointer.prototype.tojson = function() {
    return this.toString();
};

DBPointer.prototype.getCollection = function() {
    return this.ns;
};

DBPointer.prototype.getId = function() {
    return this.id;
};

DBPointer.prototype.toString = function() {
    return `DBPointer(${tojson(this.ns)}, ${tojson(this.id)})`;
};

// DBRef
DBRef.prototype.fetch = function() {
    assert(this.$ref, "need a ns");
    assert(this.$id, "need an id");
    let coll = this.$db ? globalThis.db.getSiblingDB(this.$db).getCollection(this.$ref)
                        : globalThis.db[this.$ref];
    return coll.findOne({_id: this.$id});
};

/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
DBRef.prototype.tojson = function() {
    return this.toString();
};

DBRef.prototype.getDb = function() {
    return this.$db ?? undefined;
};

DBRef.prototype.getCollection = function() {
    return this.$ref;
};

DBRef.prototype.getRef = function() {
    return this.$ref;
};

DBRef.prototype.getId = function() {
    return this.$id;
};

DBRef.prototype.toString = function() {
    return "DBRef(" + tojson(this.$ref) + ", " + tojson(this.$id) +
        (this.$db ? ", " + tojson(this.$db) : "") + ")";
};

// BinData
/**
 * The return value is not a valid JSON string. See 'tojson()' function comment for details.
 */
BinData.prototype.tojson = function() {
    return this.toString();
};

BinData.prototype.subtype = function() {
    return this.type;
};
BinData.prototype.length = function() {
    return this.len;
};

// BSONAwareMap
BSONAwareMap = function() {
    this._data = {};
};

BSONAwareMap.hash = function(val) {
    if (!val)
        return val;

    switch (typeof (val)) {
        case 'string':
        case 'number':
        case 'date':
            return val.toString();
        case 'object':
        case 'array': {
            let s = "";
            for (let k in val) {
                s += k + val[k];
            }
            return s;
        }
    }

    throw Error("can't hash : " + typeof (val));
};

BSONAwareMap.prototype.put = function(key, value) {
    let o = this._get(key);
    let old = o.value;
    o.value = value;
    return old;
};

BSONAwareMap.prototype.get = function(key) {
    return this._get(key).value;
};

BSONAwareMap.prototype._get = function(key) {
    let h = BSONAwareMap.hash(key);
    let a = this._data[h];
    if (!a) {
        a = [];
        this._data[h] = a;
    }
    for (let i = 0; i < a.length; i++) {
        if (friendlyEqual(key, a[i].key)) {
            return a[i];
        }
    }
    let o = {key: key, value: null};
    a.push(o);
    return o;
};

BSONAwareMap.prototype.values = function() {
    let all = [];
    for (let k in this._data) {
        this._data[k].forEach(function(z) {
            all.push(z.value);
        });
    }
    return all;
};

// Free Functions

/**
 * The return value is not always a valid JSON string. See 'tojson()' function comment for details.
 */
tojsononeline = function(x) {
    return tojson(x, " ", true);
};

/**
 * Serializes the given argument 'x' to a string that can be used to deserialize it with 'eval()'.
 * The return value is not always a valid JSON string. Use 'toJsonForLog()' for valid JSON output
 * and for printing values into the logs.
 */
tojson = function(x, indent, nolint, depth = 0, sortKeys) {
    // Note that `nolint` is used as a tri-state: not providing it IS specifying behavior
    if (x === null)
        return "null";

    if (x === undefined)
        return "undefined";

    if (globalThis.TestData?.logFormat === "json" && typeof nolint !== "boolean") {
        nolint = true;
        indent = "";
    }

    // can be undefined, null, or empty string, and even false
    indent ||= "";

    switch (typeof x) {
        case "string":
            return JSON.stringify(x);
        case "number":
        case "boolean":
            return "" + x;
        case "object": {
            let s = tojsonObject(x, indent, nolint, depth, sortKeys);
            if ((nolint == null || nolint == true) && s.length < 80 && indent.length == 0) {
                s = s.replace(/[\t\r\n]+/gm, " ");
            }
            return s;
        }
        case "function":
            if (x === MinKey || x === MaxKey)
                return x.tojson();
            return x.toString();
        default:
            throw Error("tojson can't handle type " + (typeof x));
    }
};
tojson.MAX_DEPTH = 100;

/**
 * Serializes the given object argument 'x' to a string, which can be used to deserialize it with
 * 'eval()'. The return value is not always a valid JSON string. Use 'toJsonForLog()' for valid JSON
 * output and for printing values into the logs.
 */
tojsonObject = function(x, indent, nolint, depth = 0, sortKeys = false) {
    // Note that `nolint` is used as a tri-state: not providing it IS specifying behavior
    if (globalThis.TestData?.logFormat === "json" && typeof nolint !== "boolean") {
        nolint = true;
        indent = "";
    }
    let lineEnding = nolint ? " " : "\n";
    let tabSpace = nolint ? "" : "\t";
    assert.eq((typeof x), "object", "tojsonObject needs object, not [" + (typeof x) + "]");

    if (typeof (x.tojson) == "function" && x.tojson != tojson) {
        return x.tojson(indent, nolint, depth, sortKeys);
    }

    if (x.constructor && typeof (x.constructor.tojson) == "function" &&
        x.constructor.tojson != tojson) {
        return x.constructor.tojson(x, indent, nolint, depth, sortKeys);
    }

    if (x instanceof Error) {
        return `new ${x.name}(${JSON.stringify(x.message)})`;
    }

    try {
        x.toString();
    } catch (e) {
        // toString not callable
        return "[Object]";
    }

    if (depth > tojson.MAX_DEPTH) {
        return "[Object]";
    }

    let s = "{" + lineEnding;

    // push one level of indent
    indent += tabSpace;

    let keys = x;
    if (typeof (x._simpleKeys) == "function")
        keys = x._simpleKeys();
    let keyNames = [];
    for (let k in keys) {
        keyNames.push(k);
    }
    if (sortKeys)
        keyNames.sort();

    let fieldStrings = [];
    for (const k of keyNames) {
        let val = x[k];

        // skip internal DB types to avoid issues with interceptors
        if (val == globalThis.DB?.prototype)
            continue;
        if (val == globalThis.DBCollection?.prototype)
            continue;

        fieldStrings.push(indent + "\"" + k +
                          "\" : " + tojson(val, indent, nolint, depth + 1, sortKeys));
    }

    if (fieldStrings.length > 0) {
        s += fieldStrings.join("," + lineEnding);
    } else {
        s += indent;
    }
    s += lineEnding;

    // pop one level of indent
    indent = indent.substring(1);
    return s + indent + "}";
};

/**
 * Serializes the given argument 'x' to a valid JSON string suitable for logging. The
 * results of 'toJsonForLog()' and 'tostrictjson()' should be equal for BSON objects and arrays.
 * Unlike 'tostrictjson()', 'toJsonForLog()' also accepts non-object types, recognizes recursive
 * objects, and provides more detailed serializations for commonly used JavaScript classes, for
 * instance:
 *  - Set instances serialize to {"$set": [<elem1>,...]}
 *  - Map instances serialize to {"$map": [[<key1>, <value1>],...]}
 *  - Errors instances serialize to {"$error": "<error_message>"}
 *
 * 'toJsonForLog()' must be used when serializing JavaScript values into JSON logs to adhere to the
 * format requirements.
 *
 * Unlike 'tojson()', the result of 'eval(toJsonForLog(x))' will not always evaluate into an object
 * equivalent to 'x' and may throw a syntax error.
 */
toJsonForLog = function(x) {
    function ensureEJSONAndStopOnRecursion() {
        // Stack of ancestors (objects) of the current 'value'.
        // eg, For {"x": 1, "y": {"z": 2}} and value = 2,
        // ancestors = [{"x": 1, "y": {"z": 2}}, {"z": 2}]
        let ancestors = [];
        return function(key, value) {
            if (value === undefined) {
                return {"$undefined": true};
            }
            if (value instanceof Error) {
                return {"$error": value.message};
            }
            if (value instanceof Map) {
                return {"$map": [...value]};
            }
            if (value instanceof Set) {
                return {"$set": [...value]};
            }
            // 'value' is a a pre-transformed property value (of type string in case of Dates),
            // so we use 'this[key]' instead to get the original value.
            if (this[key] instanceof Date) {
                return {"$date": this[key].toISOString().replace("Z", "+00:00")};
            }
            if (typeof value !== "object" || value === null) {
                return value;
            }
            // Remove ancestors not part of the path to current 'value' anymore.
            // `this` is the object that value is contained in,
            // i.e., its direct parent.
            while (ancestors.length > 0 && ancestors.at(-1) !== this) {
                ancestors.pop();
            }
            // 'value' is an object at this point. If it has already been seen, prune the traversal
            // to avoid a 'TypeError' due to self-referencing objects.
            if (ancestors.includes(value)) {
                return "[recursive]";
            }
            ancestors.push(value);
            return value;
        };
    }
    return JSON.stringify(x, ensureEJSONAndStopOnRecursion());
};

/**
 * The printed value is not always a valid JSON string. See 'tojson()' function comment for details.
 */
printjson = function(x) {
    print(tojson(x));
};

/**
 * The printed value is not always a valid JSON string. See 'tojson()' function comment for details.
 */
printjsononeline = function(x) {
    print(tojsononeline(x));
};

isString = function(x) {
    return typeof (x) == "string";
};

isNumber = function(x) {
    return typeof (x) == "number";
};

// This function returns true even if the argument is an array.  See SERVER-14220.
isObject = function(x) {
    return typeof (x) == "object";
};
