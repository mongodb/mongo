/**
 * Test ensures that users can specify non-default collation when querying on time-series
 * collections.
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   requires_pipeline_optimization,
 *   does_not_support_stepdowns,
 *   multiversion_incompatible,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.timeseries_nondefault_collation;
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

coll.drop();  // implicitly drops bucketsColl.

const timeFieldName = 'time';
const metaFieldName = 'meta';

const numericOrdering = {
    collation: {locale: "en_US", numericOrdering: true}
};

const caseSensitive = {
    collation: {locale: "en_US", strength: 1, caseLevel: true, numericOrdering: true}
};

const diacriticSensitive = {
    collation: {locale: "en_US", strength: 2}
};

const englishCollation = {
    locale: 'en',
    strength: 1
};

const simpleCollation = {
    collation: {locale: "simple"}
};

assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    collation: englishCollation
}));
assert.contains(bucketsColl.getName(), db.getCollectionNames());

assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "1", name: 'A', name2: "á"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "2", name: 'a', name2: "á"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "5", name: 'A', name2: "á"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "10", name: 'a', name2: "á"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "20", name: 'A', name2: "a"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "50", name: 'B', name2: "a"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "100", name: 'b', name2: "a"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "200", name: 'B', name2: "a"}));
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: "500", name: 'b', name2: "a"}));

// Default collation is case and diacretic insensitive.
assert.eq(2, coll.aggregate([{$sortByCount: "$name"}]).itcount());
assert.eq(1, coll.aggregate([{$sortByCount: "$name2"}]).itcount());

// Test that a explicit collation different from collection's default passes for a timeseries
// collection.
let results =
    coll.aggregate([{$bucket: {groupBy: "$meta", boundaries: ["1", "10", "100", "1000"]}}],
                   numericOrdering)
        .toArray();
assert.eq(3, results.length);
assert.eq({_id: "1", count: 3}, results[0]);
assert.eq({_id: "10", count: 3}, results[1]);
assert.eq({_id: "100", count: 3}, results[2]);

assert.eq(4, coll.aggregate([{$sortByCount: "$name"}], caseSensitive).itcount());
assert.eq(2, coll.aggregate([{$sortByCount: "$name2"}], diacriticSensitive).itcount());

coll.drop();
const defaultCollation = {
    locale: "en",
    numericOrdering: true,
    caseLevel: true,
    strength: 2
};
assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    collation: defaultCollation
}));
assert.contains(bucketsColl.getName(), db.getCollectionNames());
assert.commandWorked(coll.createIndex({[metaFieldName]: 1}, {collation: {locale: "simple"}}));

assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: 1, name: 'A', name2: "á", value: "1"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: 2, name: 'a', name2: "á", value: "11"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: 1, name: 'A', name2: "á", value: "50"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: 1, name: 'a', name2: "á", value: "100"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: "2", name: 'A', name2: "a", value: "3"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: "5", name: 'B', name2: "a", value: "-100"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: "1", name: 'b', name2: "a", value: "-200"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: "2", name: 'B', name2: "a", value: "1000"}));
assert.commandWorked(coll.insert(
    {[timeFieldName]: ISODate(), [metaFieldName]: "5", name: 'b', name2: "a", value: "4"}));

// This collection has been created using non simple collation. The collection was then indexed on
// its metadata using simple collation. These tests confirm that queries on the indexed field using
// nondefault (simple) collation use the index. They also confirm that queries that don't involve
// strings but do use default collation, on indexed fields, also use the index.
const nonDefaultCollationQuery = coll.find({meta: 2}, {collation: englishCollation}).explain();
assert(aggPlanHasStage(nonDefaultCollationQuery, "IXSCAN"), nonDefaultCollationQuery);

const simpleNonDefaultCollationQuery = coll.find({meta: 2}, simpleCollation).explain();
assert(aggPlanHasStage(simpleNonDefaultCollationQuery, "IXSCAN"), simpleNonDefaultCollationQuery);

const defaultCollationQuery = coll.find({meta: 1}, {collation: defaultCollation}).explain();
assert(aggPlanHasStage(defaultCollationQuery, "IXSCAN"), defaultCollationQuery);

// This test guarantees that the bucket's min/max matches the query's min/max regardless of
// collation.
results = coll.find({value: {$gt: "4"}}, simpleCollation);
assert.eq(4, results.itcount());
}());
