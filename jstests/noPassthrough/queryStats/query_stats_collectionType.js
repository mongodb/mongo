/**
 * Test that collectionType is returned properly in $queryStats. Checks for collection types
 * "collection", "view", "timeseries", "nonExistent", and "virtual". Type "changeStream" is covered
 * in query_stats_changeStreams.js.
 * @tags: [requires_fcv_72]
 */
import {getQueryStats} from "jstests/libs/query_stats_utils.js";

function runTest(conn) {
    const testDB = conn.getDB('test');

    // We create one collection for each corresponding type reported by query stats.
    assert.commandWorked(testDB.createCollection(jsTestName() + "_collection"));
    assert.commandWorked(testDB.createView(
        jsTestName() + "_view", jsTestName() + "_collection", [{$match: {v: {$gt: 42}}}]));
    assert.commandWorked(
        testDB.createCollection(jsTestName() + "_timeseries", {timeseries: {timeField: "time"}}));

    // Next we run queries over each of the collection types to generate query stats.

    // Base _collection has a few simple documents.
    var coll = testDB[jsTestName() + "_collection"];
    coll.insert({v: 1});
    coll.insert({v: 2});
    coll.insert({v: 3});
    coll.find({v: 3}).toArray();
    coll.aggregate([]).toArray();

    // View _view is over _collection.
    coll = testDB[jsTestName() + "_view"];
    coll.find({v: 5}).toArray();
    coll.aggregate([{$match: {v: {$lt: 99}}}]).toArray();

    // Timeseries collection _timeseries.
    coll = testDB[jsTestName() + "_timeseries"];
    coll.insert({v: 1, time: ISODate("2021-05-18T00:00:00.000Z")});
    coll.insert({v: 2, time: ISODate("2021-05-18T01:00:00.000Z")});
    coll.insert({v: 3, time: ISODate("2021-05-18T02:00:00.000Z")});
    coll.find({v: 6}).toArray();
    coll.aggregate().toArray();

    // QueryStats should still be collected for queries run on nonexistent collections.
    assert.commandWorked(testDB.runCommand({find: jsTestName() + "_nonExistent", filter: {v: 6}}));
    assert.commandWorked(
        testDB.runCommand({aggregate: jsTestName() + "_nonExistent", pipeline: [], cursor: {}}));

    // Verify that we have two query stats entries for the collection type. This assumes we have
    // executed one find and one agg query for the given collection type.
    function verifyQueryStatsForCollectionType(collectionType) {
        const queryStats = getQueryStats(conn, {
            extraMatch: {
                "key.collectionType": collectionType,
                "key.queryShape.cmdNs.coll": jsTestName() + "_" + collectionType
            }
        });
        // We should see one entry for find() and one for aggregate()
        // for each collection type. The queries account for the fact
        // that find() queries over views are rewritten to
        // aggregate(). Ie, the query shapes are different because the
        // queries are different.
        assert.eq(2, queryStats.length, "Expected result for collection type " + collectionType);
    }

    verifyQueryStatsForCollectionType("collection");
    verifyQueryStatsForCollectionType("view");
    verifyQueryStatsForCollectionType("timeseries");
    verifyQueryStatsForCollectionType("nonExistent");

    // Run commands that should be tracked as "virtual" collection type.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}));
    assert.commandWorked(testDB.adminCommand(
        {aggregate: 1, pipeline: [{$documents: [{a: 1}, {a: 4}]}], cursor: {}}));
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$listLocalSessions: {}}], cursor: {}}));

    // Verify the queries on "virtual" collection types were tracked appropriately. This includes
    // the 3 queries directly run above, in addition to 1 entry for the $queryStats aggregations
    // run for the test.
    let queryStats = getQueryStats(conn, {extraMatch: {"key.collectionType": "virtual"}});
    assert.eq(4, queryStats.length);

    // Verify that, for views, we capture the original query before it's rewritten. The view would
    // include a $gt predicate on 'v'.
    const findOnViewShape =
        getQueryStats(
            conn, {extraMatch: {"key.collectionType": "view", "key.queryShape.command": "find"}})[0]
            .key.queryShape;
    assert.eq(findOnViewShape.filter, {"v": {"$eq": "?number"}});

    const aggOnViewShape =
        getQueryStats(
            conn,
            {extraMatch: {"key.collectionType": "view", "key.queryShape.command": "aggregate"}})[0]
            .key.queryShape;
    assert.eq(aggOnViewShape.pipeline, [{"$match": {"v": {"$lt": "?number"}}}]);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

// TODO Implement this in SERVER-76263.
if (false) {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        config: 1,
        rs: {nodes: 1},
        mongosOptions: {
            setParameter: {
                internalQueryStatsSamplingRate: -1,
                'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
            }
        },
    });
    runTest(st.s);
    st.stop();
}
