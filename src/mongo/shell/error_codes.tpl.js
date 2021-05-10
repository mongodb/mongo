
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
    switch (error) {
        //#for $code in $cat.codes
        case '$code':
            return true;
        //#end for
        default:
            return false;
    }
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
