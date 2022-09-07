/**
 * Test the input/output behavior of some predicates following projections on time-series
 * collections.
 *
 * @tags: [
 *   # deleteMany({}) is not retryable
 *   requires_non_retryable_writes,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const coll = db.timeseries_predicates_with_projections_normal;
const tsColl = db.timeseries_predicates_with_projections_timeseries;
coll.drop();
tsColl.drop();
assert.commandWorked(
    db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time', metaField: 'mm'}}));
const bucketsColl = db.getCollection('system.buckets.' + tsColl.getName());

// Test that 'predicate' behaves correctly following 'projection' on the example documents,
// by comparing the result on a time-series collection against a normal collection.
function checkPredicateResult(projection, predicate, documents) {
    for (const doc of documents) {
        doc._id = ObjectId();
        doc.time = ISODate();
    }

    assert.commandWorked(coll.deleteMany({}));
    bucketsColl.deleteMany({});
    assert.commandWorked(coll.insert(documents));
    assert.commandWorked(tsColl.insert(documents));

    const normalResult = coll.aggregate([{$project: projection}, {$match: predicate}]).toArray();
    const tsResult = tsColl.aggregate([{$project: projection}, {$match: predicate}]).toArray();
    jsTest.log(normalResult, tsResult);
    assert.sameMembers(normalResult, tsResult);
}

// predicate on preserved measurement field
checkPredicateResult({x: 1}, {x: {$lt: 0}}, [
    {x: -1},
    {x: 1},
]);

// predicate on preserved meta field
checkPredicateResult({"mm.x": 1}, {"mm.x": {$lt: 0}}, [
    {mm: {x: -1}},
    {mm: {x: 1}},
]);

// predicate on discarded measurement field
checkPredicateResult({y: 1}, {x: {$lt: 0}}, [
    {x: -1},
    {x: 1},
]);

// predicate on discarded meta field
checkPredicateResult({x: 1}, {"mm.x": {$lt: 0}}, [
    {mm: {x: -1}},
    {mm: {x: 1}},
]);
})();
