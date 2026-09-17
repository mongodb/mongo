// Tests that change stream updateLookup on a sharded collection handles documents whose shard key
// contains dollar-prefixed field names across all three single-document-lookup engines:
// - Aggregation (the universal fallback).
// - SBE (used by default for collection-level change streams).
// - Express (used by default for database- and cluster-level change streams).
//
// The test pins the engine via a failpoint, verifies that the selected engine is the one that
// resolves the lookup via serverStatus metrics, and confirms that updateLookup returns the
// fully updated document.
//
// @tags: [
//   assumes_balancer_off,
//   # If a rollback is triggered during a stepdown, the change stream cursor can become invalid.
//   does_not_support_stepdowns,
//   requires_fcv_90,
//   requires_majority_read_concern,
//   featureFlagChangeStreamOptimizedUpdateLookup,
//   uses_change_streams,
// ]
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {
    readUpdateLookupDelta,
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const runWithFailPointOnShards = (st, failPointName, data = {}, cb) => {
    // The updateLookup stage runs on the shards, so the engine-selection failpoint only needs to
    // be active there. Mongos does not run the optimized local-lookup path, so forcing SBE/Express
    // on it would fall back to aggregation and pollute the per-shard metrics we are checking.
    const failPoints = [
        configureFailPointForRS(st.rs0.nodes, failPointName, data, "alwaysOn"),
        configureFailPointForRS(st.rs1.nodes, failPointName, data, "alwaysOn"),
    ];
    try {
        return cb();
    } finally {
        failPoints.forEach((fp) => fp.off());
    }
};

/**
 * Asserts that the requested single-document-lookup engine was used to resolve the updateLookup
 * lookups in the metrics delta, and that the other engines were not involved.
 */
function assertEngineUsed(delta, engine, expectedFound) {
    const byEngine = readUpdateLookupDelta(delta);

    const selected = byEngine[engine];
    assert.eq(selected.found, expectedFound, {engine, delta});
    assert.eq(selected.notHandled, 0, {engine, delta});

    for (const other of Object.values(UpdateLookupExecutor)) {
        if (other === engine) {
            continue;
        }
        const stats = byEngine[other];
        assert.eq(stats.found, 0, {other, engine, delta});
        assert.eq(stats.notFound, 0, {other, engine, delta});
    }
}

describe("change stream updateLookup engines with dollar-prefixed shard key", () => {
    let st;
    let db;
    let coll;

    before(() => {
        st = new ShardingTest({
            shards: 2,
            rs: {
                nodes: 1,
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
            },
        });

        db = st.s0.getDB(jsTestName());

        // Ensure the test db primary is st.shard0.shardName.
        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: st.rs0.getURL()}),
        );

        coll = db[jsTestName()];
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        // Shard the test collection on 'sk'.
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));
    });

    afterEach(() => {
        assertDropCollection(db, coll.getName());
    });

    it("prohibits storing an _id field with dollar-prefixed sub-fields", () => {
        assert.commandFailedWithCode(
            coll.insert({_id: {$v: 2}, sk: 1, val: "original"}),
            ErrorCodes.DollarPrefixedFieldName,
        );
    });

    // The three engines under test. SBE is the default for collection-level streams, Express for
    // database-/cluster-level streams, and Aggregation is the fallback path.
    const engines = [
        {
            name: UpdateLookupExecutor.kAggregation,
            // The aggregation engine can be exercised at any change-stream level. Use a collection
            // stream here to line up with the SBE case.
            getChangeStream: (startTime) => {
                return coll.watch([], {
                    startAtOperationTime: startTime,
                    fullDocument: "updateLookup",
                });
            },
        },
        {
            name: UpdateLookupExecutor.kSBE,
            getChangeStream: (startTime) => {
                return coll.watch([], {
                    startAtOperationTime: startTime,
                    fullDocument: "updateLookup",
                });
            },
        },
        {
            name: UpdateLookupExecutor.kExpress,
            // Express is used for database-level (and cluster-level) streams. Restrict the stream
            // to the test collection so the assertions only see events for the document under test.
            getChangeStream: (startTime) => {
                return db.watch([{$match: {"ns.coll": coll.getName()}}], {
                    startAtOperationTime: startTime,
                    fullDocument: "updateLookup",
                });
            },
        },
    ];

    // Each engine is tested with a local lookup (document and update oplog entry on the same
    // shard). The aggregation engine is additionally tested with a remote lookup (document moved to
    // a different shard from the update oplog entry) because it is the universal fallback and must
    // handle that scenario.
    const testCases = [
        // Local lookup: document stays on the shard that owns its chunk.
        {
            name: "local lookup",
            doc: {_id: 1, sk: {$type: 2}, val: "original"},
            prepare: () => {},
            remote: false,
        },
        // Remote lookup: move both chunks to shard1. The document is now on shard1, but the
        // oplog entries are on the primary shard (shard0). Only the aggregation engine is expected
        // to handle this case.
        {
            name: "remote lookup",
            doc: {_id: 1, sk: {$type: 2}, val: "original"},
            prepare: () => {
                [-1, 1].forEach((splitPoint) => {
                    assert.commandWorked(
                        db.adminCommand({
                            moveChunk: coll.getFullName(),
                            find: {sk: {test: splitPoint}},
                            to: st.rs1.getURL(),
                            _waitForDelete: true,
                        }),
                    );
                });
            },
            remote: true,
        },
        // A dollar-prefixed field name that does not exist as an MQL operator.
        {
            name: "unknown operator",
            doc: {_id: 1, sk: {$operatorDoesNotExist: 2}, val: "original"},
            prepare: () => {},
            remote: false,
        },
    ];

    for (const engineConfig of engines) {
        for (const testCase of testCases) {
            // Skip the remote lookup for SBE and Express: those engines only perform local
            // single-document lookups and fall back to aggregation for remote cases, so a remote
            // test would not exercise the selected engine.
            if (testCase.remote && engineConfig.name !== UpdateLookupExecutor.kAggregation) {
                // Need to skip here because we are setting the engine directly via the failpoint,
                // with no fallback. Getting into the fallback case would trigger a tassert on the
                // server because no fallback is configured.
                continue;
            }

            it(`returns expected full document via ${engineConfig.name}, ${testCase.name}`, () => {
                const startTime = getClusterTime(db);

                assert.commandWorked(coll.insert(testCase.doc));

                // Confirm the document can be fetched directly by _id; this also proves the
                // dollar-prefixed shard-key value is stored correctly.
                const lookup = coll.find({_id: testCase.doc._id}).toArray();
                assert.eq(1, lookup.length, {lookup});
                assert.docEq(testCase.doc, lookup[0]);

                assert.commandWorked(
                    coll.update({_id: testCase.doc._id}, {$set: {val: "changed"}}),
                );

                const expectedFullDoc = Object.assign({}, testCase.doc, {val: "changed"});

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(db, () => {
                    runWithFailPointOnShards(
                        st,
                        "forceChangeStreamUpdateLookupEngine",
                        {engine: engineConfig.name},
                        () => {
                            testCase.prepare();

                            const changeStream = engineConfig.getChangeStream(startTime);

                            try {
                                assert.soon(() => changeStream.hasNext());
                                let next = changeStream.next();
                                assert.eq(next.operationType, "insert", {next});
                                assert.docEq(testCase.doc, next.fullDocument);

                                assert.soon(() => changeStream.hasNext());
                                next = changeStream.next();
                                assert.eq(next.operationType, "update", {next});
                                assert.docEq(expectedFullDoc, next.fullDocument);
                            } finally {
                                changeStream.close();
                            }
                        },
                    );
                });

                // The selected engine should have resolved exactly one lookup (the update event).
                assertEngineUsed(delta, engineConfig.name, /*expectedFound=*/ 1);
            });
        }
    }
});
