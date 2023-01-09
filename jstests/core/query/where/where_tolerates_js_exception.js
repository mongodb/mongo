/**
 * Test that $where fails gracefully when user-provided JavaScript code throws and that the user
 * gets back the JavaScript stacktrace.
 *
 * @tags: [
 *   requires_non_retryable_commands,
 *   requires_scripting,
 * ]
 */
(function() {
"use strict";

const collection = db.where_tolerates_js_exception;
collection.drop();

assert.commandWorked(collection.save({a: 1}));

const res = collection.runCommand("find", {
    filter: {
        $where: function myFunction() {
            return a();
        }
    }
});

assert.commandFailedWithCode(res, ErrorCodes.JSInterpreterFailure);
assert(/ReferenceError/.test(res.errmsg),
       () => "$where didn't failed with a ReferenceError: " + tojson(res));
assert(/myFunction@/.test(res.errmsg),
       () => "$where didn't return the JavaScript stacktrace: " + tojson(res));
assert(!res.hasOwnProperty("stack"),
       () => "$where shouldn't return JavaScript stacktrace separately: " + tojson(res));
assert(!res.hasOwnProperty("originalError"),
       () => "$where shouldn't return wrapped version of the error: " + tojson(res));
})();
