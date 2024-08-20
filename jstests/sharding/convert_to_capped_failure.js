/**
 * Tests the convertToCapped coordinator behavior when there is an hypothetical failure during the
 * local convertToCapped operation.
 *
 * @tags: [featureFlagConvertToCappedCoordinator]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
});

const db = st.s.getDB(jsTestName());
const coll = db.getCollection("coll");
const primaryShard = st.shard0;

assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}));

{
    jsTestLog("Test an hypothetical failure right after the collection is locally capped.");
    assert.commandWorked(db.runCommand({create: coll.getName()}));
    assert(!coll.isCapped());

    const failpoint =
        configureFailPoint(primaryShard, "convertToCappedFailAfterCappingTheCollection");

    const waitForConvertToCapped = startParallelShell(
        funWithArgs(function(dbName, collName, isTracked) {
            const testDB = db.getSiblingDB(dbName);
            if (isTracked) {
                // The coordinator will eventually succeed since once the collection has been
                // locally capped, the sharding catalog has to be updated.
                assert.commandWorked(testDB.runCommand({convertToCapped: collName, size: 1000}));
            } else {
                // The coordinator will fail the first time because it doesn't retry on error if the
                // collection is not tracked (since the sharding catalog doesn't need to be
                // updated). So let's retry the operation once the failpoint has been disabled to
                // make sure it eventually succeeds.
                assert.commandFailedWithCode(
                    testDB.runCommand({convertToCapped: collName, size: 1000}),
                    ErrorCodes.InternalError);
                assert.commandWorked(testDB.runCommand({convertToCapped: collName, size: 1000}));
            }
        }, db.getName(), coll.getName(), FixtureHelpers.isTracked(coll)), st.s.port);

    // Disable the failpoint once its being hit to unblock the coordinator.
    failpoint.wait();
    failpoint.off();

    waitForConvertToCapped();

    assert(coll.isCapped());

    coll.drop();
}

{
    jsTestLog("Test an hypothetical failure right before the collection is locally capped.");
    assert.commandWorked(db.runCommand({create: coll.getName()}));
    assert(!coll.isCapped());

    const failpoint =
        configureFailPoint(primaryShard, "convertToCappedFailBeforeCappingTheCollection");

    // If the collection hasn't been capped when the error rises, there is no need to for the
    // coordinator to retry the operation, so it finishes returning the error received from the data
    // shard.
    assert.commandFailedWithCode(db.runCommand({convertToCapped: coll.getName(), size: 1000}),
                                 ErrorCodes.InternalError);

    assert(!coll.isCapped());

    failpoint.off();
    coll.drop();
}

st.stop();
