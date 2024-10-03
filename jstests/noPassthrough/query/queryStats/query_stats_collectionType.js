/**
 * Test that collectionType is returned properly in $queryStats. Checks for collection types
 * "collection", "view", "timeseries", "nonExistent", and "virtual". Type "changeStream" is covered
 * in query_stats_changeStreams.js.
 * @tags: [requires_fcv_72]
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const testDB = conn.getDB('test');

    // We create one collection for each corresponding type reported by query stats.
    assert.commandWorked(testDB.createCollection(jsTestName() + "_collection"));
    assert.commandWorked(testDB.createView(
        jsTestName() + "_view", jsTestName() + "_collection", [{$match: {v: {$gt: 42}}}]));
    assert.commandWorked(
        testDB.createCollection(jsTestName() + "_timeseries", {timeseries: {timeField: "time"}}));
    // We create an additional view over the existing view to test full view resolution. We use
    // the $setWindowFields stage since it desugars into multiple stages, to make sure the query
    // shape produced is restricted to the user-provided query.
    assert.commandWorked(testDB.createView(jsTestName() + "_viewOverView", jsTestName() + "_view", [
        {
            $setWindowFields: {
                partitionBy: "$x",
                sortBy: {v: 1},
                output: {sum: {$sum: "$y", window: {documents: ["unbounded", "current"]}}}
            }
        },
        {$project: {_id: 0, v: 1, x: 1, y: "$output.sum"}}
    ]));

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

    // View _viewOverView is over _view.
    coll = testDB[jsTestName() + "_viewOverView"];
    // Run an empty find to make sure filter in query stats key is recorded as empty.
    coll.find({}).toArray();
    // We test with $densify since it desugars into multiple stages, again to ensure the query
    // shape calculated is restricted to the user-provided query. (In this case, the desugared
    // $densify is acceptable, as long as the view pipeline is ignored).
    coll.aggregate([{$densify: {field: "y", range: {step: 10, bounds: "full"}}}]).toArray();

    // Timeseries collection _timeseries.
    coll = testDB[jsTestName() + "_timeseries"];
    coll.insert({v: 1, time: ISODate("2021-05-18T00:00:00.000Z")});
    coll.insert({v: 2, time: ISODate("2021-05-18T01:00:00.000Z")});
    coll.insert({v: 3, time: ISODate("2021-05-18T02:00:00.000Z")});
    coll.find({v: 6}).toArray();
    // Run an empty aggregate to ensure pipeline in query stats key is recorded as empty.
    coll.aggregate().toArray();

    // QueryStats should still be collected for queries run on nonexistent collections.
    assert.commandWorked(testDB.runCommand({find: jsTestName() + "_nonExistent", filter: {v: 6}}));
    assert.commandWorked(
        testDB.runCommand({aggregate: jsTestName() + "_nonExistent", pipeline: [], cursor: {}}));

    // Verify that we have two query stats entries for the collection type. This assumes we have
    // executed one find and one agg query for the given collection type.
    function verifyQueryStatsForCollectionType(
        collectionType, collectionName = jsTestName() + "_" + collectionType) {
        const queryStats = getQueryStats(conn, {
            extraMatch:
                {"key.collectionType": collectionType, "key.queryShape.cmdNs.coll": collectionName}
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
    verifyQueryStatsForCollectionType("view", jsTestName() + "_viewOverView");
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
    // include a $match stage with $gt predicate on 'v'. The viewOnView would include the same
    // $match stage, the $setWindowFields stage, and the $project stage. The timeseries would
    // include the bucket unpacking stages.
    const findOnViewShape = getQueryStats(conn, {
                                extraMatch: {
                                    "key.collectionType": "view",
                                    "key.queryShape.command": "find",
                                    "key.queryShape.cmdNs.coll": jsTestName() + "_view"
                                }
                            })[0]
                                .key.queryShape;
    assert.eq(findOnViewShape.filter, {"v": {"$eq": "?number"}});

    const aggOnViewShape = getQueryStats(conn, {
                               extraMatch: {
                                   "key.collectionType": "view",
                                   "key.queryShape.command": "aggregate",
                                   "key.queryShape.cmdNs.coll": jsTestName() + "_view"
                               }
                           })[0]
                               .key.queryShape;
    assert.eq(aggOnViewShape.pipeline, [{"$match": {"v": {"$lt": "?number"}}}]);

    const findOnViewOverViewShape =
        getQueryStats(conn, {
            extraMatch: {
                "key.collectionType": "view",
                "key.queryShape.command": "find",
                "key.queryShape.cmdNs.coll": jsTestName() + "_viewOverView"
            }
        })[0]
            .key.queryShape;
    assert.eq(findOnViewOverViewShape.filter, {});

    const aggOnViewOverViewShape =
        getQueryStats(conn, {
            extraMatch: {
                "key.collectionType": "view",
                "key.queryShape.command": "aggregate",
                "key.queryShape.cmdNs.coll": jsTestName() + "_viewOverView"
            }
        })[0]
            .key.queryShape;
    assert.eq(aggOnViewOverViewShape.pipeline, [
        {"$sort": {"y": 1}},
        {
            "$_internalDensify": {
                "field": "y",
                "partitionByFields": [],
                "range": {"step": "?number", "bounds": "full"}
            }
        }
    ]);

    const findOnTimeseriesShape = getQueryStats(conn, {
                                      extraMatch: {
                                          "key.collectionType": "timeseries",
                                          "key.queryShape.command": "find",
                                          "key.queryShape.cmdNs.coll": jsTestName() + "_timeseries"
                                      }
                                  })[0]
                                      .key.queryShape;
    assert.eq(findOnTimeseriesShape.filter, {"v": {"$eq": "?number"}});

    const aggOnTimeseriesShape = getQueryStats(conn, {
                                     extraMatch: {
                                         "key.collectionType": "timeseries",
                                         "key.queryShape.command": "aggregate",
                                         "key.queryShape.cmdNs.coll": jsTestName() + "_timeseries"
                                     }
                                 })[0]
                                     .key.queryShape;
    assert.eq(aggOnTimeseriesShape.pipeline, []);
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
