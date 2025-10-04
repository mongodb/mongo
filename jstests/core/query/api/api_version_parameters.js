/**
 * Checks that the server properly parses "API Version" parameters
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: testDeprecation.
 *   not_allowed_with_signed_security_token,
 *   uses_api_parameters,
 *   no_selinux,
 * ]
 */

const testDb = db.getSiblingDB(jsTestName());

// Test parsing logic on command included in API V1.
// If the client passed apiStrict, they must also pass apiVersion.
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiStrict: true}),
    4886600,
    "Provided apiStrict without passing apiVersion",
);

// If the client passed apiDeprecationErrors, they must also pass apiVersion.
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiDeprecationErrors: false}),
    4886600,
    "Provided apiDeprecationErrors without passing apiVersion",
);

// If the client passed apiVersion, it must be of type string.
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiVersion: 1}),
    ErrorCodes.TypeMismatch,
    "apiVersion' is the wrong type 'double', expected type 'string'",
);

// If the client passed apiVersion, its value must be "1".
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiVersion: "2"}),
    ErrorCodes.APIVersionError,
    'API version must be "1"',
);

// If the client passed apiStrict, it must be of type boolean.
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiVersion: "1", apiStrict: "true"}),
    ErrorCodes.TypeMismatch,
    "apiStrict' is the wrong type 'string', expected type 'boolean'",
);

// If the client passed apiDeprecationErrors, it must be of type boolean.
assert.commandFailedWithCode(
    testDb.runCommand({ping: 1, apiVersion: "1", apiDeprecationErrors: "false"}),
    ErrorCodes.TypeMismatch,
    "apiDeprecationErrors' is the wrong type 'string', expected type 'boolean'",
);

// Sanity check that command works with proper parameters.
assert.commandWorked(testDb.runCommand({ping: 1, apiVersion: "1", apiStrict: true, apiDeprecationErrors: true}));
assert.commandWorked(testDb.runCommand({ping: 1, apiVersion: "1", apiStrict: false, apiDeprecationErrors: false}));
assert.commandWorked(testDb.runCommand({ping: 1, apiVersion: "1"}));

// Test parsing logic on command not included in API V1.
assert.commandWorked(testDb.runCommand({listCommands: 1, apiVersion: "1"}));
// If the client passed apiStrict: true, but the command is not in V1, reply with
// APIStrictError.
assert.commandFailedWithCode(
    testDb.runCommand({listCommands: 1, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError,
);
assert.commandFailedWithCode(
    testDb.runCommand({isMaster: 1, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError,
);
assert.commandWorked(testDb.runCommand({listCommands: 1, apiVersion: "1", apiDeprecationErrors: true}));

// Test parsing logic of command deprecated in API V1.
assert.commandWorked(testDb.runCommand({testDeprecation: 1, apiVersion: "1"}));
assert.commandWorked(testDb.runCommand({testDeprecation: 1, apiVersion: "1", apiStrict: true}));
// If the client passed apiDeprecationErrors: true, but the command is
// deprecated in API Version 1, reply with APIDeprecationError.
assert.commandFailedWithCode(
    testDb.runCommand({testDeprecation: 1, apiVersion: "1", apiDeprecationErrors: true}),
    ErrorCodes.APIDeprecationError,
    'Provided apiDeprecationErrors: true, but the invoked command\'s deprecatedApiVersions() does not include "1"',
);

// Assert APIStrictError message for unsupported commands contains link to docs site
let err = assert.commandFailedWithCode(
    testDb.runCommand({buildInfo: 1, apiStrict: true, apiVersion: "1"}),
    ErrorCodes.APIStrictError,
);
assert.includes(err.errmsg, "buildInfo");
assert.includes(err.errmsg, "dochub.mongodb.org");

// Test writing to system.js fails.
assert.commandFailedWithCode(
    testDb.runCommand({
        insert: "system.js",
        documents: [
            {
                _id: "shouldntExist",
                value: function () {
                    return 1;
                },
            },
        ],
        apiVersion: "1",
        apiStrict: true,
    }),
    ErrorCodes.APIStrictError,
    "Provided apiStrict:true, but the command insert attempts to write to system.js",
);
assert.commandFailedWithCode(
    testDb.runCommand({
        update: "system.js",
        updates: [
            {
                q: {
                    _id: "shouldExist",
                    value: function () {
                        return 1;
                    },
                },
                u: {
                    _id: "shouldExist",
                    value: function () {
                        return 2;
                    },
                },
            },
        ],
        apiVersion: "1",
        apiStrict: true,
    }),
    ErrorCodes.APIStrictError,
    "Provided apiStrict:true, but the command update attempts to write to system.js",
);
assert.commandFailedWithCode(
    testDb.runCommand({
        delete: "system.js",
        deletes: [
            {
                q: {
                    _id: "shouldExist",
                    value: function () {
                        return 1;
                    },
                },
                limit: 1,
            },
        ],
        apiVersion: "1",
        apiStrict: true,
    }),
    ErrorCodes.APIStrictError,
    "Provided apiStrict:true, but the command delete attempts to write to system.js",
);
assert.commandFailedWithCode(
    testDb.runCommand({
        findAndModify: "system.js",
        query: {
            _id: "shouldExist",
            value: function () {
                return 1;
            },
        },
        remove: true,
        apiVersion: "1",
        apiStrict: true,
    }),
    ErrorCodes.APIStrictError,
    "Provided apiStrict:true, but the command findAndModify attempts to write to system.js",
);
// Test reading from system.js succeeds.
assert.commandWorked(
    testDb.runCommand({find: "system.js", filter: {_id: "shouldExist"}, apiVersion: "1", apiStrict: true}),
);
