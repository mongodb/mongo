/**
 * Test that explained aggregation commands behave correctly with the readConcern option.
 */

import {planHasStage} from "jstests/libs/analyze_plan.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const runTest = (db, coll) => {
    // Test that explain is legal with all readConcern levels.
    const readConcernLevels = ["local", "majority", "available", "linearizable", "snapshot"];
    readConcernLevels.forEach(function(readConcernLevel) {
        assert.commandWorked(
            coll.explain().aggregate([], {readConcern: {level: readConcernLevel}}));

        assert.commandWorked(db.runCommand({
            aggregate: coll.getName(),
            pipeline: [],
            explain: true,
            readConcern: {level: readConcernLevel}
        }));

        assert.commandWorked(db.runCommand({
            explain: {aggregate: coll.getName(), pipeline: [], cursor: {}},
            readConcern: {level: readConcernLevel}
        }));

        assert.commandWorked(db.runCommand(
            {explain: {find: coll.getName(), filter: {}}, readConcern: {level: readConcernLevel}}));

        assert.commandWorked(db.runCommand(
            {explain: {count: coll.getName(), query: {}, readConcern: {level: readConcernLevel}}}));
    });
};

// Test with a replica set
{
    const rst = new ReplSetTest({name: "aggExplainReadConcernSet", nodes: 2});
    rst.startSet();
    rst.initiate();

    const session =
        rst.getPrimary().getDB("test").getMongo().startSession({causalConsistency: false});
    const db = session.getDatabase("test");
    const coll = db.agg_explain_read_concern;

    runTest(db, coll);

    session.endSession();
    rst.stopSet();
}

// Test with a sharded cluster
{
    const st = new ShardingTest({shards: 2, config: 1});

    const config = st.s.getDB("config");
    const db = st.s.getDB("test");
    const coll = db.agg_explain_read_concern;

    assert.commandWorked(st.s.adminCommand(
        {enableSharding: "test", primaryShard: config.shards.find().toArray()[0]._id}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: "test.agg_explain_read_concern", key: {_id: 1}}));

    runTest(db, coll);

    // Ensure read concern level is reflected in explain output.
    let explain = db.runCommand(
        {explain: {find: coll.getName(), filter: {}}, readConcern: {level: "available"}});
    assert(!planHasStage(db, explain, "SHARDING_FILTER"));

    explain = db.runCommand(
        {explain: {find: coll.getName(), filter: {}}, readConcern: {level: "snapshot"}});
    assert(planHasStage(db, explain, "SHARDING_FILTER"));

    st.stop();
}
