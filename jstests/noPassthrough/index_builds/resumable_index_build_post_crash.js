/**
 * Tests that index builds on a node that crashes during the drain phase are resumable without a
 * full restart of the index build.
 *
 * These crashes may occur before or after the vote to commit, on either the primary or secondary.
 * In the case where the crash occurs after the vote to commit but before oplog application of the
 * commitIndexBuild command on the secondary, the index build should be resumable on the secondary.
 * In the case where the crash occurs after the oplog application of the command on both, the
 * resume should be effectively a noop (nothing to resume).
 *
 * While side writes are timestamped, the corresponding writes from the drains are not. This
 * affects what does and does not get included in the checkpoint, which is taken at the stable
 * timestamp and used - along with the recent replicated, journaled oplog entries - on restart.
 * Given these facts, different interleavings of writes/checkpointing/crashing must be tested.
 * This includes cases where:
 *  - Side writes AND corresponding drains have been stable-checkpointed before crash.
 *      - Cases by timeline:
 *          - Side writes, Drains, Stable Timestamp, Checkpoint@ST, <CRASH>
 *  - Side writes BUT NOT corresponding drains have been stable-checkpointed before crash.
 *      - Cases by timeline:
 *          - Side writes, Stable Timestamp, Checkpoint@ST, Drains, <CRASH>
 *  - Drains BUT NOT corresponding side writes have been stable-checkpointed before crash.
 *      - This is the case where a checkpoint is taken after the drain, so it includes the
 *          untimestamped writes from the drain, but before the stable timestamp exceeds the
 *          timestamp of the corresponding side writes.
 *      - Cases by timeline:
 *          - Stable Timestamp, Side writes, Drains, Checkpoint@ST, <CRASH>
 *  - Neither side writes NOR corresponding drains have been stable-checkpointed before crash.
 *      - This is the case whether side writes have occurred or not, as long as a checkpoint
 *          hasn't been taken at a stable timestamp that exceeds that of the writes, and given that
 *          these side writes either haven't yet been drained or been checkpointed.
 *      - Cases by timeline:
 *          * crashAndRestart: <CRASH> (before persisting resume state, before any side writes)
 *              - This case restarts, as it cannot resume.
 *          - <CRASH> (before any side writes)
 *          - Side writes, <CRASH> (before any drain, before any checkpoint)
 *          - Stable Timestamp, Side writes, Checkpoint@ST, <CRASH> (before drain)
 *          - Stable Timestamp, Side writes, Checkpoint@ST, Drains, <CRASH>
 *
 * N.B. The cases currently implemented are denoted by (*), along with the subtest name.
 * As part of ongoing work for SERVER-112315, the goal is to run all variations of this test with
 * the documented interleavings.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   requires_commit_quorum,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";

jsTest.log.info("Boot ReplSet of 2 nodes");
let rst = new ReplSetTest({
    nodes: [
        {}, // primary
        {rsConfig: {priority: 0}}, // disallow elections on secondary
    ],
    useHostName: false, // TODO(SERVER-110017): Remove when fixed.
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

/**
 * @param {ReplSetTest} test
 * @param {string} subtestName
 * @param {Mongo} nodeToCrash
 * @param {string} failPointToHang
 * @param {boolean} expectResume
 * @param {string} expectResumePhase
 */
const runTest = function (test, subtestName, nodeToCrash, failPointToHang, expectResume, expectResumePhase) {
    const collName = jsTestName() + "_" + subtestName;

    jsTest.log.info("Prepare data");

    // When i is in [0, 3, 6, 9...], insert {a: i} before starting the index build.
    const insertPreBuild = function (i) {
        return i % 3 == 0;
    };
    // When i is in [1, 4, 7, 10...], insert {a: i} during the index build (i.e. a side write).
    const insertDuringBuild = function (i) {
        return i % 3 == 1;
    };
    // When i is in [2, 5, 8, 11...], insert {a: i} after the index build.
    const insertAfterBuild = function (i) {
        return i % 3 == 2;
    };
    // When i is positive and even, plan a side write update (unless superseded).
    // This should result in side write updates when i in [6, 12, 24].
    const sideUpdate = function (i) {
        return i > 0 && i % 2 == 0;
    };
    // When i is positive and divisible by 5, plan a side write delete (unless superseded).
    // This should result in side write deletes when i in [15].
    const sideDelete = function (i) {
        return i > 0 && i % 5 == 0;
    };
    // When i is positive and divisible by 9, plan a post-index build delete.
    // This should result in post-index build updates when i in [9, 18, 27].
    const postUpdate = function (i) {
        return i > 0 && i % 9 == 0;
    };

    let sideWriteOps = [];
    let postWriteOps = [];
    const sideWriteDelta = 100;
    const postWriteDelta = 1000;
    for (let i = 0; i < 30; i++) {
        let modWrites;
        if (insertPreBuild(i)) {
            assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert({a: i}));

            if (sideUpdate(i)) {
                modWrites = () => sideWriteOps.push((coll) => coll.update({a: i}, {$inc: {a: sideWriteDelta}}));
            }
            if (sideDelete(i)) {
                modWrites = () => sideWriteOps.push((coll) => coll.deleteOne({a: i}));
            }
            if (postUpdate(i)) {
                modWrites = () => postWriteOps.push((coll) => coll.update({a: i}, {$inc: {a: postWriteDelta}}));
            }
        }
        if (insertDuringBuild(i)) {
            sideWriteOps.push((coll) => coll.insert({a: i}));
        }
        if (insertAfterBuild(i)) {
            postWriteOps.push((coll) => coll.insert({a: i}));
        }

        if (modWrites) {
            modWrites();
        }
    }
    test.awaitReplication();

    jsTest.log.info("ResumableIndexBuildTest.runWithCrash");

    ResumableIndexBuildTest.runWithCrash(
        test,
        dbName,
        collName,
        {a: 1},
        secondary,
        failPointToHang,
        expectResume,
        expectResumePhase,
        (coll) => sideWriteOps.map((op) => op(coll)),
        (coll) => postWriteOps.map((op) => op(coll)),
    );

    test.awaitReplication();

    jsTest.log.info("Validating collection data post-index build");

    test.nodes.forEach(function (conn) {
        let coll = conn.getDB(dbName).getCollection(collName);

        let expectedDocs = 0;
        for (let i = 0; i < 30; i++) {
            let expectedVal = i;
            let expectedCount = 1;

            if (insertPreBuild(i) && sideUpdate(i)) {
                expectedVal = i + sideWriteDelta;
            }
            if (insertPreBuild(i) && sideDelete(i)) {
                expectedCount = 0;
            }
            if (insertPreBuild(i) && postUpdate(i)) {
                expectedVal = i + postWriteDelta;
            }

            assert.eq(
                expectedCount,
                coll.find({a: expectedVal}).itcount(),
                expectedCount == 0
                    ? `expected {a: ${expectedVal}} to have been deleted (i = ${i})`
                    : `expected to find {a: ${expectedVal}} (i = ${i})`,
            );
            expectedDocs += expectedCount;
        }

        assert.eq(expectedDocs, coll.find().itcount());
    });
};

runTest(
    rst,
    "crashAndRestart",
    secondary,
    /*failpointToHang*/ "hangAfterIndexBuildDumpsInsertsFromBulk",
    /*expectResume*/ false,
    /*expectedResumePhase*/ "drain writes",
);

rst.stopSet();
