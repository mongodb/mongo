/**
 * Test that queries will be properly routed after executing a write that does not
 * perform any shard version checks.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let runTest = function (writeFunc) {
    let st = new ShardingTest({shards: 2, mongos: 2});

    let testDB = st.s.getDB("test");
    testDB.dropDatabase();

    assert.commandWorked(testDB.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

    assert.commandWorked(testDB.adminCommand({shardCollection: "test.user", key: {x: 1}}));
    assert.commandWorked(testDB.adminCommand({split: "test.user", middle: {x: 0}}));
    assert.commandWorked(
        testDB.adminCommand({moveChunk: "test.user", find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
    );

    let testDB2 = st.s1.getDB("test");
    testDB2.user.insert({x: 123456});

    // Move chunk to bump version on a different mongos.
    assert.commandWorked(
        testDB.adminCommand({moveChunk: "test.user", find: {x: 0}, to: st.shard0.shardName, _waitForDelete: true}),
    );

    // Issue a query and make sure it gets routed to the right shard.
    assert.neq(null, testDB2.user.findOne({x: 123456}));

    // At this point, s1 thinks the version of 'test.user' is 2, bounce it again so it gets
    // incremented to 3
    assert.commandWorked(
        testDB.adminCommand({moveChunk: "test.user", find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
    );

    // Issue a query and make sure it gets routed to the right shard again.
    assert.neq(null, testDB2.user.findOne({x: 123456}));

    // At this point, s0 thinks the version of 'test.user' is 3, bounce it again so it gets
    // incremented to 4
    assert.commandWorked(
        testDB.adminCommand({moveChunk: "test.user", find: {x: 0}, to: st.shard0.shardName, _waitForDelete: true}),
    );

    // Ensure that write commands with multi version do not reset the connection shard version
    // to
    // ignored.
    writeFunc(testDB2);

    assert.neq(null, testDB2.user.findOne({x: 123456}));

    st.stop();
};

runTest(function (db) {
    db.user.update({}, {$inc: {y: 987654}}, false, true);
});

runTest(function (db) {
    db.user.remove({y: "noMatch"}, false);
});
