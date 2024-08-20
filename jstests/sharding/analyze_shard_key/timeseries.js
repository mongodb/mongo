/**
 * Tests that timeseries collections do not support the analyzeShardKey and configureQueryAnalyzer
 * commands since a timeseries collection is a view (of a bucket collection) and the analyzeShardKey
 * and configureQueryAnalyzer commands cannot be run against a view.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numNodesPerRS = 2;

function runTest(conn, {isShardedColl, st}) {
    const dbName = "testDb";
    const collName = "testColl";
    const numDocs = 10;
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    if (st) {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
    }

    const coll = db.getCollection(collName);
    assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "ts"}}));

    if (isShardedColl) {
        assert(st);
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {ts: 1}}));
    }

    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({ts: new Date()});
    }
    assert.commandWorked(coll.insert(docs));

    assert.commandFailedWithCode(conn.adminCommand({analyzeShardKey: ns, key: {ts: 1}}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1}),
        ErrorCodes.IllegalOperation);

    assert(coll.drop());
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS}});
    runTest(st.s, {isShardedColl: false, st});
    runTest(st.s, {isShardedColl: true, st});
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: numNodesPerRS});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary(), {isShardedColl: false});
    rst.stopSet();
}