/**
 * Verifies that benchRun() fails in the following cases.
 *
 * - No readCmd param or readCmd: false is specified for read ops
 * - No writeCmd param or writeCmd: false is specified for write ops
 * - Exhaust query is requested
 * The test runs commands that are not allowed with security token: benchRun.
 * @tags: [not_allowed_with_security_token]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
testDB.dropDatabase();
const coll = testDB.coll;

// Composes the common benchRun() args.
const benchArgs = {
    ops: [],
    parallel: 1,
    seconds: 1,
    host: db.getMongo().host
};
if (jsTest.options().auth) {
    benchArgs["db"] = "admin";
    benchArgs["username"] = jsTest.options().authUser;
    benchArgs["password"] = jsTest.options().authPassword;
}

const readCmdParamError = 5751400;
const writeCmdParamError = 5751401;
const exhaustOptionError = 5751402;

const opsAndErrors = [
    // No writeCmd param for insert op will get an error.
    {op: {ns: coll.getFullName(), op: "insert", doc: [{_id: 0, a: 1}]}, error: writeCmdParamError},
    // The writeCmd: false for update op will get an error.
    {
        op: {ns: coll.getFullName(), op: "insert", doc: [{_id: 0, a: 1}], writeCmd: false},
        error: writeCmdParamError
    },
    // No writeCmd param for update op will get an error.
    {
        op: {
            ns: coll.getFullName(),
            op: "update",
            query: {_id: 0},
            update: {$inc: {a: 1}},
        },
        error: writeCmdParamError
    },
    // The writeCmd: false for update op will get an error.
    {
        op: {
            ns: coll.getFullName(),
            op: "update",
            query: {_id: 0},
            update: {$inc: {a: 1}},
            writeCmd: false
        },
        error: writeCmdParamError
    },
    // No writeCmd param for delete op will get an error.
    {
        op: {
            ns: coll.getFullName(),
            op: "delete",
            query: {_id: 0},
        },
        error: writeCmdParamError
    },
    // The writeCmd: false for delete op will get an error.
    {
        op: {ns: coll.getFullName(), op: "delete", query: {_id: 0}, writeCmd: false},
        error: writeCmdParamError
    },
    // No readCmd param for findOne op will get an error.
    {op: {ns: coll.getFullName(), op: "findOne", query: {}}, error: readCmdParamError},
    // The readCmd: false for findOne op will get an error.
    {
        op: {ns: coll.getFullName(), op: "findOne", query: {}, readCmd: false},
        error: readCmdParamError
    },
    // No readCmd param for find op will get an error.
    {op: {ns: coll.getFullName(), op: "find", query: {}}, error: readCmdParamError},
    // The readCmd: false for find op will get an error.
    {op: {ns: coll.getFullName(), op: "find", query: {}, readCmd: false}, error: readCmdParamError},
    // Exhaust query for read op is not supported for benchRun().
    {
        op: {
            ns: coll.getFullName(),
            op: "find",
            query: {},
            options: DBQuery.Option.exhaust,
            readCmd: true
        },
        error: exhaustOptionError
    },
    // Exhaust query for command op is not supported for benchRun().
    {
        op: {
            ns: testDB.getName(),
            op: "command",
            command: {"find": coll.getName()},
            options: DBQuery.Option.exhaust
        },
        error: exhaustOptionError
    },
];

opsAndErrors.forEach(opAndError => {
    benchArgs.ops = [opAndError.op];

    assert.throwsWithCode(() => benchRun(benchArgs), opAndError.error);
});
})();
