/**
 * Correctness tests for TS collections with collation that might not match the explicit collation,
 * specified in the query.
 *
 * Queries on timeseries attempt various optimizations to avoid unpacking of buckets. These rely on
 * the meta field and the control data (currently, min and max), computed for each bucket.
 * Collection's collation might affect the computed control values.
 *
 * @tags: [
 *   # TODO (SERVER-73322): remove
 *   assumes_against_mongod_not_mongos,
 *   requires_non_retryable_writes,
 *   requires_pipeline_optimization,
 *   does_not_support_stepdowns,
 *   multiversion_incompatible,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test relies on specific bucketing behaviour, which isn't guaranteed in
 *   # upgrade/downgrade.
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
import {aggPlanHasStage} from "jstests/libs/analyze_plan.js";

const coll = db.timeseries_nondefault_collation;
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

const numericOrdering = {
    locale: "en_US",
    numericOrdering: true,
    strength: 1  // case and diacritics ignored
};
const caseSensitive = {
    locale: "en_US",
    strength: 1,
    caseLevel: true
};
const diacriticSensitive = {
    locale: "en_US",
    strength: 2,
    caseLevel: false
};
const insensitive = {
    locale: "en_US",
    strength: 1
};

// Find on meta field isn't different from a find on any other view, but let's check it anyway.
(function testFind_MetaField() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: numericOrdering}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(coll.insert({time: ISODate(), meta: "1", value: 42}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "10", value: 42}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "5", value: 42}));

    // Use the collection's collation with numeric ordering.
    let res1 = coll.find({meta: {$gt: "4"}});
    assert.eq(2, res1.itcount(), res1.toArray());  // should match "5" and "10"

    // Use explicit collation with lexicographic ordering.
    let res2 = coll.find({meta: {$gt: "4"}}).collation(insensitive);
    assert.eq(1, res2.itcount(), res2.toArray());  // should match only "5"
}());

// For the measurement fields each bucket computes additional "control values", such as min/max and
// might use them to avoid unpacking.
(function testFind_MeasurementField() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: numericOrdering}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // The 'numericOrdering' on the collection means that the max of the bucket with the three docs
    // below is "10" (while the lexicographic max is "5").
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "1"}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "10"}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "5"}));

    // A query with default collation would use the bucket's min/max and find the matches. We are
    // not checking the unpacking optimizations here as it's not a concern of collation per se.
    let res1 = coll.find({value: {$gt: "4"}});
    assert.eq(2, res1.itcount(), res1.toArray());  // should match "5" and "10"

    // If a query with 'insensitive' collation, which doesn't do numeric ordering, used the bucket's
    // min/max it would miss the bucket. Check, that it doesn't.
    let res2 = coll.find({value: {$gt: "4"}}).collation(insensitive);
    assert.eq(1, res2.itcount(), res2.toArray());  // should match only "5"
}());

(function testFind_OnlyQueryHasCollation() {
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    // This should generate a bucket with control.min.value = 'C' and control.max.value = 'c'.
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "C"}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "b"}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, value: "c"}));

    // A query with default collation would use the bucket's min/max and find the two matches.
    const resWithNoCollation = coll.find({value: {$lt: "c"}});
    assert.eq(2,
              resWithNoCollation.itcount(),
              resWithNoCollation.toArray());  // should match "C" and "b".

    // If a query with 'insensitive' collation used the bucket's min/max it would miss the bucket.
    // Check, that it doesn't.
    const resWithCollation_find = coll.find({value: {$lt: "c"}}).collation(insensitive);
    assert.eq(1,
              resWithCollation_find.itcount(),
              resWithCollation_find.toArray());  // should match only "b".

    // Run the same test with aggregate command.
    const resWithCollation_agg =
        coll.aggregate([{$match: {value: {$lt: "c"}}}], {collation: insensitive}).toArray();
    assert.eq(1, resWithCollation_agg.length, resWithCollation_agg);
}());

(function testAgg_GroupByMetaField() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: numericOrdering}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(coll.insert({time: ISODate(), meta: "1", val: 1}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "5", val: 1}));

    // Using collection's collation with numeric ordering.
    let res1 =
        coll.aggregate([{$bucket: {groupBy: "$meta", boundaries: ["1", "10", "50"]}}]).toArray();
    assert.eq(1, res1.length);
    assert.eq({_id: "1", count: 2}, res1[0]);

    // Using explicit collation with lexicographic ordering.
    let res2 = coll.aggregate([{$bucket: {groupBy: "$meta", boundaries: ["1", "10", "50"]}}],
                              {collation: insensitive})
                   .toArray();
    assert.eq(2, res2.length);
    assert.eq({_id: "1", count: 1}, res2[0]);   // "1" goes here
    assert.eq({_id: "10", count: 1}, res2[1]);  // "5" goes here
}());

(function testAgg_GroupByMeasurementField() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: insensitive}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // Cause two different buckets with various case/diacritics in each for the measurement 'name'.
    assert.commandWorked(coll.insert({time: ISODate(), meta: "a", name: 'A'}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "a", name: 'a'}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "a", name: 'á'}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "b", name: 'A'}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "b", name: 'a'}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "b", name: 'ä'}));

    // Test with the collection's collation, which is case and diacritic insensitive.
    assert.eq(1, coll.aggregate([{$sortByCount: "$name"}]).itcount());

    // Test with explicit collation that is different from the collection's.
    assert.eq(2, coll.aggregate([{$sortByCount: "$name"}], {collation: caseSensitive}).itcount());
    assert.eq(3,
              coll.aggregate([{$sortByCount: "$name"}], {collation: diacriticSensitive}).itcount());
}());

// For $group queries that would put whole buckets into the same group, it might be possible to
// avoid unpacking if the information the group is computing is exposed in the control data of each
// bucket. Currently, we only do this optimization for min/max with the meta as the group key.
(function testAgg_MinMaxOptimization() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: numericOrdering}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // These two docs will be placed in the same bucket, and the max for the bucket will be computed
    // using collection's collation, that is, it should be "10".
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, val: "10"}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42, val: "5"}));

    // Let's check our understanding of what happens with the bucketing as otherwise the tests below
    // won't be testing what we think they are.
    let buckets = bucketsColl.find().toArray();
    assert.eq(1, buckets.length, "All docs should be placed into the same bucket");
    assert.eq("10", buckets[0].control.max.val, "Computed max control for 'val' measurement");

    // Use the collection's collation with numeric ordering.
    let res1 = coll.aggregate([{$group: {_id: "$meta", v: {$max: "$val"}}}]).toArray();
    assert.eq("10", res1[0].v, "max val in numeric ordering per the collection's collation");

    // Use the collection's collation with lexicographic ordering.
    let res2 =
        coll.aggregate([{$group: {_id: "$meta", v: {$max: "$val"}}}], {collation: insensitive})
            .toArray();
    assert.eq("5", res2[0].v, "max val in lexicographic ordering per the query collation");
}());

(function testFind_IndexWithDifferentCollation() {
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: 'time', metaField: 'meta'}, collation: diacriticSensitive}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // Create index with a different collation.
    assert.commandWorked(coll.createIndex({meta: 1}, {collation: insensitive}));

    // We only check that the correct plan is chosen so the contents of the collection don't matter
    // as long as it's not empty.
    assert.commandWorked(coll.insert({time: ISODate(), meta: 42}));
    assert.commandWorked(coll.insert({time: ISODate(), meta: "the answer"}));

    // Queries that don't specify explicit collation should use the collection's default collation
    // which isn't compatible with the index, so the index should NOT be used.
    let query = coll.find({meta: "str"}).explain();
    assert(!aggPlanHasStage(query, "IXSCAN"), query);

    // Queries with an explicit collation which isn't compatible with the index, should NOT do
    // index scan.
    query = coll.find({meta: "str"}).collation(caseSensitive).explain();
    assert(!aggPlanHasStage(query, "IXSCAN"), query);

    // Queries with the same collation as in the index, should do index scan.
    query = coll.find({meta: "str"}).collation(insensitive).explain();
    assert(aggPlanHasStage(query, "IXSCAN"), query);

    // Numeric queries that don't rely on collation should do index scan.
    query = coll.find({meta: 1}).explain();
    assert(aggPlanHasStage(query, "IXSCAN"), query);
}());