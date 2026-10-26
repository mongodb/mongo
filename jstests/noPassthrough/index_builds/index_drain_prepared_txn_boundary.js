/**
 * Builds multiple indexes while preparing transactions at different points during the build.
 * Verifies that, after an earlier transaction commits or aborts, the build reaches readiness
 * without waiting for a newer prepared transaction to resolve, then completes with consistent indexes.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("prepared side writes beyond the initial drain tail", function () {
    let rst, primary, db;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        db = primary.getDB(jsTestName());
    });

    after(function () {
        rst.stopSet();
    });

    for (const initialState of ["commit", "abort", "none"]) {
        for (const batchSize of [1, 1000]) {
            it(`initial resolution ${initialState}, batch size ${batchSize}`, function () {
                assert.commandWorked(
                    primary.adminCommand({
                        setParameter: 1,
                        maxIndexBuildDrainBatchSize: batchSize,
                    }),
                );
                const coll = db.getCollection(`coll_${initialState}_${batchSize}`);
                assert.commandWorked(coll.insert({_id: 0, a: -1, b: -1}));

                const bulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
                const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), [
                    {a: 1},
                    {b: 1},
                ]);
                bulk.wait();

                // A committed side write provides a drain pause before any prepared record is read.
                assert.commandWorked(coll.insert({_id: 1, a: 1, b: 1}));

                const initialSession = primary.startSession();
                const initialColl = initialSession
                    .getDatabase(db.getName())
                    .getCollection(coll.getName());
                let initialTimestamp;
                if (initialState !== "none") {
                    initialSession.startTransaction();
                    assert.commandWorked(initialColl.insert({_id: 2, a: 640, b: 640}));
                    assert.commandWorked(initialColl.update({_id: 2}, {$set: {a: 2, b: 2}}));
                    initialTimestamp = PrepareHelpers.prepareTransaction(initialSession);
                }

                // Keep the last initial record committed so the reverse prefix read can reach the pause.
                assert.commandWorked(coll.insert({_id: 5, a: 5, b: 5}));

                const duringDrain = configureFailPoint(
                    primary,
                    "hangIndexBuildDuringDrainWritesPhase",
                    {
                        indexNames: ["a_1"],
                        iteration: 0,
                    },
                );
                const readiness = configureFailPoint(
                    primary,
                    "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
                );

                // Include the initial prepared writes and committed tail in the drain's majority snapshot.
                rst.awaitLastOpCommitted();
                bulk.off();
                duringDrain.wait();

                // These writes are beyond BOTH captured tails, including the index drained second.
                assert.commandWorked(coll.insert({_id: 3, a: 3, b: 3}));
                const laterSession = primary.startSession();
                const laterColl = laterSession
                    .getDatabase(db.getName())
                    .getCollection(coll.getName());
                laterSession.startTransaction();
                assert.commandWorked(laterColl.insert({_id: 4, a: 740, b: 740}));
                assert.commandWorked(laterColl.update({_id: 4}, {$set: {a: 4, b: 4}}));
                const laterTimestamp = PrepareHelpers.prepareTransaction(laterSession);

                duringDrain.off();

                if (initialState !== "none") {
                    const opId = IndexBuildTest.waitForIndexBuildToStart(
                        db,
                        coll.getName(),
                        "a_1",
                        {
                            desc: {$regex: /IndexBuildsCoordinatorMongod/},
                        },
                    );
                    assert.soon(() => {
                        const [op] = db.currentOp({opid: opId, $all: true}).inprog;
                        return (
                            op &&
                            op.prepareReadConflicts > 0 &&
                            Object.keys(op.locks || {}).length === 0
                        );
                    });

                    if (initialState === "commit") {
                        assert.commandWorked(
                            PrepareHelpers.commitTransaction(initialSession, initialTimestamp),
                        );
                    } else {
                        assert.commandWorked(initialSession.abortTransaction_forTesting());
                    }
                }

                // This must be reachable while the newer transaction is STILL prepared. In particular,
                // neither retries nor starting the second index may move the initial boundary forward.
                readiness.wait();
                if (initialState === "abort") {
                    assert.commandWorked(laterSession.abortTransaction_forTesting());
                } else {
                    assert.commandWorked(
                        PrepareHelpers.commitTransaction(laterSession, laterTimestamp),
                    );
                }

                readiness.off();
                awaitBuild();

                initialSession.endSession();
                laterSession.endSession();
            });
        }
    }
});
