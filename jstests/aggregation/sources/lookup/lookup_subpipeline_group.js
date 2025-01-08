// Exercises $lookup queries with include a $group in their sub-pipeline, with a focus on
// distinct-like queries.
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const baseCollName = jsTestName() + "_base";
const baseColl = db[baseCollName];
baseColl.drop();

const fromCollName = jsTestName() + "_from";
const fromColl = db[fromCollName];
fromColl.drop();

assert.commandWorked(baseColl.insertMany([
    {_id: 0, key: "foo"},
    {_id: 1, key: "bar"},
]));

assert.commandWorked(fromColl.insertMany([
    {_id: 2, key: "foo", num: 0},
    {_id: 3, key: "foo", num: 5},
    {_id: 4, key: "bar", num: 10},
    {_id: 5, key: "baz", num: 5},
    {_id: 6, key: "baz", num: 5},
]));

// Create a couple of indexes that could be used in the "from" collection for a distinct scan, and
// will be multiplanned if shard-filtering distinct is enabled.
assert.commandWorked(fromColl.createIndex({key: 1}));
assert.commandWorked(fromColl.createIndex({key: -1, num: 1}));
assert.commandWorked(fromColl.createIndex({key: 1, num: -1}));
assert.commandWorked(fromColl.createIndex({key: 1, num: -1, other: 1}));

function assertLookupResults({pipeline, expected}) {
    const actual = assert
                       .commandWorked(db.runCommand(
                           {aggregate: baseCollName, pipeline, cursor: {batchSize: 100}}))
                       .cursor.firstBatch;
    assertArrayEq({expected, actual});
}

// Note: we intentionally omit a {$group: {_id: "$key"}} test case here, since when the shard
// filtering distinct scan flag is off, this will return orphans on passthrough suites.

//
// Test a $group with $avg- not distinct scan eligible.
//
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        pipeline: [{$group: {_id: "$key", avg: {$avg: "$num"}}}]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", avg: 2.5},
        {_id: "bar", avg: 10},
        {_id: "baz", avg: 5},
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "foo", avg: 2.5},
        {_id: "bar", avg: 10},
        {_id: "baz", avg: 5},
    ]},
]});

// Now with join field.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [{$group: {_id: "$key", avg: {$avg: "$num"}}}]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", avg: 2.5}
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "bar", avg: 10},
    ]},
]});

// Now with unwind.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [{$group: {_id: "$key", avg: {$avg: "$num"}}}]
    }},
    {$unwind: {path: "$" + fromCollName}}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: 
        {_id: "foo", avg: 2.5}
    },
    {_id: 1, key: "bar", [fromCollName]: 
        {_id: "bar", avg: 10},
    },
]});

//
// Test a $group with $top/$bottom, which is distinct-scan eligible.
//
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        pipeline: [{$group: {_id: "$key", top: {$top: {output: "$num", sortBy: {key: -1, num: 1}}}}}]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", top: 0},
        {_id: "bar", top: 10},
        {_id: "baz", top: 5},
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "foo", top: 0},
        {_id: "bar", top: 10},
        {_id: "baz", top: 5},
    ]},
]});

// Now with join field.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [{$group: {_id: "$key", top: {$top: {output: "$num", sortBy: {key: 1, num: -1}}}}}]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", top: 5}
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "bar", top: 10},
    ]},
]});

// Now with unwind.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [{$group: {_id: "$key", bottom: {$bottom: {output: "$num", sortBy: {key: 1, num: -1}}}}}]
    }},
    {$unwind: {path: "$" + fromCollName}}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: 
        {_id: "foo", bottom: 0}
    },
    {_id: 1, key: "bar", [fromCollName]: 
        {_id: "bar", bottom: 10},
    },
]});

//
// Test a $group with $first/$last, which is distinct-scan eligible.
//
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        pipeline: [
            {$sort: {key: -1, num: 1}},
            {$group: {_id: "$key", first: {$first: "$num"}}}
        ]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", first: 0},
        {_id: "bar", first: 10},
        {_id: "baz", first: 5},
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "foo", first: 0},
        {_id: "bar", first: 10},
        {_id: "baz", first: 5},
    ]},
]});

// Now with join field.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [
            {$sort: {key: -1, num: 1}},
            {$group: {_id: "$key", last: {$last: "$num"}}}
        ]
    }}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: [
        {_id: "foo", last: 5}
    ]},
    {_id: 1, key: "bar", [fromCollName]: [
        {_id: "bar", last: 10},
    ]},
]});

// Now with unwind.
assertLookupResults({pipeline: [
    {$lookup: {
        from: fromCollName,
        as: fromCollName,
        localField: "key",
        foreignField: "key",
        pipeline: [
            {$sort: {key: 1, num: -1}},
            {$group: {_id: "$key", last: {$last: "$num"}}},
        ]
    }},
    {$unwind: {path: "$" + fromCollName}}
], expected: [
    {_id: 0, key: "foo", [fromCollName]: 
        {_id: "foo", last: 0}
    },
    {_id: 1, key: "bar", [fromCollName]: 
        {_id: "bar", last: 10},
    },
]});
