/**
 * Tests for serverStatus metrics.stage stats.
 * @tags: [
 *   requires_sharding,
 * ]
 */
// In memory map of stage names to their counters. Used to verify that serverStatus is
// incrementing the appropriate stages correctly across multiple pipelines.
let countersWeExpectToIncreaseMap = {};

function checkCounters(command, countersWeExpectToIncrease, countersWeExpectNotToIncrease = []) {
    // Capture the pre-aggregation counts of the stages which we expect not to increase.
    let metrics = db.serverStatus().metrics.aggStageCounters;
    let noIncreaseCounterMap = {};
    for (const stage of countersWeExpectNotToIncrease) {
        if (!countersWeExpectToIncreaseMap[stage]) {
            countersWeExpectToIncreaseMap[stage] = 0;
        }
        noIncreaseCounterMap[stage] = metrics[stage];
    }

    // Update in memory map to reflect what each counter's count should be after running 'command'.
    for (const stage of countersWeExpectToIncrease) {
        if (!countersWeExpectToIncreaseMap[stage]) {
            countersWeExpectToIncreaseMap[stage] = 0;
        }
        countersWeExpectToIncreaseMap[stage]++;
    }

    // Run the command and update metrics to reflect the post-command serverStatus state.
    command();
    metrics = db.serverStatus().metrics.aggStageCounters;

    // Verify that serverStatus reflects expected counters.
    for (const stage of countersWeExpectToIncrease) {
        assert.eq(metrics[stage], countersWeExpectToIncreaseMap[stage]);
    }

    // Verify that the counters which we expect not to increase did not do so.
    for (const stage of countersWeExpectNotToIncrease) {
        assert.eq(metrics[stage], noIncreaseCounterMap[stage]);
    }
}

function runTests(db, coll) {
    // Reset our counter map before running any aggregations.
    countersWeExpectToIncreaseMap = {};

    // Setup for agg stages which have nested pipelines.
    assert.commandWorked(coll.insert([
        {"_id": 1, "item": "almonds", "price": 12, "quantity": 2},
        {"_id": 2, "item": "pecans", "price": 20, "quantity": 1},
        {"_id": 3}
    ]));

    assert.commandWorked(db.inventory.insert([
        {"_id": 1, "sku": "almonds", description: "product 1", "instock": 120},
        {"_id": 2, "sku": "bread", description: "product 2", "instock": 80},
        {"_id": 3, "sku": "cashews", description: "product 3", "instock": 60},
        {"_id": 4, "sku": "pecans", description: "product 4", "instock": 70},
        {"_id": 5, "sku": null, description: "Incomplete"},
        {"_id": 6}
    ]));

    // $skip
    checkCounters(() => coll.aggregate([{$skip: 5}]).toArray(), ['$skip']);
    // $project is an alias for $unset.
    checkCounters(() => coll.aggregate([{$project: {title: 1, author: 1}}]).toArray(),
                  ['$project'],
                  ['$unset']);
    // $count is an alias for $project and $group.
    checkCounters(
        () => coll.aggregate([{$count: "test"}]).toArray(), ['$count'], ['$project', '$group']);

    // $lookup
    checkCounters(
        () => coll.aggregate([{$lookup: {from: "inventory", pipeline: [{$match: {inStock: 70}}], as: "inventory_docs"}}]).toArray(),
        ['$lookup', "$match"]);

    // $merge
    checkCounters(
        () => coll.aggregate([{
                      $merge:
                          {into: coll.getName(), whenMatched: [{$set: {a: {$multiply: ["$a", 2]}}}]}
                  }])
                  .toArray(),
        ['$merge', "$set"]);

    // $facet
    checkCounters(
        () =>
            coll.aggregate([{
                    $facet: {"a": [{$match: {price: {$exists: 1}}}], "b": [{$project: {title: 1}}]}
                }])
                .toArray(),
        ['$facet', '$match', "$project"]);

    // Verify that explain ticks counters.
    checkCounters(() => coll.explain().aggregate([{$match: {a: 5}}]), ["$match"]);

    // Verify that a pipeline in an update ticks counters.
    checkCounters(() => coll.update(
                      {price: {$gte: 0}}, [{$addFields: {a: {$add: ['$a', 1]}}}], {multi: true}),
                  ["$addFields"],
                  ["$set"]);

    // Verify that a stage which appears multiple times in a pipeline has an accurate count.
    checkCounters(
        () =>
            coll.aggregate([
                    {
                        $facet:
                            {"a": [{$match: {price: {$exists: 1}}}], "b": [{$project: {title: 1}}]}
                    },
                    {
                        $facet: {
                            "c": [{$match: {instock: {$exists: 1}}}],
                            "d": [{$project: {title: 0}}]
                        }
                    }
                ])
                .toArray(),
        ["$facet", "$match", "$project", "$facet", "$match", "$project"]);

    // Verify that a pipeline used in a view ticks counters.
    const viewName = "counterView";
    assert.commandWorked(db.createView(viewName, coll.getName(), [{"$project": {_id: 0}}]));
    // Note that $project's counter will also be ticked since the $project used to generate the view
    // will be stitched together with the pipeline specified to the aggregate command.
    checkCounters(() => db[viewName].aggregate([{$match: {a: 5}}]).toArray(),
                  ["$match", "$project"]);

    // Failed aggregations should still increment the counters.
    checkCounters(() => {
        assert.commandFailed(db.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$project: {x: {$add: [6, "$item"]}}}],
            cursor: {}
        }));
    }, ["$project"])
}

// Standalone
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
let db = conn.getDB(jsTest.name());
const collName = jsTest.name();
let coll = db[collName];
runTests(db, coll);

MongoRunner.stopMongod(conn);

// Sharded cluster
const st = new ShardingTest({shards: 2});
db = st.s.getDB(jsTest.name());
coll = db[collName];
st.shardColl(coll, {_id: 1}, {_id: "hashed"});

runTests(db, coll);

st.stop();