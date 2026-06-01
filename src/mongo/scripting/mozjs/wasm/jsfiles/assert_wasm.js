/**
 * This is a non-modulified version of the assert library for server side javascript.
 * The WASM MozJS engine doesn't support module loading.
 */

function doassert(msg) {
    // eval if msg is a function
    if (typeof msg == "function") msg = msg();

    if (typeof msg == "object") msg = tojson(msg);

    let ex;
    ex = Error(msg);
    throw ex;
}

function _processMsg(msg) {
    if (typeof msg === "function") {
        msg = msg();
    }

    if (typeof msg === "object") {
        msg = tojson(msg);
    }
    return msg;
}

function _buildAssertionMessage(msg, prefix) {
    let fullMessage = "";

    if (prefix) {
        fullMessage += prefix;
    }

    if (prefix && msg) {
        fullMessage += " : ";
    }

    if (msg) {
        fullMessage += _processMsg(msg);
    }

    return fullMessage;
}

function formatErrorMsg(msg, attr = {}, serializeFn = tojson) {
    for (const [key, value] of Object.entries(attr)) {
        msg = msg.replaceAll(`{${key}}`, serializeFn(value));
    }
    return msg;
}

function _doassert(msg, prefix, attr) {
    doassert(_buildAssertionMessage(msg, formatErrorMsg(prefix, attr, tojson)), attr?.res);
}

function assert(value, msg, attr) {
    if (arguments.length > 3) {
        _doassert("Too many parameters to assert().");
    }

    if (value) {
        return;
    }

    _doassert(msg, "assert failed", attr);
}

function friendlyEqual(a, b) {
    if (a == b) return true;

    a = tojson(a, false, true);
    b = tojson(b, false, true);

    if (a == b) return true;

    let clean = function (s) {
        s = s.replace(/NumberInt\((-?\d+)\)/g, "$1");
        return s;
    };

    a = clean(a);
    b = clean(b);

    if (a == b) return true;

    return false;
}

function _isEq(a, b) {
    if (a == b) {
        return true;
    }

    if (a != null && b != null && friendlyEqual(a, b)) {
        return true;
    }

    return false;
}

assert.eq = function (a, b, msg, attr) {
    if (_isEq(a, b)) {
        return;
    }

    // just use one level of display for a concise but helpful output
    const shortDisp = (x) => tojson(x, " ", true, tojson.MAX_DEPTH);
    const aDisplay = shortDisp(a);
    const bDisplay = shortDisp(b);
    const diff = patchDiff(a, b);

    _doassert(msg, `expected ${aDisplay} to equal ${bDisplay}\n${diff}\n`, {...attr});
};

function assertThrowsHelper(func, params) {
    if (typeof func !== "function") {
        _doassert("1st argument must be a function");
    }

    if (
        arguments.length >= 2 &&
        !Array.isArray(params) &&
        Object.prototype.toString.call(params) !== "[object Arguments]"
    ) {
        _doassert("2nd argument must be an Array or Arguments object");
    }

    let thisKeywordWasUsed = false;

    const thisSpy = new Proxy(
        {},
        {
            has: () => {
                thisKeywordWasUsed = true;
                return false;
            },

            get: () => {
                thisKeywordWasUsed = true;
                return undefined;
            },

            set: () => {
                thisKeywordWasUsed = true;
                return false;
            },

            deleteProperty: () => {
                thisKeywordWasUsed = true;
                return false;
            },
        },
    );

    let error = null;
    let res = null;
    try {
        res = func.apply(thisSpy, params);
    } catch (e) {
        error = e;
    }

    if (thisKeywordWasUsed) {
        _doassert(
            "Attempted to access 'this' during function call in" +
                " assert.throws/doesNotThrow. Instead, wrap the function argument in" +
                " another function.",
        );
    }

    return {error, res};
}

assert.doesNotThrow = function (func, params, msg, attr) {
    // preserve the length of the `arguments` object.
    // eslint-disable-next-line prefer-rest-params
    const {error, res} = assertThrowsHelper(...arguments);

    if (error) {
        const {code, message} = error;
        _doassert(msg, "threw unexpected exception: {error}", {
            error: {...(code && {code}), ...(message && {message})},
            ...attr,
        });
    }

    return res;
};

assert.throws = function (func, params, msg, attr) {
    // preserve the length of the `arguments` object.
    // eslint-disable-next-line prefer-rest-params
    const {error} = assertThrowsHelper(...arguments);

    if (!error) {
        _doassert(msg, "did not throw exception", attr);
    }

    return error;
};
