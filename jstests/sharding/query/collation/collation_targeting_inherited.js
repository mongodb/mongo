// Test shard targeting for queries on a collection with a default collation.
// @tags : [requires_scripting]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const caseInsensitive = {
    locale: "en_US",
    strength: 2,
};

let explain;
let writeRes;

// Create a cluster with 3 shards.
let st = new ShardingTest({shards: 3});
let testDB = st.s.getDB("test");
assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard1.shardName}));

// Create a collection with a case-insensitive default collation sharded on {a: 1}.
let collCaseInsensitive = testDB.getCollection("case_insensitive");
collCaseInsensitive.drop();
assert.commandWorked(testDB.createCollection("case_insensitive", {collation: caseInsensitive}));
assert.commandWorked(collCaseInsensitive.createIndex({a: 1}, {collation: {locale: "simple"}}));
assert.commandWorked(collCaseInsensitive.createIndex({geo: "2dsphere"}));
assert.commandWorked(
    testDB.adminCommand({
        shardCollection: collCaseInsensitive.getFullName(),
        key: {a: 1},
        collation: {locale: "simple"},
    }),
);

// Split the collection.
// st.shard0.shardName: { "a" : { "$minKey" : 1 } } -->> { "a" : 10 }
// st.shard1.shardName: { "a" : 10 } -->> { "a" : "a"}
// shard0002: { "a" : "a" } -->> { "a" : { "$maxKey" : 1 }}
assert.commandWorked(testDB.adminCommand({split: collCaseInsensitive.getFullName(), middle: {a: 10}}));
assert.commandWorked(testDB.adminCommand({split: collCaseInsensitive.getFullName(), middle: {a: "a"}}));
assert.commandWorked(
    testDB.adminCommand({moveChunk: collCaseInsensitive.getFullName(), find: {a: 1}, to: st.shard0.shardName}),
);
assert.commandWorked(
    testDB.adminCommand({moveChunk: collCaseInsensitive.getFullName(), find: {a: "FOO"}, to: st.shard1.shardName}),
);
assert.commandWorked(
    testDB.adminCommand({moveChunk: collCaseInsensitive.getFullName(), find: {a: "foo"}, to: st.shard2.shardName}),
);

// Put data on each shard.
// Note that the balancer is off by default, so the chunks will stay put.
// st.shard0.shardName: {a: 1}
// st.shard1.shardName: {a: 100}, {a: "FOO"}
// shard0002: {a: "foo"}
// Include geo field to test geoNear.
let a_1 = {_id: 0, a: 1, geo: {type: "Point", coordinates: [0, 0]}};
let a_100 = {_id: 1, a: 100, geo: {type: "Point", coordinates: [0, 0]}};
let a_FOO = {_id: 2, a: "FOO", geo: {type: "Point", coordinates: [0, 0]}};
let a_foo = {_id: 3, a: "foo", geo: {type: "Point", coordinates: [0, 0]}};
assert.commandWorked(collCaseInsensitive.insert(a_1));
assert.commandWorked(collCaseInsensitive.insert(a_100));
assert.commandWorked(collCaseInsensitive.insert(a_FOO));
assert.commandWorked(collCaseInsensitive.insert(a_foo));

// Aggregate.

// Test an aggregate command on strings with a non-simple collation inherited from the
// collection default. This should be scatter-gather.
assert.eq(2, collCaseInsensitive.aggregate([{$match: {a: "foo"}}]).itcount());
explain = collCaseInsensitive.explain().aggregate([{$match: {a: "foo"}}]);
assert.commandWorked(explain);
assert.eq(3, Object.keys(explain.shards).length);

// Test an aggregate command with a simple collation. This should be single-shard.
assert.eq(1, collCaseInsensitive.aggregate([{$match: {a: "foo"}}], {collation: {locale: "simple"}}).itcount());
explain = collCaseInsensitive.explain().aggregate([{$match: {a: "foo"}}], {collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Test an aggregate command on numbers with a non-simple collation inherited from the
// collection default. This should be single-shard.
assert.eq(1, collCaseInsensitive.aggregate([{$match: {a: 100}}]).itcount());
explain = collCaseInsensitive.explain().aggregate([{$match: {a: 100}}]);
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Aggregate with $geoNear.
const geoJSONPoint = {
    type: "Point",
    coordinates: [0, 0],
};

// Test $geoNear with a query on strings with a non-simple collation inherited from the
// collection default. This should scatter-gather.
const geoNearStageStringQuery = [
    {
        $geoNear: {
            near: geoJSONPoint,
            distanceField: "dist",
            spherical: true,
            query: {a: "foo"},
        },
    },
];
assert.eq(2, collCaseInsensitive.aggregate(geoNearStageStringQuery).itcount());
explain = collCaseInsensitive.explain().aggregate(geoNearStageStringQuery);
assert.commandWorked(explain);
assert.eq(3, Object.keys(explain.shards).length);

// Test $geoNear with a query on strings with a simple collation. This should be single-shard.
assert.eq(1, collCaseInsensitive.aggregate(geoNearStageStringQuery, {collation: {locale: "simple"}}).itcount());
explain = collCaseInsensitive.explain().aggregate(geoNearStageStringQuery, {collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Test a $geoNear with a query on numbers with a non-simple collation inherited from the
// collection default. This should be single-shard.
const geoNearStageNumericalQuery = [
    {
        $geoNear: {
            near: geoJSONPoint,
            distanceField: "dist",
            spherical: true,
            query: {a: 100},
        },
    },
];
assert.eq(1, collCaseInsensitive.aggregate(geoNearStageNumericalQuery).itcount());
explain = collCaseInsensitive.explain().aggregate(geoNearStageNumericalQuery);
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Count.

// Test a count command on strings with a non-simple collation inherited from the collection
// default. This should be scatter-gather.
assert.eq(2, collCaseInsensitive.find({a: "foo"}).count());
explain = collCaseInsensitive.explain().find({a: "foo"}).count();
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test a count command with a simple collation. This should be single-shard.
assert.eq(1, collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).count());
explain = collCaseInsensitive.explain().find({a: "foo"}).collation({locale: "simple"}).count();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a find command on numbers with a non-simple collation inheritied from the collection
// default. This should be single-shard.
assert.eq(1, collCaseInsensitive.find({a: 100}).count());
explain = collCaseInsensitive.explain().find({a: 100}).count();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Distinct.

// Test a distinct command on strings with a non-simple collation inherited from the collection
// default. This should be scatter-gather.
assert.eq(2, collCaseInsensitive.distinct("_id", {a: "foo"}).length);
explain = collCaseInsensitive.explain().distinct("_id", {a: "foo"});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test that deduping respects the collation inherited from the collection default.
assert.eq(1, collCaseInsensitive.distinct("a", {a: "foo"}).length);

// Test a distinct command with a simple collation. This should be single-shard.
assert.eq(1, collCaseInsensitive.distinct("_id", {a: "foo"}, {collation: {locale: "simple"}}).length);
explain = collCaseInsensitive.explain().distinct("_id", {a: "foo"}, {collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a distinct command on numbers with a non-simple collation inherited from the collection
// default. This should be single-shard.
assert.eq(1, collCaseInsensitive.distinct("_id", {a: 100}).length);
explain = collCaseInsensitive.explain().distinct("_id", {a: 100});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Find.

// Test a find command on strings with a non-simple collation inherited from the collection
// default. This should be scatter-gather.
assert.eq(2, collCaseInsensitive.find({a: "foo"}).itcount());
explain = collCaseInsensitive.find({a: "foo"}).explain();
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test a find command with a simple collation. This should be single-shard.
assert.eq(1, collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).itcount());
explain = collCaseInsensitive.find({a: "foo"}).collation({locale: "simple"}).explain();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a find command on numbers with a non-simple collation inherited from the collection
// default. This should be single-shard.
assert.eq(1, collCaseInsensitive.find({a: 100}).itcount());
explain = collCaseInsensitive.find({a: 100}).explain();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// FindAndModify.

// Sharded findAndModify that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
let res = collCaseInsensitive.findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}});
assert(res.a === "foo" || res.a === "FOO");
explain = collCaseInsensitive.explain().findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded findAndModify on strings with simple collation should succeed. This should be
// single-shard.
assert.eq(
    "foo",
    collCaseInsensitive.findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}, collation: {locale: "simple"}}).a,
);
explain = collCaseInsensitive
    .explain()
    .findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}, collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded findAndModify on numbers with non-simple collation inherited from collection default
// should succeed. This should be single-shard.
assert.eq(100, collCaseInsensitive.findAndModify({query: {a: 100}, update: {$set: {b: 1}}}).a);
explain = collCaseInsensitive.explain().findAndModify({query: {a: 100}, update: {$set: {b: 1}}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// MapReduce.

// Test that the filter on mapReduce respects the non-simple collation inherited from the
// collection default.
assert.eq(
    2,
    assert.commandWorked(
        collCaseInsensitive.mapReduce(
            function () {
                emit(this._id, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: "foo"}},
        ),
    ).results.length,
);

// Test that mapReduce respects the non-simple default collation for the emitted keys. In this
// case, the emitted keys "foo" and "FOO" should be considered equal.
assert.eq(
    1,
    assert.commandWorked(
        collCaseInsensitive.mapReduce(
            function () {
                emit(this.a, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: "foo"}},
        ),
    ).results.length,
);

// Test that the filter on mapReduce respects the simple collation if specified.
assert.eq(
    1,
    assert.commandWorked(
        collCaseInsensitive.mapReduce(
            function () {
                emit(this.a, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: "foo"}, collation: {locale: "simple"}},
        ),
    ).results.length,
);

// Test that mapReduce respects the user-specified simple collation for the emitted keys.
assert.eq(
    2,
    assert.commandWorked(
        collCaseInsensitive.mapReduce(
            function () {
                emit(this.a, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: {$type: "string"}}, collation: {locale: "simple"}},
        ),
    ).results.length,
);

// Remove.

// Test a remove command on strings with non-simple collation inherited from collection default.
// This should be scatter-gather.
writeRes = collCaseInsensitive.remove({a: "foo"});
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nRemoved);
explain = collCaseInsensitive.explain().remove({a: "foo"});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(collCaseInsensitive.insert(a_FOO));
assert.commandWorked(collCaseInsensitive.insert(a_foo));

// Test a remove command on strings with simple collation. This should be single-shard.
writeRes = collCaseInsensitive.remove({a: "foo"}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = collCaseInsensitive.explain().remove({a: "foo"}, {collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(collCaseInsensitive.insert(a_foo));

// Test a remove command on numbers with non-simple collation inherited from collection default.
// This should be single-shard.
writeRes = collCaseInsensitive.remove({a: 100});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = collCaseInsensitive.explain().remove({a: 100});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(collCaseInsensitive.insert(a_100));

// Sharded deleteOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
let beforeNumDocsMatch = collCaseInsensitive.find({a: "foo"}).collation(caseInsensitive).count();
writeRes = assert.commandWorked(collCaseInsensitive.remove({a: "foo"}, {justOne: true}));
assert.eq(1, writeRes.nRemoved);
let afterNumDocsMatch = collCaseInsensitive.find({a: "foo"}).collation(caseInsensitive).count();
assert.eq(beforeNumDocsMatch - 1, afterNumDocsMatch);
explain = collCaseInsensitive.explain().remove({a: "foo"}, {justOne: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Re-insert documents for later test cases.
collCaseInsensitive.insert(a_foo);
collCaseInsensitive.insert(a_FOO);

// Single remove on number shard key with non-simple collation inherited from collection default
// should succeed, because it is single-shard.
writeRes = collCaseInsensitive.remove({a: 100}, {justOne: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = collCaseInsensitive.explain().remove({a: 100}, {justOne: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(collCaseInsensitive.insert(a_100));

// Sharded deleteOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
assert.commandWorked(collCaseInsensitive.insert({_id: "foo_scoped", a: "bar"}));
writeRes = collCaseInsensitive.remove({_id: "foo_scoped"}, {justOne: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Single remove on string _id with collection-default collation should succeed, because it is
// an exact-ID query.
assert.commandWorked(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
writeRes = collCaseInsensitive.remove({_id: "foo"}, {justOne: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Single remove on string _id with collection-default collation explicitly given should
// succeed, because it is an exact-ID query.
assert.commandWorked(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
writeRes = collCaseInsensitive.remove({_id: "foo"}, {justOne: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Single remove on number _id with non-collection-default collation should succeed, because it
// is an exact-ID query.
writeRes = collCaseInsensitive.remove({_id: a_100._id}, {justOne: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
assert.commandWorked(collCaseInsensitive.insert(a_100));

// Update.

// Test an update command on strings with non-simple collation inherited from collection
// default. This should be scatter-gather.
writeRes = collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: "foo"}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test an update command on strings with simple collation. This should be single-shard.
writeRes = collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {multi: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive
    .explain()
    .update({a: "foo"}, {$set: {b: 1}}, {multi: true, collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test an update command on numbers with non-simple collation inherited from collection
// default. This should be single-shard.
writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded updateOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
writeRes = assert.commandWorked(collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}));
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: "foo"}, {$set: {b: 1}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Single update on string shard key with simple collation should succeed, because it is
// single-shard.
writeRes = collCaseInsensitive.update({a: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Single update on number shard key with non-simple collation inherited from collation default
// should succeed, because it is single-shard.
writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded updateOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
assert.commandWorked(collCaseInsensitive.update({_id: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}}));

// Single update on string _id with collection-default collation should succeed, because it is
// an exact-ID query.
assert.commandWorked(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
writeRes = collCaseInsensitive.update({_id: "foo"}, {$set: {b: 1}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
assert.commandWorked(collCaseInsensitive.remove({_id: "foo"}, {justOne: true}));

// Single update on string _id with collection-default collation explicitly given should
// succeed, because it is an exact-ID query.
assert.commandWorked(collCaseInsensitive.insert({_id: "foo", a: "bar"}));
writeRes = collCaseInsensitive.update({_id: "foo"}, {$set: {b: 1}}, {collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
assert.commandWorked(collCaseInsensitive.remove({_id: "foo"}, {justOne: true}));

// Single update on number _id with non-collection-default collation inherited from collection
// default should succeed, because it is an exact-ID query.
writeRes = collCaseInsensitive.update({_id: a_foo._id}, {$set: {b: 1}}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);

// Sharded upsert that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
writeRes = collCaseInsensitive.update({a: "filter"}, {$set: {b: 1}}, {multi: false, upsert: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nUpserted);

// Upsert on strings with simple collation should succeed, because it is single-shard.
writeRes = collCaseInsensitive.update(
    {a: "foo"},
    {$set: {b: 1}},
    {multi: true, upsert: true, collation: {locale: "simple"}},
);
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive
    .explain()
    .update({a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true, collation: {locale: "simple"}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Upsert on numbers with non-simple collation inherited from collection default should succeed,
// because it is single-shard.
writeRes = collCaseInsensitive.update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = collCaseInsensitive.explain().update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

st.stop();
