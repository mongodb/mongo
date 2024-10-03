/**
 * Test that shapification for queries with sub-pipelines happens before view resolution and
 * pipeline optimization.
 * @tags: [requires_fcv_72]
 */
import {
    getLatestQueryStatsEntry,
} from "jstests/libs/query/query_stats_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    }
});

const dbName = jsTestName();
const db = conn.getDB(dbName);

const collName = "base";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({x: 10, y: 20}));

const viewName = "projection_view";
assert.commandWorked(
    db.runCommand({create: viewName, viewOn: collName, pipeline: [{$project: {_id: 1, x: 1}}]}));
const view = db[viewName];

// Tests a basic query on a view as a sanity check.
(function testViewQuery() {
    view.aggregate([{$sort: {x: 1}}]).toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${viewName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq(
        [
            {"$sort": {"x": 1}},
        ],
        key.queryShape.pipeline,
        tojson(key));
})();

// Tests a query where a view reference is in a $lookup pipeline.
(function testViewInLookupPipeline() {
    coll.aggregate([{$lookup: {from: viewName, pipeline: [{$match: {x: 10}}], as: "lookup"}}])
        .toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq(
            [
                {$lookup: 
                    {
                        from: viewName, 
                        as: "lookup",
                        let: {},
                        pipeline: [{$match: {x: {$eq: "?number"}}}], 
                    }
                }
            ],
            key.queryShape.pipeline, tojson(key));
})();

// Tests that the $lookup x $match optimization is applied after shapification.
(function testLookupMatchOptimization() {
    coll.aggregate([
            {$lookup: {from: viewName, pipeline: [{$match: {x: {$gt: 5}}}], as: "lookup"}},
            {$unwind: {path: "$lookup"}},
            {$match: {"lookup.x": 10}}
        ])
        .toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq(
            [
                {$lookup: 
                    {
                        from: viewName, 
                        as: "lookup",
                        let: {},
                        pipeline: [{$match: {x: {$gt: "?number"}}}], 
                    }
                },
                {$unwind: {
                    path: "$lookup"
                }},
                {$match: {
                    "lookup.x": {
                        $eq: "?number"
                    }
                }}
            ],
            key.queryShape.pipeline, tojson(key));
})();

// Tests that the $lookup x $unwind optimization is applied after shapification.
(function testLookupUnwindOptimization() {
    coll.aggregate([
            {$lookup: {from: viewName, pipeline: [{$match: {x: 10}}], as: "lookup"}},
            {$unwind: {path: "$lookup"}}
        ])
        .toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq(
            [
                {$lookup: 
                    {
                        from: viewName, 
                        as: "lookup",
                        let: {},
                        pipeline: [{$match: {x: {$eq: "?number"}}}], 
                    }
                },
                {$unwind: {
                    path: "$lookup"
                }}
            ],
            key.queryShape.pipeline, tojson(key));
})();

// Tests a query where a view reference is in a $graphLookup pipeline.
(function testViewInGraphLookupPipeline() {
    coll.aggregate([{$graphLookup: {from: viewName, startWith: "$x", connectFromField: "x", connectToField: "x", as: "lookup"}}]).toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq(
            [
                {$graphLookup: 
                    {
                        from: viewName, 
                        as: "lookup",
                        connectToField: "x",
                        connectFromField: "x", 
                        startWith: "$x"
                    }
                }
            ],
            key.queryShape.pipeline, tojson(key));
})();

// Tests a query where a view reference is in a $unionWith.
(function testViewInUnionWith() {
    coll.aggregate([{$unionWith: viewName}]).toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq([{$unionWith: {coll: viewName, pipeline: []}}], key.queryShape.pipeline, tojson(key));
})();

// Tests a query where a view reference is in a $unionWith with a sub-pipeline.
(function testViewInUnionWithPipeline() {
    coll.aggregate([{$unionWith: {coll: viewName, pipeline: [{$sort: {x: 1}}]}}]).toArray();

    const stats = getLatestQueryStatsEntry(db);
    const key = stats.key;

    // The view should not be resolved in the key.
    assert.eq({"db": `${dbName}`, "coll": `${collName}`}, key.queryShape.cmdNs, tojson(key));
    assert.eq([{$unionWith: {coll: viewName, pipeline: [{$sort: {x: 1}}]}}],
              key.queryShape.pipeline,
              tojson(key));
})();

MongoRunner.stopMongod(conn);
