/**
 * Test partial index optimization on a time-series collection.
 * If a query expression is covered by the partial index filter, it is removed from the filter in
 * the fetch stage.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues a command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   cqf_incompatible,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
const coll = db.timeseries_partial_index_opt;

coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "time", metaField: "tag"}}));

assert.commandWorked(coll.insertMany([
    {_id: 0, time: new Date("2021-07-29T07:46:38.746Z"), tag: 2, a: 5},
    {_id: 1, time: new Date("2021-08-29T00:15:38.001Z"), tag: 1, a: 5, b: 8},
    {_id: 2, time: new Date("2021-11-29T12:20:34.821Z"), tag: 1, a: 7, b: 12},
    {_id: 3, time: new Date("2021-03-09T07:29:34.201Z"), tag: 2, a: 2, b: 7},
    {_id: 4, time: new Date("2021-10-09T07:29:34.201Z"), tag: 4, a: 8, b: 10}
]));

// Check that the plan uses partial index scan with 'indexName' and the filter of the fetch
// stage does not contain the field in the partial filter expression.
function checkIndexScanAndFilter(coll, predicate, indexName, filterField) {
    const explain = coll.find(predicate).explain();
    const scan = getAggPlanStage(explain, "IXSCAN");
    assert.eq(scan.indexName, indexName, scan);

    const fetch = getAggPlanStage(explain, "FETCH");
    if (fetch !== null && fetch.hasOwnProperty("filter")) {
        const filter = fetch.filter;
        assert(!filter.hasOwnProperty(filterField),
               "Unexpected field " + filterField + " in fetch filter: " + tojson(filter));
    }
}

const timeDate = ISODate("2021-10-01 00:00:00.000Z");
assert.commandWorked(
    coll.createIndex({time: 1}, {name: "time_1_tag", partialFilterExpression: {tag: {$gt: 1}}}));
checkIndexScanAndFilter(coll, {time: {$gte: timeDate}, tag: {$gt: 1}}, "time_1_tag", "tag");

assert.commandWorked(
    coll.createIndex({tag: 1}, {name: "tag_1_b", partialFilterExpression: {b: {$gte: 10}}}));
checkIndexScanAndFilter(coll, {tag: {$gt: 1}, b: {$gte: 10}}, "tag_1_b", "b");
})();
