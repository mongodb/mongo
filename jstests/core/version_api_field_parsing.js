/**
 * Checks that the server properly parses "API Version" parameters
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

// If the client passed apiStrict, they must also pass apiVersion.
assert.commandFailedWithCode(db.runCommand({ping: 1, apiStrict: true}),
                             4886600,
                             "Provided apiStrict without passing apiVersion");

// If the client passed apiDeprecationErrors, they must also pass apiVersion.
assert.commandFailedWithCode(db.runCommand({ping: 1, apiDeprecationErrors: false}),
                             4886600,
                             "Provided apiDeprecationErrors without passing apiVersion");

// If the client passed apiVersion, it must be of type string.
assert.commandFailedWithCode(db.runCommand({ping: 1, apiVersion: 1}),
                             ErrorCodes.TypeMismatch,
                             "apiVersion' is the wrong type 'double', expected type 'string'");

// If the client passed apiVersion, its value must be "1".
assert.commandFailedWithCode(db.runCommand({ping: 1, apiVersion: "2"}),
                             ErrorCodes.APIVersionError,
                             "API version must be \"1\"");

// If the client passed apiStrict, it must be of type boolean.
assert.commandFailedWithCode(db.runCommand({ping: 1, apiVersion: "1", apiStrict: "true"}),
                             ErrorCodes.TypeMismatch,
                             "apiStrict' is the wrong type 'string', expected type 'boolean'");

// If the client passed apiDeprecationErrors, it must be of type boolean.
assert.commandFailedWithCode(
    db.runCommand({ping: 1, apiVersion: "1", apiDeprecationErrors: "false"}),
    ErrorCodes.TypeMismatch,
    "apiDeprecationErrors' is the wrong type 'string', expected type 'boolean'");

// Sanity check that command works with proper parameters.
assert.commandWorked(
    db.runCommand({ping: 1, apiVersion: "1", apiStrict: true, apiDeprecationErrors: true}));
assert.commandWorked(
    db.runCommand({ping: 1, apiVersion: "1", apiStrict: false, apiDeprecationErrors: false}));
assert.commandWorked(db.runCommand({ping: 1, apiVersion: "1"}));
})();