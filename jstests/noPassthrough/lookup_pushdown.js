/**
 * Tests basic functionality of pushing $lookup into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

function runTest(coll, pipeline, expectedToPushdown) {
    const cmd = () => coll.aggregate(pipeline).toArray();
    if (expectedToPushdown) {
        assert.throwsWithCode(cmd, 5843700);
    } else {
        assert.doesNotThrow(cmd);
    }
}

// Standalone cases.
const conn = MongoRunner.runMongod({setParameter: "internalEnableMultipleAutoGetCollections=true"});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
let db = conn.getDB(name);

if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    jsTestLog("Skipping test because the sbe lookup pushdown feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const foreignCollName = "foreign_lookup_pushdown";
const viewName = "view_lookup_pushdown";
let coll = db[name];
assert.commandWorked(coll.insert({_id: 1, a: 2}));
let foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({_id: 0, b: 2}));
assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
let view = db[viewName];

// Basic $lookup.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        true /* expectedToPushdown */);

// Self join $lookup, no views.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        true /* expectedToPushdown */);

// Self join $lookup; left hand is a view. This is expected to be pushed down because the view
// pipeline itself is a $match, which is eligible for pushdown.
runTest(view,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        true /* expectedToPushdown */);

// Self join $lookup; right hand is a view.
runTest(coll,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        false /* expectedToPushdown */);

// Self join $lookup; both namespaces are views.
runTest(view,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        false /* expectedToPushdown */);

// $lookup preceded by $match.
runTest(coll,
        [
            {$match: {a: {$gte: 0}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        true /* expectedToPushdown */);

// $lookup preceded by $project.
runTest(coll,
        [
            {$project: {a: 1}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        true /* expectedToPushdown */);

// $lookup preceded by $project which features an SBE-incompatible expression.
// TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
runTest(coll,
        [
            {$project: {exp: {$pow: ["$a", 3]}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        false /* expectedToPushdown */);

// $lookup preceded by $group.
runTest(coll,
        [
            {$group: {_id: "$a", sum: {$sum: 1}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        true /* expectedToPushdown */);

// Consecutive $lookups (first is against view).
runTest(coll,
        [
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        false /* expectedToPushdown */);

// Consecutive $lookups (first is against regular collection).
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}
        ],
        true /* expectedToPushdown */);

// $lookup with pipeline.
runTest(coll,
    [{$lookup: {from: foreignCollName, let: {foo: "$b"}, pipeline: [{$match: {$expr: {$eq: ["$$foo",
2]}}}], as: "out"}}], false /* expectedToPushdown */);

// $lookup that absorbs $unwind.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"}
        ],
        false /* expectedToPushdown */);

// $lookup that absorbs $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"},
            {$match: {out: {$gte: 0}}}
        ],
        false /* expectedToPushdown */);

// $lookup that does not absorb $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$match: {out: {$gte: 0}}}
        ],
        true /* expectedToPushdown */);

MongoRunner.stopMongod(conn);

// Sharded cases.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {shardOptions: {setParameter: "internalEnableMultipleAutoGetCollections=true"}}
});
db = st.s.getDB(name);

// Setup. Here, 'coll' is sharded, 'foreignColl' is unsharded, 'viewName' is an unsharded view,
// and 'shardedViewName' is a sharded view.
const shardedViewName = "sharded_foreign_view";
coll = db[name];
assert.commandWorked(coll.insert({a: 1, shardKey: 1}));
assert.commandWorked(coll.insert({a: 2, shardKey: 10}));
assert.commandWorked(coll.createIndex({shardKey: 1}));
st.shardColl(coll.getName(), {shardKey: 1}, {shardKey: 5}, {shardKey: 5}, name);

foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({b: 5}));

assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
assert.commandWorked(db.createView(shardedViewName, name, [{$match: {b: {$gte: 0}}}]));

// Both collections are unsharded.
runTest(foreignColl,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        true /* expectedToPushdown */);

// Sharded main collection, unsharded right side. This is not expected to be eligible for pushdown
// because the $lookup will be preceded by a $mergeCursors stage on the merging shard.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedToPushdown */);

// Both collections are sharded.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedToPushdown */);

// Unsharded main collection, sharded right side.
runTest(foreignColl,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedToPushdown */);

// Unsharded main collection, unsharded view right side.
runTest(foreignColl,
        [{$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedToPushdown */);

// Unsharded main collection, sharded view on the right side.
runTest(foreignColl,
        [{$lookup: {from: shardedViewName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedToPushdown */);
st.stop();
}());
