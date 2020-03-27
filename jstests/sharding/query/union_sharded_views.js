// Tests that $unionWith can successfully read from a view that is backed by a sharded collection.
// @tags: [
//   requires_fcv_44,  # Uses $unionWith
//   requires_find_command,  # Uses views, which do not support OP_QUERY.
// ]
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For 'resultsEq'.

const st = new ShardingTest({shards: 2});

const db = st.s.getDB("test");
const sourceUnsharded = db.source_collection_unsharded;
const unshardedData = [
    {_id: 0},
    {_id: 1},
    {_id: 2},
    {_id: 3},
    {_id: 4},
    {_id: 5},
    {_id: 6},
    {_id: 7},
    {_id: 8},
    {_id: 9},
];
assert.commandWorked(sourceUnsharded.insert(unshardedData));
// Shard a backing collection and distribute amongst the two shards.
const sourceSharded = db.source_collection_sharded;
st.shardColl(sourceSharded, {shardKey: 1}, {shardKey: 0}, {shardKey: 1}, db.getName());

const shardedData = [
    {_id: 0, shardKey: -100},
    {_id: 1, shardKey: -100},
    {_id: 2, shardKey: -10},
    {_id: 3, shardKey: -10},
    {_id: 4, shardKey: -1},
    {_id: 5, shardKey: 0},
    {_id: 6, shardKey: 1},
    {_id: 7, shardKey: 10},
    {_id: 8, shardKey: 10},
    {_id: 9, shardKey: 100},
    {_id: 10, shardKey: 100},
];
assert.commandWorked(sourceSharded.insert(shardedData));

// Test that we can query the backing collection normally.
assert.eq(sourceSharded.aggregate().itcount(), shardedData.length);
assert.eq(sourceUnsharded.aggregate().itcount(), unshardedData.length);
assert.eq(sourceSharded.aggregate([{$unionWith: sourceUnsharded.getName()}]).itcount(),
          shardedData.length + unshardedData.length);
assert.eq(sourceUnsharded.aggregate([{$unionWith: sourceSharded.getName()}]).itcount(),
          shardedData.length + unshardedData.length);

// Now create an identity view on top of each collection and expect to get the same results.
const identityUnsharded = db.identity_unsharded;
assert.commandWorked(db.runCommand(
    {create: identityUnsharded.getName(), viewOn: sourceUnsharded.getName(), pipeline: []}));

const identitySharded = db.identity_sharded;
assert.commandWorked(db.runCommand(
    {create: identitySharded.getName(), viewOn: sourceSharded.getName(), pipeline: []}));

assert.eq(identitySharded.aggregate().itcount(), shardedData.length);
assert.eq(identityUnsharded.aggregate().itcount(), unshardedData.length);
assert.eq(identitySharded.aggregate([{$unionWith: identityUnsharded.getName()}]).itcount(),
          shardedData.length + unshardedData.length);
assert.eq(identityUnsharded.aggregate([{$unionWith: identitySharded.getName()}]).itcount(),
          shardedData.length + unshardedData.length);

// Now create some views with some actual definitions.
const groupedByShardKey = db.grouped_by_sk;
assert.commandWorked(db.runCommand({
    create: groupedByShardKey.getName(),
    viewOn: sourceSharded.getName(),
    pipeline: [{$group: {_id: "$shardKey", count: {$sum: 1}}}]
}));
assert(resultsEq(sourceUnsharded.aggregate([{$unionWith: groupedByShardKey.getName()}]).toArray(),
                 unshardedData.concat([
                     {_id: -100, count: 2},
                     {_id: -10, count: 2},
                     {_id: -1, count: 1},
                     {_id: 0, count: 1},
                     {_id: 1, count: 1},
                     {_id: 10, count: 2},
                     {_id: 100, count: 2},
                 ])));
assert(resultsEq(sourceUnsharded
                     .aggregate([
                         {$group: {_id: null, count: {$sum: 1}}},
                         {$unionWith: groupedByShardKey.getName()},
                         {$match: {count: {$gt: 1}}}
                     ])
                     .toArray(),
                 [
                     {_id: null, count: unshardedData.length},
                     {_id: -100, count: 2},
                     {_id: -10, count: 2},
                     {_id: 10, count: 2},
                     {_id: 100, count: 2},
                 ]));

const onlySmallIdsFromSharded = db.only_small_ids_sharded;
assert.commandWorked(db.runCommand({
    create: onlySmallIdsFromSharded.getName(),
    viewOn: sourceSharded.getName(),
    pipeline: [{$match: {_id: {$lte: 4}}}]
}));
assert.eq(sourceUnsharded.aggregate([{$unionWith: onlySmallIdsFromSharded.getName()}]).itcount(),
          unshardedData.length + 5);
assert(resultsEq(sourceUnsharded
                     .aggregate([{
                         $unionWith: {
                             coll: onlySmallIdsFromSharded.getName(),
                             pipeline: [{$group: {_id: "$shardKey", count: {$sum: 1}}}]
                         }
                     }])
                     .toArray(),
                 unshardedData.concat([
                     {_id: -100, count: 2},
                     {_id: -10, count: 2},
                     {_id: -1, count: 1},
                 ])));

const onlySmallIdsFromUnsharded = db.only_small_ids_unsharded;
assert.commandWorked(db.runCommand({
    create: onlySmallIdsFromUnsharded.getName(),
    viewOn: sourceUnsharded.getName(),
    pipeline: [{$match: {_id: {$lte: 4}}}]
}));
assert(resultsEq(onlySmallIdsFromUnsharded
                     .aggregate([{
                         $unionWith: {
                             coll: onlySmallIdsFromSharded.getName(),
                             pipeline: [{$group: {_id: "$shardKey", count: {$sum: 1}}}]
                         }
                     }])
                     .toArray(),
                 [
                     {_id: 0},
                     {_id: 1},
                     {_id: 2},
                     {_id: 3},
                     {_id: 4},
                     {_id: -100, count: 2},
                     {_id: -10, count: 2},
                     {_id: -1, count: 1},
                 ]));

// Now test that $unionWith can be stored in a view that is backed by a sharded collection.

/**
 * Assuming 'unionedView' is a union of two collections containing 'shardedData' and
 * 'unshardedData', runs a couple queries to confirm all the expected results show up.
 */
function testQueryOverUnionedData(unionedView) {
    assert(resultsEq(unionedView.find().toArray(), shardedData.concat(unshardedData)));
    assert(resultsEq(
        unionedView.aggregate([{$group: {_id: "$shardKey", count: {$sum: 1}}}]).toArray(), [
            {_id: null, count: unshardedData.length},
            {_id: -100, count: 2},
            {_id: -10, count: 2},
            {_id: -1, count: 1},
            {_id: 0, count: 1},
            {_id: 1, count: 1},
            {_id: 10, count: 2},
            {_id: 100, count: 2},
        ]));
}
assert.commandWorked(db.runCommand({
    create: "union_view",
    viewOn: sourceSharded.getName(),
    pipeline: [{$unionWith: sourceUnsharded.getName()}]
}));
testQueryOverUnionedData(db["union_view"]);
// Now test that $unionWith can be stored in a view that is backed by an unsharded collection, but
// the $unionWith is targeting a sharded collection.
assert.commandWorked(db.runCommand({
    collMod: "union_view",
    viewOn: sourceUnsharded.getName(),
    pipeline: [{$unionWith: sourceSharded.getName()}]
}));
testQueryOverUnionedData(db["union_view"]);

st.stop();
}());
