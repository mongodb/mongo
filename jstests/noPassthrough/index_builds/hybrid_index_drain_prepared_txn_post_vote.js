/**
 * Prepares a transaction after the primary signals index-build readiness while withholding a
 * secondary's readiness vote. Verifies that the build continues making progress whether the
 * transaction commits before or after the secondary is allowed to satisfy the commit quorum.
 *
 * @tags: [
 *   primary_driven_index_builds_incompatible,
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("prepared side writes after commit readiness", function () {
    let rst, primary, secondary, db;

    before(function () {
        rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0}}],
            nodeOptions: {setParameter: {featureFlagPrimaryDrivenIndexBuilds: false}},
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();
        db = primary.getDB(jsTestName());
    });

    after(function () {
        rst.stopSet();
    });

    for (const resolveBeforeCommitQuorumSatisfied of [false, true]) {
        it(`resolve before commit quorum satisfied: ${resolveBeforeCommitQuorumSatisfied}`, function () {
            const coll = db.getCollection(`coll_${resolveBeforeCommitQuorumSatisfied}`);
            assert.commandWorked(coll.insert({_id: 0, a: -1}));

            const bulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
            const beforeSecondaryVote = configureFailPoint(
                secondary,
                "hangIndexBuildBeforeSignalPrimaryForCommitReadiness",
            );
            const afterPrimaryVote = configureFailPoint(
                primary,
                "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
            );
            const beforeCommit = configureFailPoint(primary, "hangIndexBuildBeforeCommit");
            const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
            bulk.wait();

            // These records are physically in the initial prefix, but invisible while T is unprepared.
            const session = primary.startSession();
            const sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());
            session.startTransaction();
            assert.commandWorked(sessionColl.insert({_id: 1, a: 640}));
            assert.commandWorked(sessionColl.update({_id: 1}, {$set: {a: 1}}));

            bulk.off();
            afterPrimaryVote.wait();
            beforeSecondaryVote.wait();

            const buildUUID = IndexBuildTest.assertIndexesIdHelper(coll, 1, ["_id_"], ["a_1"], {
                includeBuildUUIDs: true,
            }).a_1.buildUUID;
            const buildInfo = primary
                .getCollection("config.system.indexBuilds")
                .findOne({_id: buildUUID});
            assert(buildInfo, tojson(buildInfo));
            assert(buildInfo.commitReadyMembers.includes(primary.host), tojson(buildInfo));
            assert(!buildInfo.commitReadyMembers.includes(secondary.host), tojson(buildInfo));

            // Prepare only AFTER the primary has voted, so the next IX drain sees an initial-prefix
            // conflict that the pre-readiness policy would wait for.
            const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: "majority"});

            // The first post-vote drain must see prepare in its majority snapshot.
            rst.awaitLastOpCommitted(undefined, [primary]);

            // This existing pause also runs after subsequent IX drains. Enable it after the initial drain.
            const postVoteDrain = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
            afterPrimaryVote.off();
            postVoteDrain.wait();

            const opId = IndexBuildTest.waitForIndexBuildToStart(db, coll.getName(), "a_1", {
                desc: {$regex: /IndexBuildsCoordinatorMongod/},
            });
            IndexBuildTest.assertIndexBuildCurrentOpContents(db, opId, (op) => {
                assert.eq({}, op.locks || {}, tojson(op));
                assert.gt(op.prepareReadConflicts, 0, tojson(op));
            });

            if (resolveBeforeCommitQuorumSatisfied) {
                assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

                // After releasing the first post-vote attempt, the action wait's existing timeout
                // schedules another drain.
                const drainYield = configureFailPoint(primary, "hangDuringIndexBuildDrainYield", {
                    namespace: coll.getFullName(),
                });
                postVoteDrain.off();
                drainYield.wait();
                drainYield.off();
                beforeSecondaryVote.off();
                beforeCommit.wait();
            } else {
                // Satisfy commit quorum without resolving T.
                beforeSecondaryVote.off();
                postVoteDrain.off();
                beforeCommit.wait();
                assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
            }

            beforeCommit.off();
            awaitBuild();
            rst.awaitReplication();

            session.endSession();
        });
    }
});
