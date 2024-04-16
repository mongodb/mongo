// Tests the collection of query stats for a change stream query.
// @tags: [
//   uses_change_streams,
//   requires_replication,
//   requires_sharding,
//   requires_fcv_72
// ]
import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getLatestQueryStatsEntry,
} from "jstests/libs/query_stats_utils.js";

// Make sure to  only call this function while waiting for one new document or set batchSize of the
// input cursor to 1, so each hasNext() call will correspond to an internal getMore.
function getNumberOfGetMoresUntilNextDoc(cursor) {
    let numGetMores = 0;
    assert.soon(() => {
        numGetMores++;
        return cursor.hasNext();
    });

    // Get the document that is on the cursor to reset the cursor to a state where calling hasNext()
    // corresponds to a getMore.
    cursor.next();
    return numGetMores;
}

function checkChangeStreamEntry({queryStatsEntry, db, collectionName, numExecs, numDocsReturned}) {
    assert.eq(collectionName, queryStatsEntry.key.queryShape.cmdNs.coll);

    // Confirm entry is a change stream request.
    let stringifiedPipeline = JSON.stringify(queryStatsEntry.key.queryShape.pipeline, null, 0);
    assert(stringifiedPipeline.includes("_internalChangeStream"));

    // TODO SERVER-76263 Support reporting 'collectionType' on a sharded cluster.
    if (!FixtureHelpers.isMongos(db)) {
        assert.eq("changeStream", queryStatsEntry.key.collectionType);
    }

    // Checking that metrics match expected metrics.
    assert.eq(queryStatsEntry.metrics.execCount, numExecs);
    assert.eq(queryStatsEntry.metrics.docsReturned.sum, numDocsReturned);

    // FirstResponseExecMicros and TotalExecMicros match since each getMore is recorded as a new
    // first response.
    assert.eq(queryStatsEntry.metrics.totalExecMicros.sum,
              queryStatsEntry.metrics.firstResponseExecMicros.sum);
    assert.eq(queryStatsEntry.metrics.totalExecMicros.max,
              queryStatsEntry.metrics.firstResponseExecMicros.max);
    assert.eq(queryStatsEntry.metrics.totalExecMicros.min,
              queryStatsEntry.metrics.firstResponseExecMicros.min);
}

function testCollectionChangeStream(conn) {
    const db = conn.getDB("test");
    assertDropAndRecreateCollection(db, "coll");

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
    numExecs += getNumberOfGetMoresUntilNextDoc(cursor);
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
    let queryStatsEntry = getLatestQueryStatsEntry(db);
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
    numExecs += getNumberOfGetMoresUntilNextDoc(wholeDBCursor);
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
    numExecs += getNumberOfGetMoresUntilNextDoc(wholeDBCursor);
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
    numExecs += getNumberOfGetMoresUntilNextDoc(wholeClusterCursor);
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
    numExecs += getNumberOfGetMoresUntilNextDoc(wholeClusterCursor);
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
