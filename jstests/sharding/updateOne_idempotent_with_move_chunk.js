/**
 * Tests that concurrent updateOne operation with _id without shard key and chunk migration for the
 * chunk being updated doesn't cause zero updates or double updates when
 * featureFlagUpdateOneWithIdWithoutShardKey is enabled.
 *
 * @tags: [featureFlagUpdateOneWithIdWithoutShardKey, requires_fcv_73]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

function runTest(testDoubleUpdates) {
    const st = new ShardingTest({shards: 2, mongos: 1, useBridge: true});
    const mongos = st.s0;
    let db = mongos.getDB(jsTestName());

    const coll = db.coll;
    const fullCollName = coll.getFullName();
    coll.drop();

    // Shard the test collection on x.
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: fullCollName, key: {x: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(mongos.adminCommand({split: fullCollName, middle: {x: 0}}));

    // Move the [0, MaxKey] chunk to st.shard1.shardName.
    assert.commandWorked(
        mongos.adminCommand({moveChunk: fullCollName, find: {x: 1}, to: st.shard1.shardName}));

    // Write a document.
    assert.commandWorked(coll.insert({x: -1, _id: 0}));

    // Delay messages from mongos to shard 0 or shard 1 such that the updateOne to that shard
    // reaches post chunk migration from shard 0 to shard 1 below.
    const delayMillis = 500;
    if (testDoubleUpdates) {
        // In this scenario, we delay updateOne broadcast from mongos to shard 1 until after the
        // chunk is migrated to shard 1. This causes double updates to the document in the
        // absence of featureFlagUpdateOneWithIdWithoutShardKey.
        st.rs1.getPrimary().delayMessagesFrom(st.s, delayMillis);
    } else {
        // In this scenario, we delay updateOne broadcast from mongos to shard 0 until after the
        // chunk is migrated to shard 1. This causes zero updates to the document in the
        // absence of featureFlagUpdateOneWithIdWithoutShardKey.
        st.rs0.getPrimary().delayMessagesFrom(st.s, delayMillis);
    }

    const cmdObj = {
        update: coll.getName(),
        updates: [
            {q: {_id: 0}, u: {$inc: {counter: 1}}, multi: false},
        ]
    };

    const joinUpdate = startParallelShell(funWithArgs(function(cmdObj, testName) {
                                              const res =
                                                  db.getSiblingDB(testName).runCommand(cmdObj);
                                              assert.commandWorked(res);
                                              assert.eq(1, res.nModified, tojson(res));
                                          }, cmdObj, jsTestName()), mongos.port);

    const joinMoveChunk = startParallelShell(
        funWithArgs(function(fullCollName, shardName) {
            // Sleep for small duration to ascertain that we don't start
            // moveChunk before an updateOne is received by shard 0 or shard 1
            // depending on the scenario tested.
            sleep(100);
            assert.commandWorked(
                db.adminCommand({moveChunk: fullCollName, find: {x: -1}, to: shardName}));
        }, coll.getFullName(), st.shard1.shardName), mongos.port);

    joinMoveChunk();
    joinUpdate();

    // There should only be a single update of counter value in both scenarios with
    // featureFlagUpdateOneWithIdWithoutShardKey enabled.
    assert.neq(null, coll.findOne({x: -1, counter: 1}));
    st.stop();
}

jsTest.log("Running test for double updates");
runTest(true);

jsTest.log("Running test for zero updates");
runTest(false);
