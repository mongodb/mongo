// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

var {ErrorCodes, ErrorCodeStrings} = (function() {
    const handler = {
        get: function(obj, prop) {
            if (typeof prop !== "symbol" && prop in obj === false && prop in Object === false) {
                throw new Error('Unknown Error Code: ' + prop.toString());
            }

            return obj[prop];
        }
    };

    const ErrorCodesObject = {
        //#for $ec in $codes
        '$ec.name': $ec.code,
        //#end for
    };

    const ErrorCodeStringsObject = {
        //#for $ec in $codes
        $ec.code: '$ec.name',
        //#end for
    };

    return {
        ErrorCodes: new Proxy(ErrorCodesObject, handler),
        ErrorCodeStrings: new Proxy(ErrorCodeStringsObject, handler),
    };
})();

//#for $cat in $categories
ErrorCodes.${cat.name} = new Set([
    //#for $code in $cat.codes
    '$code',
    //#end for
]);

ErrorCodes.is${cat.name} = function(err) {
    'use strict';

    var error;
    if (typeof err === 'string') {
        error = err;
    } else if (typeof err === 'number') {
        if (Object.prototype.hasOwnProperty.call(ErrorCodeStrings, err)) {
            error = ErrorCodeStrings[err];
        } else {
            return false;
        }
    }
    return ErrorCodes.${cat.name}.has(error);
};
//#end for

/**
 * Returns true if the mongos `s` is expected to rewrite state change errors.
 * This is determined by a `getParameter` of the "rewriteStateChangeErrors" option.
 * Failure would indicate a mongos that doesn't support the rewrite feature.
 */
ErrorCodes.probeMongosRewrite = function(s) {
    const param = "rewriteStateChangeErrors";
    const res = s.adminCommand({getParameter: 1, [param]: 1});
    if (!(param in res)) {
        print("Mongos did not return a " + param + " server parameter: " + tojson(res));
        return false;
    }
    return res[param];
};

/**
 * Returns the ErrorCode to which the specified mongos `s` would remap the
 * specified `err`. Mongos normally rewrites connection state change errors,
 * unless it is shutting down or the code was injected by a mongos failpoint.
 *
 * The optional `doesRewrite` bool parameter provides a mechanism to bypass the
 * probe, which may be useful if the probe would interfere with a test's
 * operation.
 */
ErrorCodes.doMongosRewrite = function(s, err, doesRewrite) {
    if (doesRewrite === undefined)
        doesRewrite = ErrorCodes.probeMongosRewrite(s);
    if (doesRewrite) {
        if (ErrorCodes.isNotPrimaryError(err) || ErrorCodes.isShutdownError(err)) {
            return ErrorCodes.HostUnreachable;
        }
    }
    return err;
};
