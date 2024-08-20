/**
 * Tests the collection of query stats for a change stream query.
 * @tags: [
 *   uses_change_streams,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    checkChangeStreamEntry,
    getLatestQueryStatsEntry,
    getNumberOfGetMoresUntilNextDocForChangeStream
} from "jstests/libs/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function testCollectionChangeStream(conn) {
    const db = conn.getDB("test");
    assertDropAndRecreateCollection(db, "coll");

    // Check change stream explain is recorded.
    assert.commandWorked(
        db.coll.explain({"verbosity": "queryPlanner"}).aggregate([{"$changeStream": {}}]));
    let queryStatsEntry = getLatestQueryStatsEntry(db);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: db,
        collectionName: "coll",
        numExecs: 1,
        numDocsReturned: 0
    });
    assert(queryStatsEntry.key.hasOwnProperty("explain"));

    // Check creation of change stream cursor is recorded.
    let cursor = db.coll.watch([], {batchSize: 1});

    let numExecs = 1;
    let numDocsReturned = 0;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(db),
        db: db,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Insert document into a collection and make sure retrieving the information updates the query
    // stats entry.
    assert.commandWorked(db.coll.insert({_id: 0, a: 1}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(cursor);
    numDocsReturned++;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(db),
        db: db,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Close cursor and check that it updates query stats entry.
    cursor.close();
    numExecs++;
    queryStatsEntry = getLatestQueryStatsEntry(db);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: db,
        collectionName: "coll",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    // Closing the cursor should result in 0 ms for the operation.
    assert.eq(queryStatsEntry.metrics.lastExecutionMicros, 0);
}

function testDatabaseChangeStream(conn) {
    const db = conn.getDB("test");
    assertDropAndRecreateCollection(db, "coll1");
    assertDropAndRecreateCollection(db, "coll2");

    // Check creation of change stream cursor is recorded.
    let wholeDBCursor = db.watch([]);
    let numExecs = 1;
    let numDocsReturned = 0;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(db),
        db: db,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Insert document into a collection and make sure retrieving the information updates the query
    // stats entry.
    assert.commandWorked(db.coll1.insert({_id: 1, a: 2}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(wholeDBCursor);
    numDocsReturned++;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(db),
        db: db,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Insert document into a different collection and make sure retrieving the information updates
    // the query stats entry.
    assert.commandWorked(db.coll2.insert({_id: 1, a: 2}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(wholeDBCursor);
    numDocsReturned++;
    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(db),
        db: db,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Close cursor and check that it increases numExecs.
    wholeDBCursor.close();
    numExecs++;
    let queryStatsEntry = getLatestQueryStatsEntry(db);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: db,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Closing the cursor should result in 0 ms for the operation.
    assert.eq(queryStatsEntry.metrics.lastExecutionMicros, 0);
}

function testWholeClusterChangeStream(conn) {
    const dbA = conn.getDB("testA");
    const dbB = conn.getDB("testB");
    assertDropAndRecreateCollection(dbA, "collA");
    assertDropAndRecreateCollection(dbB, "collB");

    // Creating the change stream cursor should create a query stats entry.
    let wholeClusterCursor = dbA.getMongo().watch([]);
    let numExecs = 1;
    let numDocsReturned = 0;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(dbA),
        db: dbA,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Insert document into a database and make sure retrieving the information updates the query
    // stats entry.
    assert.commandWorked(dbA.collA.insert({_id: 1, a: 2}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(wholeClusterCursor);
    numDocsReturned++;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(dbA),
        db: dbA,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Insert document into different database and make sure retrieving the information updates the
    // query stats entry.
    assert.commandWorked(dbB.collB.insert({_id: 1, a: 2}));
    numExecs += getNumberOfGetMoresUntilNextDocForChangeStream(wholeClusterCursor);
    numDocsReturned++;

    checkChangeStreamEntry({
        queryStatsEntry: getLatestQueryStatsEntry(dbA),
        db: dbA,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });

    // Close cursor and check that it increases numExecs.
    wholeClusterCursor.close();
    numExecs++;
    let queryStatsEntry = getLatestQueryStatsEntry(dbA);
    checkChangeStreamEntry({
        queryStatsEntry: queryStatsEntry,
        db: dbA,
        collectionName: "$cmd.aggregate",
        numExecs: numExecs,
        numDocsReturned: numDocsReturned
    });
    // Closing the cursor should result in 0 ms for the operation.
    assert.eq(queryStatsEntry.metrics.lastExecutionMicros, 0);
}

function runTest(conn) {
    testCollectionChangeStream(conn);
    testDatabaseChangeStream(conn);
    testWholeClusterChangeStream(conn);
}

{
    // Test the non-sharded case.
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
    rst.initiate();
    rst.getPrimary().getDB("admin").setLogLevel(3, "queryStats");
    runTest(rst.getPrimary());
    rst.stopSet();
}

{
    // Test on a sharded cluster.
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        config: 1,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
            }
        },
    });
    runTest(st.s);
    st.stop();
}
