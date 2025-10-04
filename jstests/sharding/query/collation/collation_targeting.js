// Test shard targeting for queries with collation.
// @tags: [requires_scripting]
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

// Create a collection sharded on {a: 1}. Add 2dsphere index to test $geoNear.
let coll = testDB.getCollection("simple_collation");
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

// Split the collection.
// st.shard0.shardName: { "a" : { "$minKey" : 1 } } -->> { "a" : 10 }
// st.shard1.shardName: { "a" : 10 } -->> { "a" : "a"}
// shard0002: { "a" : "a" } -->> { "a" : { "$maxKey" : 1 }}
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: 10}}));
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: "a"}}));
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {a: 1}, to: st.shard0.shardName}));
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {a: "FOO"}, to: st.shard1.shardName}));
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {a: "foo"}, to: st.shard2.shardName}));

// Put data on each shard.
// Note that the balancer is off by default, so the chunks will stay put.
// st.shard0.shardName: {a: 1}
// st.shard1.shardName: {a: 100}, {a: "FOO"}
// shard0002: {a: "foo"}
// Include geo field to test $geoNear.
let a_1 = {_id: 0, a: 1, geo: {type: "Point", coordinates: [0, 0]}};
let a_100 = {_id: 1, a: 100, geo: {type: "Point", coordinates: [0, 0]}};
let a_FOO = {_id: 2, a: "FOO", geo: {type: "Point", coordinates: [0, 0]}};
let a_foo = {_id: 3, a: "foo", geo: {type: "Point", coordinates: [0, 0]}};
assert.commandWorked(coll.insert(a_1));
assert.commandWorked(coll.insert(a_100));
assert.commandWorked(coll.insert(a_FOO));
assert.commandWorked(coll.insert(a_foo));

// Aggregate.

// Test an aggregate command on strings with a non-simple collation. This should be
// scatter-gather.
assert.eq(2, coll.aggregate([{$match: {a: "foo"}}], {collation: caseInsensitive}).itcount());
explain = coll.explain().aggregate([{$match: {a: "foo"}}], {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(3, Object.keys(explain.shards).length);

// Test an aggregate command with a simple collation. This should be single-shard.
assert.eq(1, coll.aggregate([{$match: {a: "foo"}}]).itcount());
explain = coll.explain().aggregate([{$match: {a: "foo"}}]);
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Test an aggregate command on numbers with a non-simple collation. This should be
// single-shard.
assert.eq(1, coll.aggregate([{$match: {a: 100}}], {collation: caseInsensitive}).itcount());
explain = coll.explain().aggregate([{$match: {a: 100}}], {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Aggregate with $geoNear.
const geoJSONPoint = {
    type: "Point",
    coordinates: [0, 0],
};

// Test $geoNear with a query on strings with a non-simple collation. This should
// scatter-gather.
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
assert.eq(2, coll.aggregate(geoNearStageStringQuery, {collation: caseInsensitive}).itcount());
explain = coll.explain().aggregate(geoNearStageStringQuery, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(3, Object.keys(explain.shards).length);

// Test $geoNear with a query on strings with a simple collation. This should be single-shard.
assert.eq(1, coll.aggregate(geoNearStageStringQuery).itcount());
explain = coll.explain().aggregate(geoNearStageStringQuery);
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Test a $geoNear with a query on numbers with a non-simple collation. This should be
// single-shard.
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
assert.eq(1, coll.aggregate(geoNearStageNumericalQuery, {collation: caseInsensitive}).itcount());
explain = coll.explain().aggregate(geoNearStageNumericalQuery, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, Object.keys(explain.shards).length);

// Count.

// Test a count command on strings with a non-simple collation. This should be scatter-gather.
assert.eq(2, coll.find({a: "foo"}).collation(caseInsensitive).count());
explain = coll.explain().find({a: "foo"}).collation(caseInsensitive).count();
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test a count command with a simple collation. This should be single-shard.
assert.eq(1, coll.find({a: "foo"}).count());
explain = coll.explain().find({a: "foo"}).count();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a count command on numbers with a non-simple collation. This should be single-shard.
assert.eq(1, coll.find({a: 100}).collation(caseInsensitive).count());
explain = coll.explain().find({a: 100}).collation(caseInsensitive).count();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Distinct.

// Test a distinct command on strings with a non-simple collation. This should be
// scatter-gather.
assert.eq(2, coll.distinct("_id", {a: "foo"}, {collation: caseInsensitive}).length);
explain = coll.explain().distinct("_id", {a: "foo"}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test that deduping respects the collation.
assert.eq(1, coll.distinct("a", {a: "foo"}, {collation: caseInsensitive}).length);

// Test a distinct command with a simple collation. This should be single-shard.
assert.eq(1, coll.distinct("_id", {a: "foo"}).length);
explain = coll.explain().distinct("_id", {a: "foo"});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a distinct command on numbers with a non-simple collation. This should be single-shard.
assert.eq(1, coll.distinct("_id", {a: 100}, {collation: caseInsensitive}).length);
explain = coll.explain().distinct("_id", {a: 100}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Find.

// Test a find command on strings with a non-simple collation. This should be scatter-gather.
assert.eq(2, coll.find({a: "foo"}).collation(caseInsensitive).itcount());
explain = coll.find({a: "foo"}).collation(caseInsensitive).explain();
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test a find command with a simple collation. This should be single-shard.
assert.eq(1, coll.find({a: "foo"}).itcount());
explain = coll.find({a: "foo"}).explain();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test a find command on numbers with a non-simple collation. This should be single-shard.
assert.eq(1, coll.find({a: 100}).collation(caseInsensitive).itcount());
explain = coll.find({a: 100}).collation(caseInsensitive).explain();
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// FindAndModify.

// Sharded findAndModify that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
let res = coll.findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}, collation: caseInsensitive});
assert(res.a === "foo" || res.a === "FOO");

explain = coll.explain().findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded findAndModify on strings with simple collation should succeed. This should be
// single-shard.
assert.eq("foo", coll.findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}}).a);
explain = coll.explain().findAndModify({query: {a: "foo"}, update: {$set: {b: 1}}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded findAndModify on numbers with non-simple collation should succeed. This should be
// single-shard.
assert.eq(100, coll.findAndModify({query: {a: 100}, update: {$set: {b: 1}}, collation: caseInsensitive}).a);
explain = coll.explain().findAndModify({query: {a: 100}, update: {$set: {b: 1}}, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// MapReduce.

// Test that the filter on mapReduce respects the non-simple collation from the user.
assert.eq(
    2,
    assert.commandWorked(
        coll.mapReduce(
            function () {
                emit(this._id, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: "foo"}, collation: caseInsensitive},
        ),
    ).results.length,
);

// Test that mapReduce respects the non-simple collation for the emitted keys. In this case, the
// emitted keys "foo" and "FOO" should be considered equal.
assert.eq(
    1,
    assert.commandWorked(
        coll.mapReduce(
            function () {
                emit(this.a, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: "foo"}, collation: caseInsensitive},
        ),
    ).results.length,
);

// Test that the filter on mapReduce respects the simple collation if none is specified.
assert.eq(
    1,
    assert.commandWorked(
        coll.mapReduce(
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

// Test that mapReduce respects the simple collation for the emitted keys. In this case, the
// emitted keys "foo" and "FOO" should *not* be considered equal.
assert.eq(
    2,
    assert.commandWorked(
        coll.mapReduce(
            function () {
                emit(this.a, 1);
            },
            function (key, values) {
                return Array.sum(values);
            },
            {out: {inline: 1}, query: {a: {$type: "string"}}},
        ),
    ).results.length,
);

// Remove.

// Test a remove command on strings with non-simple collation. This should be scatter-gather.
writeRes = coll.remove({a: "foo"}, {collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nRemoved);
explain = coll.explain().remove({a: "foo"}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(coll.insert(a_FOO));
assert.commandWorked(coll.insert(a_foo));

// Test a remove command on strings with simple collation. This should be single-shard.
writeRes = coll.remove({a: "foo"});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = coll.explain().remove({a: "foo"});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(coll.insert(a_foo));

// Test a remove command on numbers with non-simple collation. This should be single-shard.
writeRes = coll.remove({a: 100}, {collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = coll.explain().remove({a: 100}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(coll.insert(a_100));

// Sharded deleteOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
let beforeNumDocsMatch = coll.find({a: "foo"}).collation(caseInsensitive).count();
writeRes = assert.commandWorked(coll.remove({a: "foo"}, {justOne: true, collation: caseInsensitive}));
assert.eq(1, writeRes.nRemoved);
let afterNumDocsMatch = coll.find({a: "foo"}).collation(caseInsensitive).count();
assert.eq(beforeNumDocsMatch - 1, afterNumDocsMatch);

explain = coll.explain().remove({a: "foo"}, {justOne: true, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

coll.insert(a_foo);
coll.insert(a_FOO);

// Single remove on number shard key with non-simple collation should succeed, because it is
// single-shard.
writeRes = coll.remove({a: 100}, {justOne: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
explain = coll.explain().remove({a: 100}, {justOne: true, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);
assert.commandWorked(coll.insert(a_100));

// Sharded deleteOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = assert.commandWorked(coll.remove({_id: "foo"}, {justOne: true, collation: caseInsensitive}));
let countDocMatch = coll.find({_id: "foo"}).collation(caseInsensitive).count();
assert.eq(1, writeRes.nRemoved);
assert.eq(0, countDocMatch);

// Single remove on string _id with collection-default collation should succeed, because it is
// an exact-ID query.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = coll.remove({_id: "foo"}, {justOne: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Single remove on string _id with collection-default collation explicitly given should
// succeed, because it is an exact-ID query.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = coll.remove({_id: "foo"}, {justOne: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Single remove on number _id with non-collection-default collation should succeed, because it
// is an exact-ID query.
writeRes = coll.remove({_id: a_100._id}, {justOne: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);
assert.commandWorked(coll.insert(a_100));

// Update.

// Test an update command on strings with non-simple collation. This should be scatter-gather.
writeRes = coll.update({a: "foo"}, {$set: {b: 1}}, {multi: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nMatched);
explain = coll.explain().update({a: "foo"}, {$set: {b: 1}}, {multi: true, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(3, explain.queryPlanner.winningPlan.shards.length);

// Test an update command on strings with simple collation. This should be single-shard.
writeRes = coll.update({a: "foo"}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: "foo"}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Test an update command on numbers with non-simple collation. This should be single-shard.
writeRes = coll.update({a: 100}, {$set: {b: 1}}, {multi: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: 100}, {$set: {b: 1}}, {multi: true, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded updateOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
writeRes = assert.commandWorked(coll.update({a: "foo"}, {$set: {b: 1}}, {collation: caseInsensitive}));
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: "foo"}, {$set: {b: 1}}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Single update on string shard key with simple collation should succeed, because it is
// single-shard.
writeRes = coll.update({a: "foo"}, {$set: {b: 1}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: "foo"}, {$set: {b: 1}});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Single update on number shard key with non-simple collation should succeed, because it is
// single-shard.
writeRes = coll.update({a: 100}, {$set: {b: 1}}, {collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: 100}, {$set: {b: 1}}, {collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Sharded updateOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = assert.commandWorked(coll.update({_id: "foo"}, {$set: {b: 1}}, {collation: caseInsensitive}));
assert.eq(1, writeRes.nMatched);
assert.commandWorked(coll.remove({_id: "foo"}, {justOne: true}));

// Single update on string _id with collection-default collation should succeed, because it is
// an exact-ID query.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = coll.update({_id: "foo"}, {$set: {b: 1}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
assert.commandWorked(coll.remove({_id: "foo"}, {justOne: true}));

// Single update on string _id with collection-default collation explicitly given should
// succeed, because it is an exact-ID query.
assert.commandWorked(coll.insert({_id: "foo", a: "bar"}));
writeRes = coll.update({_id: "foo"}, {$set: {b: 1}}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
assert.commandWorked(coll.remove({_id: "foo"}, {justOne: true}));

// Single update on number _id with non-collection-default collation should succeed, because it
// is an exact-ID query.
writeRes = coll.update({_id: a_foo._id}, {$set: {b: 1}}, {collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);

// Sharded upsert that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
writeRes = coll.update({a: "filter"}, {$set: {b: 1}}, {multi: false, upsert: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nUpserted);
assert.commandWorked(coll.remove({a: "filter"}, {justOne: true}));

// Upsert on strings with simple collation should succeed, because it is single-shard.
writeRes = coll.update({a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: "foo"}, {$set: {b: 1}}, {multi: true, upsert: true});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

// Upsert on numbers with non-simple collation should succeed, because it is single shard.
writeRes = coll.update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true, collation: caseInsensitive});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);
explain = coll.explain().update({a: 100}, {$set: {b: 1}}, {multi: true, upsert: true, collation: caseInsensitive});
assert.commandWorked(explain);
assert.eq(1, explain.queryPlanner.winningPlan.shards.length);

st.stop();
