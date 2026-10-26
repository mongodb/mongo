/**
 * Commits or aborts a prepared transaction while an index build runs on a replica set. Verifies
 * that transaction resolution replicates and the index build completes, including when its commit
 * quorum allows the primary to finish before the secondary signals readiness.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("prepared side writes are drained in order", function () {
    let rst, primary, primaryDriven, testDB, secondary;

    before(function () {
        rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0}}],
        });
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        primaryDriven = FeatureFlagUtil.isPresentAndEnabled(
            primary.getDB("admin"),
            "PrimaryDrivenIndexBuilds",
        );
        assert.commandWorked(
            primary.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"},
            }),
        );
        testDB = primary.getDB("test");
        secondary = rst.getSecondary();
    });

    after(function () {
        rst.stopSet();
    });

    for (const commit of [true, false]) {
        for (const batchSize of [1, 1000]) {
            it(`${commit ? "commit" : "abort"}, batch size ${batchSize}`, function () {
                for (const commitQuorum of primaryDriven
                    ? ["votingMembers"]
                    : ["votingMembers", 1]) {
                    const coll = testDB.getCollection(
                        `coll_${commit}_${batchSize}_${commitQuorum}`,
                    );
                    for (const node of rst.nodes) {
                        assert.commandWorked(
                            node.adminCommand({
                                setParameter: 1,
                                maxIndexBuildDrainBatchSize: batchSize,
                            }),
                        );
                    }

                    assert.commandWorked(coll.insert({_id: 0, a: -1}));

                    const hangAfterBulkLoad = configureFailPoint(
                        primary,
                        "hangAfterIndexBuildDumpsInsertsFromBulk",
                    );
                    const hangSecondaryBulkLoad = primaryDriven
                        ? undefined
                        : configureFailPoint(secondary, "hangAfterIndexBuildDumpsInsertsFromBulk");
                    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
                        primary,
                        coll.getFullName(),
                        {a: 1},
                        {},
                        [],
                        commitQuorum,
                    );
                    hangAfterBulkLoad.wait();
                    hangSecondaryBulkLoad?.wait();

                    // Test partial-batch rollback, or a committed prefix when the batch size is one.
                    assert.commandWorked(coll.insert({_id: 3, a: 3}));

                    // Write to `a` twice within the same document in a single transaction, with
                    // an unrelated write in between. All of these generate side writes for the
                    // index build.
                    const session = primary.startSession();
                    const sessionColl = session
                        .getDatabase(testDB.getName())
                        .getCollection(coll.getName());
                    session.startTransaction();
                    assert.commandWorked(sessionColl.insert({_id: 1, a: 640}));
                    assert.commandWorked(coll.insert({_id: 2, a: 7}));
                    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {a: 0}}));
                    const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {
                        w: "majority",
                    });

                    // Majority acknowledgment can precede secondary application. Both builders must see
                    // the prepared writes, including when a resumable drain uses a majority snapshot.
                    rst.awaitReplication();
                    rst.awaitLastOpCommitted();

                    const beforeSecondaryVote =
                        primaryDriven || commitQuorum !== 1
                            ? undefined
                            : configureFailPoint(
                                  secondary,
                                  "hangIndexBuildBeforeSignalPrimaryForCommitReadiness",
                              );

                    hangAfterBulkLoad.off();
                    hangSecondaryBulkLoad?.off();

                    for (const node of primaryDriven ? [primary] : rst.nodes) {
                        const nodeDB = node.getDB(testDB.getName());
                        const opId = IndexBuildTest.waitForIndexBuildToStart(
                            nodeDB,
                            coll.getName(),
                            "a_1",
                            {
                                desc: {$regex: /IndexBuildsCoordinatorMongod/},
                            },
                        );
                        assert.soon(
                            () => {
                                const [op] = nodeDB.currentOp({opid: opId, $all: true}).inprog;
                                return (
                                    op &&
                                    op.prepareReadConflicts > 0 &&
                                    Object.keys(op.locks || {}).length === 0
                                );
                            },
                            () =>
                                tojson({
                                    host: node.host,
                                    currentOp: nodeDB.currentOp({opid: opId, $all: true}),
                                }),
                        );
                    }

                    if (!primaryDriven) {
                        const buildUUID = IndexBuildTest.assertIndexesIdHelper(
                            coll,
                            1,
                            ["_id_"],
                            ["a_1"],
                            {
                                includeBuildUUIDs: true,
                            },
                        ).a_1.buildUUID;
                        const buildInfo = primary
                            .getCollection("config.system.indexBuilds")
                            .findOne({_id: buildUUID});
                        assert(buildInfo, tojson(buildInfo));
                        assert.eq(
                            0,
                            (buildInfo.commitReadyMembers || []).length,
                            tojson(buildInfo),
                        );
                    }

                    // Both transaction resolution and index completion must make progress.
                    if (commit) {
                        assert.commandWorked(
                            PrepareHelpers.commitTransaction(session, prepareTimestamp),
                        );
                    } else {
                        assert.commandWorked(session.abortTransaction_forTesting());
                    }

                    if (commitQuorum === 1) {
                        // The primary can finish without the secondary voting. Transaction resolution
                        // precedes index-commit application on the secondary.
                        beforeSecondaryVote.wait();
                        awaitIndexBuild();
                        beforeSecondaryVote.off();
                    } else {
                        awaitIndexBuild();
                    }

                    rst.awaitReplication();
                    session.endSession();
                }
            });
        }
    }
});
