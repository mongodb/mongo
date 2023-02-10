/**
 * Test the behavior of $group on time-series collections.
 *
 * @tags: [
 *   directly_against_shardsvrs_incompatible,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_61,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/core/timeseries/libs/timeseries.js");

const coll = db.timeseries_groupby_reorder;
coll.drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {metaField: "meta", timeField: "t"}}));

const t = new Date();
assert.commandWorked(coll.insert({_id: 0, t: t, b: 1, c: 1}));
assert.commandWorked(coll.insert({_id: 0, t: t, b: 2, c: 2}));
assert.commandWorked(coll.insert({_id: 0, t: t, b: 3, c: 3}));

// Test reordering the groupby and internal unpack buckets.
if (!isMongos(db)) {
    const res = coll.explain("queryPlanner").aggregate([
        {$group: {_id: '$meta', accmin: {$min: '$b'}, accmax: {$max: '$c'}}}
    ]);

    assert.docEq(res.stages[1], {
        "$group":
            {_id: "$meta", accmin: {"$min": "$control.min.b"}, accmax: {"$max": "$control.max.c"}}
    });
}

let res = coll.aggregate([{$group: {_id: '$meta', accmin: {$min: '$b'}, accmax: {$max: '$c'}}}])
              .toArray();
assert.docEq([{"_id": null, "accmin": 1, "accmax": 3}], res);

// Test SERVER-73822 fix: complex $min and $max (i.e. not just straight field refs) work correctly.
res = coll.aggregate([{
              $group: {
                  _id: '$meta',
                  accmin: {$min: {$add: ["$b", "$c"]}},
                  accmax: {$max: {$add: ["$b", "$c"]}}
              }
          }])
          .toArray();
assert.docEq([{"_id": null, "accmin": 2, "accmax": 6}], res);
})();
