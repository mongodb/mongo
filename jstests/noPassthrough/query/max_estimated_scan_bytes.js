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
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
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

    function setDryRun(val) {
        assert.commandWorked(db.adminCommand({setParameter: 1, maxEstimatedScanBytesDryRun: val}));
    }

    function getCounters() {
        const ss = assert.commandWorked(db.adminCommand({serverStatus: 1}));
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
        setDryRun(false);
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

    it("13. aggregation pipeline with a leading $limit: allowed", function () {
        withParam(belowHeavy, () => {
            assert.commandWorked(
                db.runCommand({aggregate: "heavy", pipeline: [{$limit: 5}], cursor: {}}),
            );
        });
    });

    it("14. aggregation pipeline with $match then $limit: allowed", function () {
        withParam(belowHeavy, () => {
            assert.commandWorked(
                db.runCommand({
                    aggregate: "heavy",
                    pipeline: [{$match: {a: {$gte: 0}}}, {$limit: 5}],
                    cursor: {},
                }),
            );
        });
    });

    it("15. aggregation pipeline with $skip then $limit: allowed", function () {
        withParam(belowHeavy, () => {
            assert.commandWorked(
                db.runCommand({
                    aggregate: "heavy",
                    pipeline: [{$skip: 3}, {$limit: 5}],
                    cursor: {},
                }),
            );
        });
    });

    it("16. aggregation pipeline with $sort then $limit: allowed", function () {
        withParam(belowHeavy, () => {
            assert.commandWorked(
                db.runCommand({
                    aggregate: "heavy",
                    pipeline: [{$sort: {padding: 1}}, {$limit: 5}],
                    cursor: {},
                }),
            );
        });
    });

    // $unwind and $group can change the number of documents flowing out of the scan stage
    // relative to the number scanned, so a trailing $limit does not bound the underlying
    // COLLSCAN and these pipelines are correctly still rejected.
    it("17. aggregation pipeline with $unwind then $limit: rejected", function () {
        withParam(belowHeavy, () => {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: "heavy",
                    pipeline: [{$unwind: "$a"}, {$limit: 5}],
                    cursor: {},
                }),
                ErrorCodes.NoQueryExecutionPlans,
            );
        });
    });

    it("18. aggregation pipeline with $group then $limit: rejected", function () {
        withParam(belowHeavy, () => {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: "heavy",
                    pipeline: [{$group: {_id: "$a"}}, {$limit: 5}],
                    cursor: {},
                }),
                ErrorCodes.NoQueryExecutionPlans,
            );
        });
    });

    it("19. dryRun: query that would be rejected succeeds, and dryRunWouldReject increments", function () {
        withParam(belowHeavy, () => {
            setDryRun(true);
            const beforeDryRun = getCounters().dryRunWouldReject;
            const beforeRejected = getCounters().rejected;
            // Earlier tests may have cached a COLLSCAN plan for this exact query shape, which
            // would replay via the plan-cache path (LOGV2 10130231) instead of the collScanRequired
            // path (10130233) asserted on below. Clear the cache so the log ID is deterministic.
            assert.commandWorked(db.runCommand({planCacheClear: "heavy"}));
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}}));
            assert.eq(
                getCounters().dryRunWouldReject,
                beforeDryRun + 1,
                "dryRunWouldReject should increment when dry-run mode is on",
            );
            assert.eq(
                getCounters().rejected,
                beforeRejected,
                "rejected should NOT increment while dry-run mode is on",
            );
            checkLog.containsJson(conn, 10130233, {
                "namespace": "test.heavy",
            });
            setDryRun(false);
        });
    });

    it("20. dryRun: has no effect when maxEstimatedScanBytes is disabled", function () {
        withParam(-1, () => {
            setDryRun(true);
            const beforeDryRun = getCounters().dryRunWouldReject;
            assert.commandWorked(db.runCommand({find: "heavy", filter: {a: 1}}));
            assert.eq(
                getCounters().dryRunWouldReject,
                beforeDryRun,
                "dryRunWouldReject should not increment when maxEstimatedScanBytes is disabled",
            );
            setDryRun(false);
        });
    });

    it("21. dryRun: $lookup foreign collection that would be rejected succeeds under dry-run", function () {
        assertDropAndRecreateCollection(db, "local_lookup");
        assert.commandWorked(
            db["local_lookup"].insertMany([
                {_id: 1, k: 1},
                {_id: 2, k: 2},
            ]),
        );
        withParam(belowHeavy, () => {
            setDryRun(true);
            const beforeDryRun = getCounters().dryRunWouldReject;
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
            // The classic NLJ replans the foreign collection scan once per outer document (2
            // here), so dryRunWouldReject is recorded multiple times for one command; see the
            // identical note on the PQS $natural override $lookup test below.
            assert.gt(
                getCounters().dryRunWouldReject,
                beforeDryRun,
                "dryRunWouldReject should increment for a $lookup foreign collection under dry-run",
            );
            setDryRun(false);
        });
    });

    // TODO SERVER-130345: add integration tests for PQS $natural hint override path.
});

// setQuerySettings requires a replica set; it is not supported on a standalone mongod.
describe("maxEstimatedScanBytes with PQS $natural hint override", function () {
    let rst;
    let db;
    let qsutils;
    let heavyColl;
    let belowHeavy;
    let aboveHeavy;

    function setParam(val) {
        assert.commandWorked(db.adminCommand({setParameter: 1, maxEstimatedScanBytes: val}));
    }

    function setDryRun(val) {
        assert.commandWorked(db.adminCommand({setParameter: 1, maxEstimatedScanBytesDryRun: val}));
    }

    function getCounters() {
        const ss = assert.commandWorked(db.adminCommand({serverStatus: 1}));
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

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        db = rst.getPrimary().getDB("test");
        qsutils = new QuerySettingsUtils(db, "heavy_pqs");

        heavyColl = makeHeavyCollection("heavy_pqs");
        const heavySize = heavyColl.stats().size;
        assert.gt(heavySize, 0, "heavy_pqs collection must have non-zero dataSize");
        belowHeavy = heavySize - 1;
        aboveHeavy = heavySize * 10;
    });

    after(function () {
        setParam(-1);
        setDryRun(false);
        rst.stopSet();
    });

    it("PQS $natural hint overrides rejection on the main collection", function () {
        setParam(belowHeavy);
        const query = qsutils.makeFindQueryInstance({filter: {a: 1}});
        const ns = {db: db.getName(), coll: "heavy_pqs"};
        qsutils.withQuerySettings(
            query,
            {indexHints: [{ns, allowedIndexes: [{$natural: 1}]}]},
            () => {
                const beforeOverride = getCounters().rejectedAndOverridden;
                assert.commandWorked(db.runCommand({find: "heavy_pqs", filter: {a: 1}}));
                assert.eq(
                    getCounters().rejectedAndOverridden,
                    beforeOverride + 1,
                    "rejectedAndOverridden should increment for PQS $natural hint override",
                );
            },
        );
    });

    it("PQS $natural hint overrides rejection on a $lookup foreign collection", function () {
        assertDropAndRecreateCollection(db, "local_lookup_pqs");
        assert.commandWorked(
            db["local_lookup_pqs"].insertMany([
                {_id: 1, k: 1},
                {_id: 2, k: 2},
            ]),
        );
        setParam(belowHeavy);

        const cmd = {
            aggregate: "local_lookup_pqs",
            pipeline: [
                {
                    $lookup: {
                        from: "heavy_pqs",
                        localField: "k",
                        foreignField: "a",
                        as: "joined",
                    },
                },
            ],
            cursor: {},
        };
        // The query settings namespace targets the $lookup foreign collection, not the main
        // collection being aggregated.
        const ns = {db: db.getName(), coll: "heavy_pqs"};
        qsutils.withQuerySettings(
            {...cmd, $db: db.getName()},
            {indexHints: [{ns, allowedIndexes: [{$natural: 1}]}]},
            () => {
                const beforeOverride = getCounters().rejectedAndOverridden;
                assert.commandWorked(db.runCommand(cmd));
                // The classic NLJ replans the foreign collection scan once per outer document
                // (2 here), so the override is recorded multiple times for one command. The
                // override decision itself is stable: it does not re-evaluate live PQS/threshold
                // state per document, so an in-progress $lookup cannot flip from allowed to
                // rejected mid-execution.
                assert.gt(
                    getCounters().rejectedAndOverridden,
                    beforeOverride,
                    "rejectedAndOverridden should increment for PQS $natural hint override on $lookup foreign collection",
                );
            },
        );
    });

    it("PQS $natural hint on the main collection does not override rejection on a $lookup foreign collection", function () {
        assertDropAndRecreateCollection(db, "local_lookup_pqs");
        assert.commandWorked(
            db["local_lookup_pqs"].insertMany([
                {_id: 1, k: 1},
                {_id: 2, k: 2},
            ]),
        );
        setParam(belowHeavy);

        const cmd = {
            aggregate: "local_lookup_pqs",
            pipeline: [
                {
                    $lookup: {
                        from: "heavy_pqs",
                        localField: "k",
                        foreignField: "a",
                        as: "joined",
                    },
                },
            ],
            cursor: {},
        };
        // The query settings namespace targets the main aggregated collection, not the $lookup
        // foreign collection that actually requires the unbounded COLLSCAN, so the foreign scan is
        // still rejected.
        const ns = {db: db.getName(), coll: "local_lookup_pqs"};
        qsutils.withQuerySettings(
            {...cmd, $db: db.getName()},
            {indexHints: [{ns, allowedIndexes: [{$natural: 1}]}]},
            () => {
                assert.commandFailedWithCode(
                    db.runCommand(cmd),
                    ErrorCodes.NoQueryExecutionPlans,
                    "PQS $natural hint on the main collection should not override rejection on the $lookup foreign collection",
                );
            },
        );
    });

    it("PQS index hint on a non-existent index does not override rejection", function () {
        setParam(belowHeavy);
        const query = qsutils.makeFindQueryInstance({filter: {a: 1}});
        const ns = {db: db.getName(), coll: "heavy_pqs"};
        qsutils.withQuerySettings(
            query,
            {indexHints: [{ns, allowedIndexes: [{doesNotExist: 1}]}]},
            () => {
                assert.commandFailedWithCode(
                    db.runCommand({find: "heavy_pqs", filter: {a: 1}}),
                    ErrorCodes.NoQueryExecutionPlans,
                    "a PQS hint on a non-existent index is not a $natural override and should not exempt the query from rejection",
                );
            },
        );
    });

    it("PQS queryKnobs can lower maxEstimatedScanBytes for a specific query shape", function () {
        setParam(-1);
        const targetShape = qsutils.makeFindQueryInstance({filter: {a: 1}});
        qsutils.withQuerySettings(
            targetShape,
            {queryKnobs: {maxEstimatedScanBytes: belowHeavy}},
            () => {
                assert.commandFailedWithCode(
                    db.runCommand({find: "heavy_pqs", filter: {a: 1}}),
                    ErrorCodes.NoQueryExecutionPlans,
                    "the targeted query shape should be rejected by the per-shape maxEstimatedScanBytes override",
                );
                // A different query shape is untouched by the per-shape override and still uses the
                // globally disabled (-1) threshold.
                assert.commandWorked(
                    db.runCommand({find: "heavy_pqs", filter: {b: 1}}),
                    "a differently-shaped query should not be affected by the per-shape override",
                );
            },
        );
    });

    it("PQS queryKnobs can raise maxEstimatedScanBytes for a specific query shape", function () {
        setParam(belowHeavy);
        const targetShape = qsutils.makeFindQueryInstance({filter: {a: 1}});
        qsutils.withQuerySettings(
            targetShape,
            {queryKnobs: {maxEstimatedScanBytes: aboveHeavy}},
            () => {
                assert.commandWorked(
                    db.runCommand({find: "heavy_pqs", filter: {a: 1}}),
                    "the targeted query shape should be exempted by the per-shape maxEstimatedScanBytes override",
                );
                // A different query shape is untouched by the per-shape override and still uses the
                // globally-set (low) threshold.
                assert.commandFailedWithCode(
                    db.runCommand({find: "heavy_pqs", filter: {b: 1}}),
                    ErrorCodes.NoQueryExecutionPlans,
                    "a differently-shaped query should still be rejected by the global threshold",
                );
            },
        );
    });

    it("PQS $natural hint override also suppresses dry-run: dryRunWouldReject does not increment", function () {
        setParam(belowHeavy);
        setDryRun(true);
        const query = qsutils.makeFindQueryInstance({filter: {a: 1}});
        const ns = {db: db.getName(), coll: "heavy_pqs"};
        qsutils.withQuerySettings(
            query,
            {indexHints: [{ns, allowedIndexes: [{$natural: 1}]}]},
            () => {
                const beforeDryRun = getCounters().dryRunWouldReject;
                assert.commandWorked(db.runCommand({find: "heavy_pqs", filter: {a: 1}}));
                assert.eq(
                    getCounters().dryRunWouldReject,
                    beforeDryRun,
                    "dryRunWouldReject should NOT increment when the PQS $natural hint override " +
                        "clears COLLECTION_EXCEEDS_SCAN_BYTES before the dry-run check runs",
                );
            },
        );
        setDryRun(false);
    });
});
