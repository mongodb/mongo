/**
 * Test how $densify desugars.
 *
 * @tags: [
 *   # $mergeCursors was added to explain output in 5.3.
 *   requires_fcv_53,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For anyEq and
                                              // desugarSingleStageAggregation.

const coll = db[jsTestName()];
coll.insert({});

// Implicit partition fields and sort are generated.
assert.eq(desugarSingleStageAggregation(
              db, coll, {$densify: {field: "a", range: {step: 1.0, bounds: "full"}}}),
          [
              {$sort: {sortKey: {a: 1}}},
              {
                  $_internalDensify:
                      {field: "a", partitionByFields: [], range: {step: 1.0, bounds: "full"}}
              },
          ]);

// PartitionByFields are prepended to the sortKey if "partition" is specified.
assert.eq(
    desugarSingleStageAggregation(db, coll, {
        $densify:
            {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "partition"}}
    }),
    [
        {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "partition"}}
        },
    ]);

// PartitionByFields are not prepended to the sortKey if "full" is specified.
assert.eq(
    desugarSingleStageAggregation(db, coll, {
        $densify: {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "full"}}
    }),
    [
        {$sort: {sortKey: {a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "full"}}
        },
    ]);

// PartitionByFields are prepended to the sortKey if numeric bounds are specified.
assert.eq(
    desugarSingleStageAggregation(db, coll, {
        $densify: {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: [-10, 0]}}
    }),
    [
        {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: [-10, 0]}}
        },
    ]);

// PartitionByFields are prepended to the sortKey if date bounds are specified.
assert.eq(desugarSingleStageAggregation(db, coll, {
              $densify: {
                  field: "a",
                  partitionByFields: ["b", "c"],
                  range: {
                      step: 1.0,
                      bounds: [new ISODate("2020-01-03"), new ISODate("2020-01-04")],
                      unit: "day"
                  }
              }
          }),
          [
              {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
              {
                  $_internalDensify: {
                      field: "a",
                      partitionByFields: ["b", "c"],
                      range: {
                          step: 1.0,
                          bounds: [new ISODate("2020-01-03"), new ISODate("2020-01-04")],
                          unit: "day"
                      }
                  }
              },
          ]);
})();
