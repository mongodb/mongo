doassert = function(msg) {
    if (msg.indexOf("assert") == 0)
        print(msg);
    else
        print("assert: " + msg);
    printStackTrace();
    throw msg;
}

assert = function(b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);
    if (b)
        return;
    doassert(msg == undefined ? "assert failed" : "assert failed : " + msg);
}

assert.automsg = function(b) {
    assert(eval(b), b);
}

assert._debug = false;

assert.eq = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a == b)
        return;

    if ((a != null && b != null) && friendlyEqual(a, b))
        return;

    doassert("[" + tojson(a) + "] != [" + tojson(b) + "] are not equal : " + msg);
}

// Sort doc/obj fields and return new sorted obj
sortDoc = function(doc) {

    // Helper to sort the elements of the array
    var sortElementsOfArray = function(arr) {
        var newArr = [];
        if(!arr || arr.constructor != Array)
            return arr;
        for(var i=0; i < arr.length; i++) {
            newArr.push(sortDoc(arr[i]));

        }
        return newArr;
    }

    // not a container we can sort
    if (!(doc instanceof Object))
        return doc;

    // if it an array, sort the elements
    if (doc.constructor == Array)
        return sortElementsOfArray(doc);

    var newDoc = {};
    var fields = Object.keys(doc);
    if (fields.length > 0) {
        fields.sort();
        for(var i=0; i < fields.length; i++){
            var field = fields[i];
            if (doc.hasOwnProperty(field)) {
                var tmp = doc[field];

                if (tmp) {
                    // Sort recursively for Arrays and Objects (including bson ones)
                    if (tmp.constructor == Array)
                        tmp = sortElementsOfArray(tmp);
                    else if (tmp._bson || tmp.constructor == Object)
                        tmp = sortDoc(tmp);
                }
                newDoc[field] = tmp;
            }
        }
    } else {
        newDoc = doc;
    }

    return newDoc;
}

assert.docEq = function(a, b, msg) {
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a == b)
        return;

    var aSorted = sortDoc(a);
    var bSorted = sortDoc(b);

    if ((aSorted != null && bSorted != null) && friendlyEqual(aSorted, bSorted))
        return;

    doassert("[" + tojson(aSorted) + "] != [" + tojson(bSorted) + "] are not equal : " + msg);
}

assert.eq.automsg = function(a, b) {
    assert.eq(eval(a), eval(b), "[" + a + "] != [" + b + "]");
}

assert.neq = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);
    if (a != b)
        return;

    doassert("[" + a + "] != [" + b + "] are equal : " + msg);
}

assert.contains = function(o, arr, msg){
    var wasIn = false

    if(! arr.length){
        for(var i in arr){
            wasIn = arr[i] == o || ((arr[i] != null && o != null) && friendlyEqual(arr[i], o))
                return;
            if(wasIn) break
        }
    }
    else {
        for(var i = 0; i < arr.length; i++){
            wasIn = arr[i] == o || ((arr[i] != null && o != null) && friendlyEqual(arr[i], o))
            if(wasIn) break
        }
    }

    if(! wasIn) doassert(tojson(o) + " was not in " + tojson(arr) + " : " + msg)
}

assert.repeat = function(f, msg, timeout, interval) {
    if (assert._debug && msg) print("in assert for: " + msg);

    var start = new Date();
    timeout = timeout || 30000;
    interval = interval || 200;
    var last;
    while(1) {

        if (typeof(f) == "string"){
            if (eval(f))
                return;
        }
        else {
            if (f())
                return;
        }

        if ((new Date()).getTime() - start.getTime() > timeout)
            break;
        sleep(interval);
    }
}

assert.soon = function(f, msg, timeout /*ms*/, interval) {
    if (assert._debug && msg) print("in assert for: " + msg);

    var start = new Date();
    timeout = timeout || 30000;
    interval = interval || 200;
    var last;
    while(1) {
        if (typeof(f) == "string"){
            if (eval(f))
                return;
        }
        else {
            if (f())
                return;
        }

        diff = (new Date()).getTime() - start.getTime();
        if (diff > timeout)
            doassert("assert.soon failed: " + f + ", msg:" + msg);
        sleep(interval);
    }
}

assert.time = function(f, msg, timeout /*ms*/) {
    if (assert._debug && msg) print("in assert for: " + msg);

    var start = new Date();
    timeout = timeout || 30000;
        if (typeof(f) == "string"){
            res = eval(f);
        }
        else {
            res = f();
        }

        diff = (new Date()).getTime() - start.getTime();
        if (diff > timeout)
            doassert("assert.time failed timeout " + timeout + "ms took " + diff + "ms : " + f + ", msg:" + msg);
        return res;
}

assert.throws = function(func, params, msg) {
    if (assert._debug && msg) print("in assert for: " + msg);
    if (params && typeof(params) == "string") {
        throw ("2nd argument to assert.throws has to be an array, not " + params);
    }
    try {
        func.apply(null, params);
    }
    catch (e) {
        return e;
    }
    doassert("did not throw exception: " + msg);
};

assert.doesNotThrow = function(func, params, msg) {
    if (assert._debug && msg) print("in assert for: " + msg);
    if (params && typeof(params) == "string") {
        throw ("2nd argument to assert.throws has to be an array, not " + params);
    }
    try {
        func.apply(null, params);
    }
    catch (e) {
        doassert("threw unexpected exception: " + e + " : " + msg);
    }
    return;
};

assert.throws.automsg = function(func, params) {
    assert.throws(func, params, func.toString());
};

assert.doesNotThrow.automsg = function(func, params) {
    assert.doesNotThrow(func, params, func.toString());
};

assert.commandWorked = function(res, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (res.ok == 1)
        return;
    doassert("command failed: " + tojson(res) + " : " + msg);
}

assert.commandFailed = function(res, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (res.ok == 0)
        return;
    doassert("command worked when it should have failed: " + tojson(res) + " : " + msg);
}

assert.isnull = function(what, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (what == null)
        return;
    doassert("supposed to be null (" + (msg || "") + ") was: " + tojson(what));
}

assert.lt = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a < b)
        return;
    doassert(a + " is not less than " + b + " : " + msg);
}

assert.gt = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a > b)
        return;
    doassert(a + " is not greater than " + b + " : " + msg);
}

assert.lte = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a <= b)
        return;
    doassert(a + " is not less than or eq " + b + " : " + msg);
}

assert.gte = function(a, b, msg){
    if (assert._debug && msg) print("in assert for: " + msg);

    if (a >= b)
        return;
    doassert(a + " is not greater than or eq " + b + " : " + msg);
}

assert.between = function(a, b, c, msg, inclusive){
    if (assert._debug && msg) print("in assert for: " + msg);

    if((inclusive == undefined || inclusive == true) &&
        a <= b && b <= c) return;
    else if(a < b && b < c) return;
    doassert(b + " is not between " + a + " and " + c + " : " + msg);
}

assert.betweenIn = function(a, b, c, msg){ assert.between(a, b, c, msg, true) }
assert.betweenEx = function(a, b, c, msg){ assert.between(a, b, c, msg, false) }

assert.close = function(a, b, msg, places){
    if (places === undefined) {
        places = 4;
    }
    if (Math.round((a - b) * Math.pow(10, places)) === 0) {
        return;
    }
    doassert(a + " is not equal to " + b + " within " + places +
              " places, diff: " + (a-b) + " : " + msg);
};
