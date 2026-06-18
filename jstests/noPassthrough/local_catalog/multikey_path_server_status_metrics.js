/**
 * Tests serverStatus counters for newly recorded ordinary multikey path changes.
 *
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();

let rst;
let primary;
let testDB;

const kMetricPaths = [
    {name: "ordinary.inTransaction", fields: ["newPaths", "ordinary", "inTransaction"]},
    {name: "ordinary.outsideTransaction", fields: ["newPaths", "ordinary", "outsideTransaction"]},
    {name: "sideTransactions", fields: ["sideTransactions"]},
];

// Walks a nested object following the given field names and returns the value at that path.
// e.g. getPath({newPaths: {ordinary: {inTransaction: 5}}}, ["newPaths", "ordinary", "inTransaction"])
// returns 5.
function getMetricValue(obj, fields) {
    return fields.reduce((current, field) => current[field], obj);
}

function getMultikeyPathChangeMetrics(conn) {
    const serverStatus = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    const metrics = serverStatus.indexStats?.multikey;
    assert(metrics, "Missing multikey path change metrics in serverStatus", {serverStatus});
    return metrics;
}

function assertMetricDeltas(conn, expectedDeltas, action) {
    const before = getMultikeyPathChangeMetrics(conn);
    action();
    const after = getMultikeyPathChangeMetrics(conn);

    for (const metric of kMetricPaths) {
        const expectedDelta = expectedDeltas[metric.name] ?? 0;
        const actualDelta =
            Number(getMetricValue(after, metric.fields)) -
            Number(getMetricValue(before, metric.fields));
        assert.eq(
            expectedDelta,
            actualDelta,
            "Unexpected multikey path serverStatus metric delta",
            {
                metric: metric.name,
                before,
                after,
            },
        );
    }
}

function runTransaction(conn, action) {
    const session = conn.startSession();
    try {
        session.startTransaction();
        action(session.getDatabase(dbName));
        assert.commandWorked(session.commitTransaction_forTesting());
    } finally {
        session.endSession();
    }
}

function runPreparedTransaction(conn, action) {
    const session = conn.startSession();
    try {
        session.startTransaction();
        action(session.getDatabase(dbName));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    } finally {
        session.endSession();
    }
}

function createIndexedCollection(conn, collName, keyPattern) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    assert.commandWorked(db.runCommand({create: collName}));
    assert.commandWorked(coll.createIndex(keyPattern));
    return coll;
}

describe("multikey path serverStatus metrics", function () {
    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        testDB = primary.getDB(dbName);
    });

    beforeEach(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    after(function () {
        if (rst) {
            rst.stopSet();
            rst = null;
        }
    });

    it("counts ordinary multikey path changes outside transactions", function () {
        const coll = createIndexedCollection(primary, "ordinary_outside_txn", {a: 1, b: 1});

        assertMetricDeltas(primary, {"ordinary.outsideTransaction": 1}, function () {
            assert.commandWorked(coll.insert({_id: 1, a: [1, 2], b: 1}));
            assert.commandWorked(coll.insert({_id: 2, a: [3, 4], b: 1}));
        });
    });

    it("counts a shared multikey field once per index", function () {
        const db = primary.getDB(dbName);
        const collName = "ordinary_shared_field";
        assert.commandWorked(db.runCommand({create: collName}));
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));
        assert.commandWorked(coll.createIndex({b: 1, c: 1}));

        // Field 'b' is indexed by both {a:1, b:1} and {b:1, c:1}. Making 'b' multikey records one
        // new path component per index, so the shared field is accounted as 2 new paths: the
        // counter has (index, path component) granularity, not distinct-field granularity.
        assertMetricDeltas(primary, {"ordinary.outsideTransaction": 2}, function () {
            assert.commandWorked(coll.insert({_id: 1, a: 1, b: [1, 2], c: 1}));
        });
    });

    it("counts ordinary multikey path changes inside unprepared transactions", function () {
        createIndexedCollection(primary, "ordinary_in_unprepared_txn", {a: 1, b: 1});

        assertMetricDeltas(
            primary,
            {
                "ordinary.inTransaction": 1,
                sideTransactions: 1,
            },
            function () {
                runTransaction(primary, function (sessionDB) {
                    assert.commandWorked(
                        sessionDB
                            .getCollection("ordinary_in_unprepared_txn")
                            .insert({_id: 1, a: [1, 2], b: 1}),
                    );
                });
            },
        );
    });

    it("counts ordinary multikey path changes inside prepared transactions", function () {
        createIndexedCollection(primary, "ordinary_in_prepared_txn", {a: 1, b: 1});

        assertMetricDeltas(
            primary,
            {
                "ordinary.inTransaction": 1,
                sideTransactions: 1,
            },
            function () {
                runPreparedTransaction(primary, function (sessionDB) {
                    assert.commandWorked(
                        sessionDB
                            .getCollection("ordinary_in_prepared_txn")
                            .insert({_id: 1, a: [1, 2], b: 1}),
                    );
                });
            },
        );
    });

    it("counts two ordinary path changes and side transactions in one transaction", function () {
        createIndexedCollection(primary, "ordinary_in_txn_two_paths", {a: 1, b: 1});

        assertMetricDeltas(
            primary,
            {
                "ordinary.inTransaction": 2,
                sideTransactions: 2,
            },
            function () {
                runTransaction(primary, function (sessionDB) {
                    const coll = sessionDB.getCollection("ordinary_in_txn_two_paths");
                    assert.commandWorked(coll.insert({_id: 1, a: [1, 2], b: 1}));
                    assert.commandWorked(coll.insert({_id: 2, a: 1, b: [2, 3]}));
                });
            },
        );
    });

    it("counts three side transactions across separate transactions", function () {
        createIndexedCollection(primary, "ordinary_in_txn_three_txns", {a: 1, b: 1, c: 1});

        assertMetricDeltas(
            primary,
            {
                "ordinary.inTransaction": 3,
                sideTransactions: 3,
            },
            function () {
                const collName = "ordinary_in_txn_three_txns";
                runTransaction(primary, function (sessionDB) {
                    assert.commandWorked(
                        sessionDB.getCollection(collName).insert({_id: 1, a: [1, 2], b: 1, c: 1}),
                    );
                });
                runTransaction(primary, function (sessionDB) {
                    assert.commandWorked(
                        sessionDB.getCollection(collName).insert({_id: 2, a: 1, b: [2, 3], c: 1}),
                    );
                });
                runTransaction(primary, function (sessionDB) {
                    assert.commandWorked(
                        sessionDB.getCollection(collName).insert({_id: 3, a: 1, b: 1, c: [4, 5]}),
                    );
                });
            },
        );
    });
});
