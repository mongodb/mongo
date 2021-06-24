/**
 * Verifies that readPrefMode param works for find/fineOne/query ops in benchRun().
 *
 * @tags: [requires_replication]
 */

(function() {
"use strict";

const rs = new ReplSetTest({nodes: 2});
rs.startSet();
rs.initiate();

const primary = rs.getPrimary();
const secondary = rs.getSecondary();
const collName = primary.getDB(jsTestName()).getCollection("coll").getFullName();

const verifyNoError = res => {
    assert.eq(res.errCount, 0);
    assert.gt(res.totalOps, 0);
};

const benchArgArray = [
    {
        ops: [{op: "find", readCmd: true, query: {}, ns: collName, readPrefMode: "primary"}],
        parallel: 1,
        host: primary.host
    },
    {
        ops: [{
            op: "findOne",
            readCmd: true,
            query: {},
            ns: collName,
            readPrefMode: "primaryPreferred"
        }],
        parallel: 1,
        host: primary.host
    },
    {
        ops: [{op: "find", readCmd: true, query: {}, ns: collName, readPrefMode: "secondary"}],
        parallel: 1,
        host: secondary.host
    },
    {
        ops: [{
            op: "findOne",
            readCmd: true,
            query: {},
            ns: collName,
            readPrefMode: "secondaryPreferred"
        }],
        parallel: 1,
        host: secondary.host
    },
    {
        ops: [{op: "query", readCmd: true, query: {}, ns: collName, readPrefMode: "nearest"}],
        parallel: 1,
        host: secondary.host
    },
];

benchArgArray.forEach(benchArg => verifyNoError(benchRun(benchArg)));

const invalidArgAndError = [
    {
        benchArg: {
            ops: [{op: "find", readCmd: true, query: {}, ns: collName, readPrefMode: 1}],
            parallel: 1,
            host: primary.host
        },
        error: ErrorCodes.BadValue
    },
    {
        benchArg: {
            ops:
                [{op: "find", readCmd: true, query: {}, ns: collName, readPrefMode: "invalidPref"}],
            parallel: 1,
            host: primary.host
        },
        error: ErrorCodes.BadValue
    },
    {
        benchArg: {
            ops: [
                {op: "insert", writeCmd: true, doc: {a: 1}, ns: collName, readPrefMode: "primary"}
            ],
            parallel: 1,
            host: primary.host
        },
        error: ErrorCodes.InvalidOptions
    },
];

invalidArgAndError.forEach(argAndError => {
    const res = assert.throws(() => benchRun(argAndError.benchArg));
    assert.commandFailedWithCode(res, argAndError.error);
});

rs.stopSet();
})();
