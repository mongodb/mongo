// Date and time types
if (typeof(Timestamp) != "undefined") {
    Timestamp.prototype.tojson = function() {
        return this.toString();
    };

    Timestamp.prototype.getTime = function() {
        return this.t;
    };

    Timestamp.prototype.getInc = function() {
        return this.i;
    };

    Timestamp.prototype.toString = function() {
        return "Timestamp(" + this.t + ", " + this.i + ")";
    };
} else {
    print("warning: no Timestamp class");
}

Date.timeFunc = function(theFunc, numTimes) {
    var start = new Date();
    numTimes = numTimes || 1;
    for (var i = 0; i < numTimes; i++) {
        theFunc.apply(null, Array.from(arguments).slice(2));
    }

    return (new Date()).getTime() - start.getTime();
};

Date.prototype.tojson = function() {
    var UTC = 'UTC';
    var year = this['get' + UTC + 'FullYear']().zeroPad(4);
    var month = (this['get' + UTC + 'Month']() + 1).zeroPad(2);
    var date = this['get' + UTC + 'Date']().zeroPad(2);
    var hour = this['get' + UTC + 'Hours']().zeroPad(2);
    var minute = this['get' + UTC + 'Minutes']().zeroPad(2);
    var sec = this['get' + UTC + 'Seconds']().zeroPad(2);

    if (this['get' + UTC + 'Milliseconds']())
        sec += '.' + this['get' + UTC + 'Milliseconds']().zeroPad(3);

    var ofs = 'Z';
    // // print a non-UTC time
    // var ofsmin = this.getTimezoneOffset();
    // if (ofsmin != 0){
    //     ofs = ofsmin > 0 ? '-' : '+'; // This is correct
    //     ofs += (ofsmin/60).zeroPad(2)
    //     ofs += (ofsmin%60).zeroPad(2)
    // }
    return 'ISODate("' + year + '-' + month + '-' + date + 'T' + hour + ':' + minute + ':' + sec +
        ofs + '")';
};

ISODate = function(isoDateStr) {
    if (!isoDateStr)
        return new Date();

    var isoDateRegex =
        /(\d{4})-?(\d{2})-?(\d{2})([T ](\d{2})(:?(\d{2})(:?(\d{2}(\.\d+)?))?)?(Z|([+-])(\d{2}):?(\d{2})?)?)?/;
    var res = isoDateRegex.exec(isoDateStr);

    if (!res)
        throw Error("invalid ISO date");

    var year = parseInt(res[1], 10) || 1970;  // this should always be present
    var month = (parseInt(res[2], 10) || 1) - 1;
    var date = parseInt(res[3], 10) || 0;
    var hour = parseInt(res[5], 10) || 0;
    var min = parseInt(res[7], 10) || 0;
    var sec = parseInt((res[9] && res[9].substr(0, 2)), 10) || 0;
    var ms = Math.round((parseFloat(res[10]) || 0) * 1000);
    if (ms == 1000) {
        ms = 0;
        ++sec;
    }
    if (sec == 60) {
        sec = 0;
        ++min;
    }
    if (min == 60) {
        min = 0;
        ++hour;
    }
    if (hour == 24) {
        hour = 0;  // the day wrapped, let JavaScript figure out the rest
        var tempTime = Date.UTC(year, month, date, hour, min, sec, ms);
        tempTime += 24 * 60 * 60 * 1000;  // milliseconds in a day
        var tempDate = new Date(tempTime);
        year = tempDate.getUTCFullYear();
        month = tempDate.getUTCMonth();
        date = tempDate.getUTCDate();
    }

    var time = Date.UTC(year, month, date, hour, min, sec, ms);

    if (res[11] && res[11] != 'Z') {
        var ofs = 0;
        ofs += (parseInt(res[13], 10) || 0) * 60 * 60 * 1000;  // hours
        ofs += (parseInt(res[14], 10) || 0) * 60 * 1000;       // mins
        if (res[12] == '+')                                    // if ahead subtract
            ofs *= -1;

        time += ofs;
    }

    return new Date(time);
};

// Regular Expression
RegExp.escape = function(text) {
    return text.replace(/[-[\]{}()*+?.,\\^$|#\s]/g, "\\$&");
};

RegExp.prototype.tojson = RegExp.prototype.toString;

// Array
Array.contains = function(a, x) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.contains must be an array");
    }

    for (var i = 0; i < a.length; i++) {
        if (a[i] == x)
            return true;
    }
    return false;
};

Array.unique = function(a) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.unique must be an array");
    }

    var u = [];
    for (var i = 0; i < a.length; i++) {
        var o = a[i];
        if (!Array.contains(u, o)) {
            u.push(o);
        }
    }
    return u;
};

Array.shuffle = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.shuffle must be an array");
    }

    for (var i = 0; i < arr.length - 1; i++) {
        var pos = i + Random.randInt(arr.length - i);
        var save = arr[i];
        arr[i] = arr[pos];
        arr[pos] = save;
    }
    return arr;
};

Array.tojson = function(a, indent, nolint) {
    if (!Array.isArray(a)) {
        throw new Error("The first argument to Array.tojson must be an array");
    }

    var elementSeparator = nolint ? " " : "\n";

    if (!indent)
        indent = "";
    if (nolint)
        indent = "";

    if (a.length == 0) {
        return "[ ]";
    }

    var s = "[" + elementSeparator;

    // add to indent if we are pretty
    if (!nolint)
        indent += "\t";

    for (var i = 0; i < a.length; i++) {
        s += indent + tojson(a[i], indent, nolint);
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

Array.fetchRefs = function(arr, coll) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.fetchRefs must be an array");
    }

    var n = [];
    for (var i = 0; i < arr.length; i++) {
        var z = arr[i];
        if (coll && coll != z.getCollection())
            continue;
        n.push(z.fetch());
    }
    return n;
};

Array.sum = function(arr) {
    if (!Array.isArray(arr)) {
        throw new Error("The first argument to Array.sum must be an array");
    }

    if (arr.length == 0)
        return null;
    var s = arr[0];
    for (var i = 1; i < arr.length; i++)
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

    var avg = Array.avg(arr);
    var sum = 0;

    for (var i = 0; i < arr.length; i++) {
        sum += Math.pow(arr[i] - avg, 2);
    }

    return Math.sqrt(sum / arr.length);
};

// Object
Object.extend = function(dst, src, deep) {
    for (var k in src) {
        var v = src[k];
        if (deep && typeof(v) == "object") {
            if (v.constructor === ObjectId) {  // convert ObjectId properly
                eval("v = " + tojson(v));
            } else if ("floatApprox" in v) {  // convert NumberLong properly
                eval("v = " + tojson(v));
            } else {
                v = Object.extend(typeof(v.length) == "number" ? [] : {}, v, true);
            }
        }
        dst[k] = v;
    }
    return dst;
};

Object.merge = function(dst, src, deep) {
    var clone = Object.extend({}, dst, deep);
    return Object.extend(clone, src, deep);
};

Object.keySet = function(o) {
    var ret = new Array();
    for (var i in o) {
        if (!(i in o.__proto__ && o[i] === o.__proto__[i])) {
            ret.push(i);
        }
    }
    return ret;
};

// String
if (String.prototype.trim === undefined) {
    String.prototype.trim = function() {
        return this.replace(/^\s+|\s+$/g, "");
    };
}
if (String.prototype.trimLeft === undefined) {
    String.prototype.trimLeft = function() {
        return this.replace(/^\s+/, "");
    };
}
if (String.prototype.trimRight === undefined) {
    String.prototype.trimRight = function() {
        return this.replace(/\s+$/, "");
    };
}

// always provide ltrim and rtrim for backwards compatibility
String.prototype.ltrim = String.prototype.trimLeft;
String.prototype.rtrim = String.prototype.trimRight;

String.prototype.startsWith = function(str) {
    return this.indexOf(str) == 0;
};

String.prototype.endsWith = function(str) {
    return this.indexOf(str, this.length - str.length) !== -1;
};

// Polyfill taken from
// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/String/includes
if (!String.prototype.includes) {
    String.prototype.includes = function() {
        'use strict';
        return String.prototype.indexOf.apply(this, arguments) !== -1;
    };
}

// Returns a copy padded with the provided character _chr_ so it becomes (at least) _length_
// characters long.
// No truncation is performed if the string is already longer than _length_.
// @param length minimum length of the returned string
// @param right if falsy add leading whitespace, otherwise add trailing whitespace
// @param chr character to be used for padding, defaults to whitespace
// @return the padded string
String.prototype.pad = function(length, right, chr) {
    if (typeof chr == 'undefined')
        chr = ' ';
    var str = this;
    for (var i = length - str.length; i > 0; i--) {
        if (right) {
            str = str + chr;
        } else {
            str = chr + str;
        }
    }
    return str;
};

// Number
Number.prototype.toPercentStr = function() {
    return (this * 100).toFixed(2) + "%";
};

Number.prototype.zeroPad = function(width) {
    return ('' + this).pad(width, false, '0');
};

// NumberLong
if (!NumberLong.prototype) {
    NumberLong.prototype = {};
}

NumberLong.prototype.tojson = function() {
    return this.toString();
};

// NumberInt
if (!NumberInt.prototype) {
    NumberInt.prototype = {};
}

NumberInt.prototype.tojson = function() {
    return this.toString();
};

// NumberDecimal
if (typeof NumberDecimal !== 'undefined') {
    if (!NumberDecimal.prototype) {
        NumberDecimal.prototype = {};
    }

    NumberDecimal.prototype.tojson = function() {
        return this.toString();
    };
}

// ObjectId
if (!ObjectId.prototype)
    ObjectId.prototype = {};

ObjectId.prototype.toString = function() {
    return "ObjectId(" + tojson(this.str) + ")";
};

ObjectId.prototype.tojson = function() {
    return this.toString();
};

ObjectId.prototype.valueOf = function() {
    return this.str;
};

ObjectId.prototype.isObjectId = true;

ObjectId.prototype.getTimestamp = function() {
    return new Date(parseInt(this.valueOf().slice(0, 8), 16) * 1000);
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

    var sourceDate;

    // Extract Date from input.
    // If input is a string, assume ISO date string and
    // create a Date from the string.
    if (source instanceof Date) {
        sourceDate = source;
    } else {
        throw Error("Cannot create ObjectId from " + typeof(source) + ": " + tojson(source));
    }

    // Convert date object to seconds since Unix epoch.
    var seconds = Math.floor(sourceDate.getTime() / 1000);

    // Generate hex timestamp with padding.
    var hexTimestamp = seconds.toString(16).pad(8, false, '0') + "0000000000000000";

    // Create an ObjectId with hex timestamp.
    var objectId = ObjectId(hexTimestamp);

    return objectId;
};

// DBPointer
if (typeof(DBPointer) != "undefined") {
    DBPointer.prototype.fetch = function() {
        assert(this.ns, "need a ns");
        assert(this.id, "need an id");
        return db[this.ns].findOne({_id: this.id});
    };

    DBPointer.prototype.tojson = function(indent) {
        return this.toString();
    };

    DBPointer.prototype.getCollection = function() {
        return this.ns;
    };

    DBPointer.prototype.getId = function() {
        return this.id;
    };

    DBPointer.prototype.toString = function() {
        return "DBPointer(" + tojson(this.ns) + ", " + tojson(this.id) + ")";
    };
} else {
    print("warning: no DBPointer");
}

// DBRef
if (typeof(DBRef) != "undefined") {
    DBRef.prototype.fetch = function() {
        assert(this.$ref, "need a ns");
        assert(this.$id, "need an id");
        var coll = this.$db ? db.getSiblingDB(this.$db).getCollection(this.$ref) : db[this.$ref];
        return coll.findOne({_id: this.$id});
    };

    DBRef.prototype.tojson = function(indent) {
        return this.toString();
    };

    DBRef.prototype.getDb = function() {
        return this.$db || undefined;
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
} else {
    print("warning: no DBRef");
}

// BinData
if (typeof(BinData) != "undefined") {
    BinData.prototype.tojson = function() {
        return this.toString();
    };

    BinData.prototype.subtype = function() {
        return this.type;
    };
    BinData.prototype.length = function() {
        return this.len;
    };
} else {
    print("warning: no BinData class");
}

// Map
if (typeof(Map) == "undefined") {
    Map = function() {
        this._data = {};
    };
}

Map.hash = function(val) {
    if (!val)
        return val;

    switch (typeof(val)) {
        case 'string':
        case 'number':
        case 'date':
            return val.toString();
        case 'object':
        case 'array':
            var s = "";
            for (var k in val) {
                s += k + val[k];
            }
            return s;
    }

    throw Error("can't hash : " + typeof(val));
};

Map.prototype.put = function(key, value) {
    var o = this._get(key);
    var old = o.value;
    o.value = value;
    return old;
};

Map.prototype.get = function(key) {
    return this._get(key).value;
};

Map.prototype._get = function(key) {
    var h = Map.hash(key);
    var a = this._data[h];
    if (!a) {
        a = [];
        this._data[h] = a;
    }
    for (var i = 0; i < a.length; i++) {
        if (friendlyEqual(key, a[i].key)) {
            return a[i];
        }
    }
    var o = {key: key, value: null};
    a.push(o);
    return o;
};

Map.prototype.values = function() {
    var all = [];
    for (var k in this._data) {
        this._data[k].forEach(function(z) {
            all.push(z.value);
        });
    }
    return all;
};

if (typeof(gc) == "undefined") {
    gc = function() {
        print("warning: using noop gc()");
    };
}

// Free Functions
tojsononeline = function(x) {
    return tojson(x, " ", true);
};

tojson = function(x, indent, nolint) {
    if (x === null)
        return "null";

    if (x === undefined)
        return "undefined";

    if (!indent)
        indent = "";

    switch (typeof x) {
        case "string": {
            var out = new Array(x.length + 1);
            out[0] = '"';
            for (var i = 0; i < x.length; i++) {
                switch (x[i]) {
                    case '"':
                        out[out.length] = '\\"';
                        break;
                    case '\\':
                        out[out.length] = '\\\\';
                        break;
                    case '\b':
                        out[out.length] = '\\b';
                        break;
                    case '\f':
                        out[out.length] = '\\f';
                        break;
                    case '\n':
                        out[out.length] = '\\n';
                        break;
                    case '\r':
                        out[out.length] = '\\r';
                        break;
                    case '\t':
                        out[out.length] = '\\t';
                        break;

                    default: {
                        var code = x.charCodeAt(i);
                        if (code < 0x20) {
                            out[out.length] =
                                (code < 0x10 ? '\\u000' : '\\u00') + code.toString(16);
                        } else {
                            out[out.length] = x[i];
                        }
                    }
                }
            }

            return out.join('') + "\"";
        }
        case "number":
        case "boolean":
            return "" + x;
        case "object": {
            var s = tojsonObject(x, indent, nolint);
            if ((nolint == null || nolint == true) && s.length < 80 &&
                (indent == null || indent.length == 0)) {
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

tojsonObject = function(x, indent, nolint) {
    var lineEnding = nolint ? " " : "\n";
    var tabSpace = nolint ? "" : "\t";
    assert.eq((typeof x), "object", "tojsonObject needs object, not [" + (typeof x) + "]");

    if (!indent)
        indent = "";

    if (typeof(x.tojson) == "function" && x.tojson != tojson) {
        return x.tojson(indent, nolint);
    }

    if (x.constructor && typeof(x.constructor.tojson) == "function" &&
        x.constructor.tojson != tojson) {
        return x.constructor.tojson(x, indent, nolint);
    }

    if (x instanceof Error) {
        return x.toString();
    }

    try {
        x.toString();
    } catch (e) {
        // toString not callable
        return "[object]";
    }

    var s = "{" + lineEnding;

    // push one level of indent
    indent += tabSpace;

    var keys = x;
    if (typeof(x._simpleKeys) == "function")
        keys = x._simpleKeys();
    var fieldStrings = [];
    for (var k in keys) {
        var val = x[k];

        // skip internal DB types to avoid issues with interceptors
        if (typeof DB != 'undefined' && val == DB.prototype)
            continue;
        if (typeof DBCollection != 'undefined' && val == DBCollection.prototype)
            continue;

        fieldStrings.push(indent + "\"" + k + "\" : " + tojson(val, indent, nolint));
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

printjson = function(x) {
    print(tojson(x));
};

printjsononeline = function(x) {
    print(tojsononeline(x));
};

isString = function(x) {
    return typeof(x) == "string";
};

isNumber = function(x) {
    return typeof(x) == "number";
};

// This function returns true even if the argument is an array.  See SERVER-14220.
isObject = function(x) {
    return typeof(x) == "object";
};
