/**
 * Change stream updateLookup cases that need to pause mid-enrichBatch() via the
 * 'hangBeforeChangeStreamUpdateLookup' fail point: DDL landing between two lookups that share
 * one cached acquisition, and killOp while cached executor/acquisition state is held. Both
 * orchestrate a getMore from a parallel shell and assert exact batch counts, which passthrough
 * overrides (transactions, sessions, per-shard cursors, read preference) perturb, so this file
 * lives in noPassthrough and runs against its own replica set.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertCreateCollection,
    assertDropCollection,
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {before, beforeEach, afterEach, after, describe, it} from "jstests/libs/mochalite.js";
import {
    expectedUpdateLookupEngine,
    readUpdateLookupDelta,
    ServerStatusMetrics,
} from "jstests/libs/query/change_stream_metrics_util.js";

describe("change stream updateLookup mid-batch", function () {
    let rst;

    before(function () {
        rst = new ReplSetTest({
            name: jsTestName(),
            nodes: 1,
            nodeOptions: {
                setParameter: {featureFlagChangeStreamOptimizedUpdateLookup: true},
            },
        });
        rst.startSet();
        rst.initiate();
    });

    after(function () {
        rst.stopSet();
    });

    // DDL landing between two lookups that share one cached acquisition, only reachable by
    // pausing mid-enrichBatch() via 'hangBeforeChangeStreamUpdateLookup'.
    describe("DDL", function () {
        const testDB = () => rst.getPrimary().getDB(jsTestName() + "_midbatch");

        const collNameA = "collA";
        const collNameB = "collB";

        beforeEach(function () {
            assertDropAndRecreateCollection(testDB(), collNameA);
        });

        afterEach(function () {
            assertDropCollection(testDB(), collNameA);
            assertDropCollection(testDB(), collNameB);
        });

        for (const matchCollectionUUIDForUpdateLookup of [false, true]) {
            // matchCollectionUUIDForUpdateLookup does not change the outcome here, and that
            // is by design: the batch's collection acquisition (including the UUID it
            // pinned) is taken when the batch starts, so a DDL landing between two lookups
            // of that batch is not observed until the next batch reacquires.
            it(`stale cached acquisition after DDL lands mid-batch [matchCollectionUUIDForUpdateLookup=${matchCollectionUUIDForUpdateLookup}]`, function () {
                const db = testDB();
                const testColl = db.getCollection(collNameA);
                assert.writeOK(
                    testColl.insert([
                        {_id: 1, a: 0},
                        {_id: 2, a: 0},
                        {_id: 3, a: 0},
                    ]),
                );

                const cursor = testColl.watch([], {
                    fullDocument: "updateLookup",
                    matchCollectionUUIDForUpdateLookup,
                    cursor: {batchSize: 0},
                    querySettings: {queryKnobs: {changeStreamUpdateLookupMaxBatchSize: 100}},
                });

                // Only the updates are streamed (inserts predate the cursor). The first
                // fillBatch() of a fresh awaitData cursor is batch-of-one, so: doc 1 alone,
                // then docs 2 and 3.
                assert.commandWorked(testColl.update({_id: 1}, {$set: {a: "old"}}));
                assert.commandWorked(testColl.update({_id: 2}, {$set: {a: "old"}}));
                assert.commandWorked(testColl.update({_id: 3}, {$set: {a: "old"}}));

                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(db, () => {
                    // Skip the first two lookups.
                    const fp = configureFailPoint(
                        db.getMongo(),
                        "hangBeforeChangeStreamUpdateLookup",
                        {},
                        {skip: 2},
                    );

                    // A cursor is pinned to its creating session, so the parallel shell's
                    // getMore must pass this lsid explicitly.
                    const lsid = db.getSession().getSessionId();
                    const ns = {db: db.getName(), coll: testColl.getName()};
                    const join = startParallelShell(
                        `
                           import {assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";

                         // Drain until all three updates are collected rather than asserting on a
                         // single response, in case a getMore returns an empty batch.
                           const updates = [];
                           assert.soon(() => {
                               const res = assert.commandWorked(
                                   db.getSiblingDB(${tojson(db.getName())}).runCommand({
                                       getMore: ${tojson(cursor._cursorid)},
                                       collection: ${tojson(testColl.getName())},
                                       batchSize: 10,
                                       lsid: ${tojson(lsid)},
                                   }),
                               );
                               updates.push(...res.cursor.nextBatch);
                               return updates.length >= 3;
                           }, "getMore did not return the three updates");
                           assert.gte(updates.length, 3, {updates});
                           const ns = ${tojson(ns)};

                           // docs 1 and 2: looked up before the DDL.
                           assertChangeStreamEventEq(updates[0], {
                               operationType: "update", ns, documentKey: {_id: 1}, fullDocument: {_id: 1, a: "old"},
                           });
                           assertChangeStreamEventEq(updates[1], {
                               operationType: "update", ns, documentKey: {_id: 2}, fullDocument: {_id: 2, a: "old"},
                           });

                        // doc 3: looked up after the DDL via the stale cached acquisition, both
                        // UUID modes silently return the pre-rename data.
                           assertChangeStreamEventEq(updates[2], {
                               operationType: "update", ns, documentKey: {_id: 3}, fullDocument: {_id: 3, a: "old"},
                           });
                           `,
                        db.getMongo().port,
                    );

                    // Wait on the failpoint while running the batched updateLookup.
                    fp.wait();

                    try {
                        // Rename away, recreate with a fresh UUID, and insert a different doc
                        // under doc 3's _id: a fresh acquisition would find it, the stale one
                        // returns "old".
                        assert.commandWorked(testColl.renameCollection(collNameB));
                        assertCreateCollection(db, collNameA);
                        assert.commandWorked(
                            db.getCollection(collNameA).insert({_id: 3, a: "post-ddl"}),
                        );
                    } finally {
                        // Release the hung getMore before joining: an assert above must not
                        // leave the failpoint armed and the parallel shell stuck until the
                        // task timeout.
                        fp.off();
                        join();
                    }
                });

                assert.eq(delta.changeStreams.updateLookup.enrichBatchesStarted, 2, {delta});

                // All three lookups were handled directly by the selected engine, no fallback.
                const engine = expectedUpdateLookupEngine();
                const metricsByEngine = readUpdateLookupDelta(delta);
                assert.eq(metricsByEngine[engine].found, 3, {engine, metricsByEngine});
                assert.eq(metricsByEngine[engine].notFound, 0, {engine, metricsByEngine});
                assert.eq(metricsByEngine[engine].notHandled, 0, {engine, metricsByEngine});

                for (const [other, otherLookup] of Object.entries(metricsByEngine)) {
                    if (other === engine) {
                        continue;
                    }
                    assert.eq(
                        otherLookup.found + otherLookup.notFound + otherLookup.notHandled,
                        0,
                        {
                            other,
                            otherLookup,
                            delta,
                        },
                    );
                }

                cursor.close();
            });
        }
    });

    // killOp between two enrich() calls of one batch, while cached executor/acquisition state
    // is held: must unwind cleanly and fail the getMore with Interrupted.
    describe("interrupt", function () {
        const testDB = () => rst.getPrimary().getDB(jsTestName() + "_interrupt");

        const collNameA = "collA";

        beforeEach(function () {
            assertDropAndRecreateCollection(testDB(), collNameA);
        });

        afterEach(function () {
            assertDropCollection(testDB(), collNameA);
        });

        it("fails cleanly when the getMore is killed while holding cached lookup state", function () {
            const db = testDB();
            const testColl = db.getCollection(collNameA);
            assert.writeOK(
                testColl.insert([
                    {_id: 1, a: 0},
                    {_id: 2, a: 0},
                    {_id: 3, a: 0},
                ]),
            );

            const cursor = testColl.watch([], {
                fullDocument: "updateLookup",
                cursor: {batchSize: 0},
            });

            // The first update lands in its own batch (awaitData first-batch-of-one), docs 2
            // and 3 share the second. Skip the first two lookups so doc 2's caches batch 2's
            // executor and acquisition; hang before doc 3's lookup, mid-batch.
            assert.commandWorked(testColl.update({_id: 1}, {$set: {a: "old"}}));
            assert.commandWorked(testColl.update({_id: 2}, {$set: {a: "old"}}));
            assert.commandWorked(testColl.update({_id: 3}, {$set: {a: "old"}}));

            // 'shouldCheckForInterrupt' makes the hung getMore observe the killOp and fail with
            // Interrupted; without it the hang only ever ends via fp.off() below, and the op
            // then can finish its remaining work without a later interrupt check, so the
            // aggregate can succeed and this test flakes (observed in burn-in).
            const fp = configureFailPoint(
                db.getMongo(),
                "hangBeforeChangeStreamUpdateLookup",
                {shouldCheckForInterrupt: true},
                {skip: 2},
            );

            const lsid = db.getSession().getSessionId();
            const comment = "change_stream_update_lookup_interrupt_test";

            const join = startParallelShell(
                `
                const res = db.getSiblingDB(${tojson(db.getName())}).runCommand({
                    getMore: ${tojson(cursor._cursorid)},
                    collection: ${tojson(testColl.getName())},
                    batchSize: 10,
                    lsid: ${tojson(lsid)},
                    comment: ${tojson(comment)},
                });
                assert.commandFailedWithCode(res, ErrorCodes.Interrupted, {res});
                `,
                db.getMongo().port,
            );

            // Wait until the getMore is blocked at the failpoint, then kill it.
            fp.wait();

            try {
                const adminDB = db.getMongo().getDB("admin");
                const ops = adminDB
                    .aggregate([{$currentOp: {}}, {$match: {"command.comment": comment}}])
                    .toArray();
                assert.eq(ops.length, 1, {ops});
                assert.commandWorked(db.killOp(ops[0].opid));
            } finally {
                // Release the failpoint and wait for the parallel shell's assertion even if
                // the $currentOp asserts fail above.
                fp.off();
                join();
            }

            cursor.close();
        });
    });
});
