/**
 * Tests the convertToCapped coordinator behavior when there is an hypothetical failure during the
 * local convertToCapped operation.
 * @tags: [
 *  # TODO SERVER-126244: Remove once 9.0 becomes last LTS.
 *  # In the newest version convertToCapped will also retry errors in untracked collections.
 *  multiversion_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
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

assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}));

{
    jsTestLog("Test an hypothetical failure right after the collection is locally capped.");
    assert.commandWorked(db.runCommand({create: coll.getName()}));
    assert(!coll.isCapped());

    const failpoint = configureFailPoint(primaryShard, "convertToCappedFailAfterCappingTheCollection");

    const waitForConvertToCapped = startParallelShell(
        funWithArgs(
            function (dbName, collName) {
                const testDB = db.getSiblingDB(dbName);
                // An error on the coordinator right after the collection is locally capped should trigger the retry logic, so the operation eventually succeeds.
                assert.commandWorked(testDB.runCommand({convertToCapped: collName, size: 1000}));
            },
            db.getName(),
            coll.getName(),
        ),
        st.s.port,
    );

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

    const failpoint = configureFailPoint(primaryShard, "convertToCappedFailBeforeCappingTheCollection");

    // If the collection hasn't been capped when the error rises, there is no need to for the
    // coordinator to retry the operation, so it finishes returning the error received from the data
    // shard.
    assert.commandFailedWithCode(
        db.runCommand({convertToCapped: coll.getName(), size: 1000}),
        ErrorCodes.InternalError,
    );

    assert(!coll.isCapped());

    failpoint.off();
    coll.drop();
}

st.stop();
