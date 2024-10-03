/**
 * Tests the collection of query stats for a change stream query created using $_passthroughToShard.
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {
    checkChangeStreamEntry,
    getLatestQueryStatsEntry,
    getNumberOfGetMoresUntilNextDocForChangeStream,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();

function testCollectionChangeStream(sdb, shardId) {
    // Check that change stream explain command is recorded.
    let aggregateCmd = {
        aggregate: "coll",
        cursor: {},
        pipeline: [{$changeStream: {}}],
        $_passthroughToShard: {shard: shardId},
    };

    assert.commandWorked(sdb.runCommand({explain: aggregateCmd, verbosity: "executionStats"}));

    let queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "coll",
        numExecs: 1,
        numDocsReturned: 0
    });

    assert(queryStatsEntry.key.hasOwnProperty("explain"));
    jsTestLog("passthroughToShard");
    jsTestLog(queryStatsEntry);
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Check creation of change stream cursor with $_passthroughToShard is recorded.
    let resp = sdb.runCommand(aggregateCmd);
    assert.commandWorked(resp);

    let cursor = new DBCommandCursor(sdb, resp);

    let numExecs = 1;
    let numDocsReturned = 0;

    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Insert document into collection on the shard the change stream was created on (negative id
    // value), and make sure retrieving the information updates the query stats entry.
    assert.commandWorked(sdb.coll.insertOne({location: 1, _id: -3}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(cursor);
    numDocsReturned++;

    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Close cursor and check that it updates query stats entry.
    cursor.close();
    numExecs++;
    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Closing the cursor should result in 0 ms for the operation.
    assert.eq(queryStatsEntry.metrics.lastExecutionMicros, 0);
}

function testDatabaseChangeStream(sdb, shardId) {
    // Check creation of change stream cursor with $_passthroughToShard  for whole database is
    // recorded.
    let aggregateCmd = {
        aggregate: 1,
        cursor: {},
        pipeline: [{$changeStream: {}}],
        $_passthroughToShard: {shard: shardId}
    };

    let resp = sdb.runCommand(aggregateCmd);
    assert.commandWorked(resp);

    let cursor = new DBCommandCursor(sdb, resp);

    // Check creation of change stream cursor is recorded.
    let numExecs = 1;
    let numDocsReturned = 0;

    let queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Insert document into collection on the shard the change stream is watching (negative id
    // value), and make sure retrieving the information updates the query stats entry.
    assert.commandWorked(sdb.coll.insertOne({location: 3, _id: -5}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(cursor);
    numDocsReturned++;

    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Insert document into different collection on the shard the change stream is watching
    // (negative id value), and make sure retrieving the information updates the query stats entry.
    assert.commandWorked(sdb.coll2.insertOne({location: 4, _id: -10}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(cursor);
    numDocsReturned++;

    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Close cursor and check that it increases numExecs.
    cursor.close();
    numExecs++;

    queryStatsEntry = getLatestQueryStatsEntry(sdb);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: sdb,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    assert(queryStatsEntry.key.hasOwnProperty("$_passthroughToShard"));
    assert(queryStatsEntry.key.$_passthroughToShard.hasOwnProperty("shard"));

    // Closing the cursor should result in 0 ms for the operation.
    assert.eq(queryStatsEntry.metrics.lastExecutionMicros, 0);
}

function runTest(sdb, shardId) {
    testCollectionChangeStream(sdb, shardId);
    testDatabaseChangeStream(sdb, shardId);
}

function setupShardedCluster() {
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: false}},
        other: {
            mongosOptions: {
                setParameter: {
                    internalQueryStatsRateLimit: -1,
                }
            }
        }
    });

    const sdb = st.s0.getDB(dbName);
    assert.commandWorked(sdb.dropDatabase());

    sdb.setProfilingLevel(0, -1);
    st.shard0.getDB(dbName).setProfilingLevel(0, -1);

    // Shard the relevant collections.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
    // Shard the collection on {_id: 1}, split at {_id: 0} and move the empty upper chunk to
    // shard1.
    st.shardColl("coll", {_id: 1}, {_id: 0}, {_id: 0}, dbName);
    st.shardColl("coll2", {_id: 1}, {_id: 0}, {_id: 0}, dbName);

    // Returns shardID of shard containing the negative _id values.
    const shardId = st.shard0.shardName;
    return [sdb, st, shardId];
}

{
    let [sdb, st, shardId] = setupShardedCluster();
    st.shard0.getDB("admin").setLogLevel(3, "queryStats");
    runTest(sdb, shardId);
    st.stop();
}
