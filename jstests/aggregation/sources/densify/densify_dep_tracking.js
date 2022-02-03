/**
 * Test that $densify communicates the fields it needs succesfully for optimizations.
 * @tags: [
 *   requires_fcv_53,
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

let documentList = [
    {
        _id: 1,
        "date": new Date("2019-12-03T02:14:31.296Z"),
        second: new Date("2020-01-10T11:20:05.778Z"),
    },
    {
        _id: 2,
        "date": new Date("2019-08-29T01:15:43.084Z"),
        second: new Date("2020-01-10T00:20:05.778Z"),
    },
];

coll.insert(documentList);

let pipeline = [
    {$sort: {"date": 1, "second": 1, "obj.obj.obj.obj.str": 1, "obj.obj.obj.num": -1, _id: 1}},
    {$densify: {field: "second", range: {step: 1, unit: "hour", bounds: "full"}}},
    {$sort: {_id: 1}},
    {$sortByCount: {$firstN: {'n': NumberInt(7), 'input': []}}}
];
let result = coll.aggregate(pipeline).toArray();
// Add ten, have two originally.
assert.eq(result.length, 1);
assert.eq(result[0].count, 12);

// Repeat the test, but with partitions.
coll.drop();
documentList = [
    {
        _id: 1,
        "date": new Date("2019-12-03T02:14:31.296Z"),
        second: new Date("2020-01-10T11:20:05.778Z"),
        part: 1
    },
    {
        _id: 2,
        "date": new Date("2019-08-29T01:15:43.084Z"),
        second: new Date("2020-01-10T00:20:05.778Z"),
        part: 1
    },
    {
        _id: 3,
        "date": new Date("2019-12-03T02:14:31.296Z"),
        second: new Date("2020-01-10T11:20:05.778Z"),
        part: 2
    },
    {
        _id: 4,
        "date": new Date("2019-08-29T01:15:43.084Z"),
        second: new Date("2020-01-10T00:20:05.778Z"),
        part: 2
    },
];

coll.insert(documentList);

pipeline = [
    {$sort: {"date": 1, "second": 1, "obj.obj.obj.obj.str": 1, "obj.obj.obj.num": -1, _id: 1}},
    {
        $densify: {
            field: "second",
            range: {step: 1, unit: "hour", bounds: "partition"},
            partitionByFields: ["part"]
        }
    },
    {$sort: {_id: 1}},
    {$sortByCount: {$firstN: {'n': NumberInt(7), 'input': []}}}
];

result = coll.aggregate(pipeline).toArray();
// Add ten, have two originally for each partition.
assert.eq(result.length, 1, result);
assert.eq(result[0].count, 24, result[0]);
})();
