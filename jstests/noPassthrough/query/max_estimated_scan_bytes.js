/**
 * Tests for the maxEstimatedScanBytes server parameter.
 *
 * This parameter rejects queries at plan time when the plan requires an unbounded COLLSCAN
 * on a collection whose data size exceeds the configured byte threshold.
 *
 * @tags: [requires_wiredtiger]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("maxEstimatedScanBytes", function () {
    let conn;
    let db;
    let heavyColl;
    let heavySize;
    let belowHeavy;
    let aboveHeavy;

    function withParam(val, fn) {
        return runWithParamsAllNonConfigNodes(db, {maxEstimatedScanBytes: val}, fn);
    }

    function getCounters() {
        const ss = db.adminCommand({serverStatus: 1});
        return ss.metrics.query.maxEstimatedScanBytes;
    }

    function makeHeavyCollection(collName) {
        assertDropAndRecreateCollection(db, collName);
        const coll = db[collName];
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            bulk.insert({a: i, padding: "x".repeat(1024)});
        }
        assert.commandWorked(bulk.execute());
        return coll;
    }

    function dataSize(coll) {
        return coll.stats().size;
    }

    before(function () {
        conn = MongoRunner.runMongod({});
        db = conn.getDB("test");

        heavyColl = makeHeavyCollection("heavy");
        heavySize = dataSize(heavyColl);
        assert.gt(heavySize, 0, "heavy collection must have non-zero dataSize");

        belowHeavy = heavySize - 1;
        aboveHeavy = heavySize * 10;
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("1. parameter disabled (default -1): no effect", function () {
        withParam(-1, () => {
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}}));
        });
    });

    it("2. collection above threshold: unbounded COLLSCAN rejected", function () {
        withParam(belowHeavy, () => {
            const beforeRejected = getCounters().rejected;
            assert.commandFailedWithCode(
                db.runCommand({find: "heavy", filter: {a: 1}}),
                ErrorCodes.NoQueryExecutionPlans,
                "unbounded COLLSCAN on oversized collection should be rejected",
            );
            assert.eq(
                getCounters().rejected,
                beforeRejected + 1,
                "rejected counter should increment",
            );
        });
    });

    it("3. collection below threshold: COLLSCAN allowed", function () {
        withParam(aboveHeavy, () => {
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}}));
        });
    });

    it("4. collection above threshold but query has a limit: COLLSCAN allowed", function () {
        withParam(belowHeavy, () => {
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}, limit: 5}));
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}, limit: 1}));
        });
    });

    it("5. indexed plan: allowed even on large collection", function () {
        assert.commandWorked(heavyColl.createIndex({a: 1}));
        withParam(belowHeavy, () => {
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}}));
        });
        assert.commandWorked(heavyColl.dropIndex({a: 1}));
    });

    it("6. threshold = 0: any non-empty collection is rejected unless limit present", function () {
        withParam(0, () => {
            assert.commandFailedWithCode(
                db.runCommand({find: "heavy", filter: {a: 1}}),
                ErrorCodes.NoQueryExecutionPlans,
            );
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}, limit: 1}));
        });
    });

    it("7. $natural hint in command: overrides the rejection", function () {
        withParam(belowHeavy, () => {
            const beforeOverride = getCounters().rejectedAndOverridden;
            assert.commandWorked(
                db.runCommand({find: "heavy", filter: {a: 1}, hint: {$natural: 1}}),
            );
            assert.eq(
                getCounters().rejectedAndOverridden,
                beforeOverride + 1,
                "rejectedAndOverridden should increment for $natural command hint",
            );
        });
    });

    it("7b. $natural: -1 hint in command: overrides the rejection (backward scan)", function () {
        withParam(belowHeavy, () => {
            const beforeOverride = getCounters().rejectedAndOverridden;
            assert.commandWorked(
                db.runCommand({find: "heavy", filter: {a: 1}, hint: {$natural: -1}}),
            );
            assert.eq(
                getCounters().rejectedAndOverridden,
                beforeOverride + 1,
                "rejectedAndOverridden should increment for $natural: -1 command hint",
            );
        });
    });

    it("7c. $natural hint with limit: does not count as an override (limit would pass anyway)", function () {
        withParam(belowHeavy, () => {
            const beforeOverride = getCounters().rejectedAndOverridden;
            assert.commandWorked(
                db.runCommand({find: "heavy", filter: {a: 1}, hint: {$natural: 1}, limit: 5}),
            );
            assert.eq(
                getCounters().rejectedAndOverridden,
                beforeOverride,
                "rejectedAndOverridden should not increment when limit is present",
            );
        });
    });

    it("8. internal namespace: always allowed", function () {
        withParam(0, () => {
            const adminDb = conn.getDB("admin");
            assert.commandWorked(adminDb.runCommand({find: "system.version", filter: {}}));
        });
    });

    it("9. empty-predicate find on large collection: rejected (no exemption)", function () {
        withParam(belowHeavy, () => {
            assert.commandFailedWithCode(
                db.runCommand({find: "heavy", filter: {}}),
                ErrorCodes.NoQueryExecutionPlans,
                "empty-predicate find should be rejected (no exemption unlike notablescan)",
            );
        });
    });

    it("10. tailable cursor on large collection: rejected", function () {
        const cappedName = "cappedColl";
        assertDropAndRecreateCollection(db, cappedName, {capped: true, size: 1024 * 1024 * 10});
        const cappedColl = db[cappedName];
        const cappedBulk = cappedColl.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            cappedBulk.insert({a: i, padding: "x".repeat(1024)});
        }
        assert.commandWorked(cappedBulk.execute());
        const cappedSize = dataSize(cappedColl);
        withParam(cappedSize - 1, () => {
            assert.commandFailedWithCode(
                db.runCommand({find: cappedName, filter: {}, tailable: true}),
                ErrorCodes.NoQueryExecutionPlans,
                "tailable cursor on oversized capped collection should be rejected",
            );
        });
        db[cappedName].drop();
    });

    it("10b. large collection on an internal db (e.g. local, which holds the oplog change streams tail): not rejected", function () {
        const localDb = conn.getDB("local");
        assertDropAndRecreateCollection(localDb, "heavyLocal");
        const localColl = localDb.heavyLocal;
        const localBulk = localColl.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            localBulk.insert({a: i, padding: "x".repeat(1024)});
        }
        assert.commandWorked(localBulk.execute());
        const localSize = dataSize(localColl);
        withParam(localSize - 1, () => {
            assert.commandWorked(localDb.runCommand({find: "heavyLocal", filter: {}}));
        });
        localColl.drop();
    });

    it("11. $lookup where foreign collection exceeds threshold: rejected", function () {
        const localColl = db["local_lookup"];
        assertDropAndRecreateCollection(db, "local_lookup");
        assert.commandWorked(
            localColl.insertMany([
                {_id: 1, k: 1},
                {_id: 2, k: 2},
            ]),
        );
        withParam(belowHeavy, () => {
            const beforeLookupRejected = getCounters().rejected;
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: "local_lookup",
                    pipeline: [
                        {
                            $lookup: {
                                from: "heavy",
                                localField: "k",
                                foreignField: "a",
                                as: "joined",
                            },
                        },
                    ],
                    cursor: {},
                }),
                ErrorCodes.NoQueryExecutionPlans,
                "$lookup requiring COLLSCAN on oversized foreign collection should be rejected",
            );
            assert.eq(
                getCounters().rejected,
                beforeLookupRejected + 1,
                "rejected counter should increment for $lookup rejection",
            );
        });
    });

    it("12. $lookup where foreign collection has an index: allowed", function () {
        assertDropAndRecreateCollection(db, "local_lookup");
        assert.commandWorked(
            db["local_lookup"].insertMany([
                {_id: 1, k: 1},
                {_id: 2, k: 2},
            ]),
        );
        assert.commandWorked(heavyColl.createIndex({a: 1}));
        withParam(belowHeavy, () => {
            assert.commandWorked(
                db.runCommand({
                    aggregate: "local_lookup",
                    pipeline: [
                        {
                            $lookup: {
                                from: "heavy",
                                localField: "k",
                                foreignField: "a",
                                as: "joined",
                            },
                        },
                    ],
                    cursor: {},
                }),
            );
        });
        assert.commandWorked(heavyColl.dropIndex({a: 1}));
    });

    // TODO SERVER-130345: add integration tests for PQS $natural hint override path.
});
