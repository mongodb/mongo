/**
 * Basic tests for the $first/$last accumulators.
 */
import "jstests/libs/sbe_assert_error_override.js";

import {arrayEq, orderedArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

// Basic correctness tests.
let docs = [];
let docId = 0;
const kMaxSales = 20;
let expectedFirstSales = [];
let expectedLastSales = [];
for (const states
         of [{state: 'AZ', sales: 3}, {state: 'CA', sales: 2}, {state: 'NY', sales: kMaxSales}]) {
    let firstSales;
    let lastSales;
    const state = states['state'];
    const sales = states['sales'];
    for (let i = 0; i < kMaxSales && i < sales; ++i) {
        const salesAmt = i * 10;
        docs.push({_id: docId++, state: state, sales: salesAmt, stateObj: {"st": state}, n: 3});
        if (i == 0) {
            firstSales = salesAmt;
        }
        if (i == sales - 1) {
            lastSales = salesAmt;
        }
    }
    expectedFirstSales.push({_id: state, sales: firstSales});
    expectedLastSales.push({_id: state, sales: lastSales});
}

assert.commandWorked(coll.insert(docs));

{  // Test $first when grouping by a string.
    const actualResults = coll.aggregate([
                                  {$sort: {_id: 1}},
                                  {$group: {_id: '$state', sales: {$first: "$sales"}}},
                              ])
                              .toArray();

    assert(arrayEq(expectedFirstSales, actualResults),
           () => "expected " + tojson(expectedFirstSales) + " actual " + tojson(actualResults));
}

{  // Test $first when grouping by an object.
    const actualResults = coll.aggregate([
                                  {$sort: {_id: 1}},
                                  {$group: {_id: "$stateObj", sales: {$first: "$sales"}}},
                              ])
                              .toArray();

    const expectedResult =
        expectedFirstSales.map(doc => ({'_id': {'st': doc['_id']}, sales: doc['sales']}));
    assert(arrayEq(expectedResult, actualResults),
           () => "expected " + tojson(expectedResult) + " actual " + tojson(actualResults));
}

{  // Test $last when grouping by a string.
    const actualResults = coll.aggregate([
                                  {$sort: {_id: 1}},
                                  {$group: {_id: '$state', sales: {$last: "$sales"}}},
                              ])
                              .toArray();
    assert(arrayEq(expectedLastSales, actualResults),
           () => "expected " + tojson(expectedLastSales) + " actual " + tojson(actualResults));
}

{  // Test $last when grouping by an object.
    const actualResults = coll.aggregate([
                                  {$sort: {_id: 1}},
                                  {$group: {_id: "$stateObj", sales: {$last: "$sales"}}},
                              ])
                              .toArray();

    const expectedResult =
        expectedLastSales.map(doc => ({'_id': {'st': doc['_id']}, sales: doc['sales']}));
    assert(arrayEq(expectedResult, actualResults),
           () => "expected " + tojson(expectedResult) + " actual " + tojson(actualResults));
}

// Basic correctness test for $first/$last used in $bucketAuto. Though $bucketAuto uses
// accumulators in the same way that $group does, the test below verifies that everything
// works properly with serialization and reporting results. Note that the $project allows us
// to compare the $bucketAuto results to the expected $group results (because there are more
// buckets than groups, it will always be the case that the min value of each bucket
// corresponds to the group key).

{  // Test $first in $bucketAuto.
    let actualResults =
        coll.aggregate([
                {$sort: {sales: 1}},
                {$bucketAuto: {groupBy: '$state', buckets: 1, output: {sales: {$first: "$sales"}}}},
                {$project: {_id: "$_id.min", sales: 1}}
            ])
            .toArray();
    const expectedResults = [{_id: 'AZ', sales: 0}];
    assert(orderedArrayEq(expectedResults, actualResults),
           () => "expected " + tojson(expectedResults) + " actual " + tojson(actualResults));
}

{  // Test $last in $bucketAuto.
    let actualResults =
        coll.aggregate([
                {$sort: {sales: 1}},
                {$bucketAuto: {groupBy: '$state', buckets: 1, output: {sales: {$last: "$sales"}}}},
                {$project: {_id: "$_id.min", sales: 1}}
            ])
            .toArray();
    const expectedResults = [{_id: 'AZ', sales: 190}];
    assert(orderedArrayEq(expectedResults, actualResults),
           () => "expected " + tojson(expectedResults) + " actual " + tojson(actualResults));
}

// Verify that an index on {_id: 1, sales: -1} will produce the expected results.
const idxSpec = {
    _id: 1,
    sales: -1
};
assert.commandWorked(coll.createIndex(idxSpec));

{  // Test $first with index.
    const actualResults = coll.aggregate(
                                  [
                                      {$sort: {_id: 1}},
                                      {$group: {_id: '$state', sales: {$first: "$sales"}}},
                                      {$sort: {_id: 1}},
                                  ],
                                  {hint: idxSpec})
                              .toArray();
    assert.eq(expectedFirstSales, actualResults);
}

{  // Test $last with index.
    const actualResults = coll.aggregate(
                                  [
                                      {$sort: {_id: 1}},
                                      {$group: {_id: '$state', sales: {$last: "$sales"}}},
                                      {$sort: {_id: 1}},
                                  ],
                                  {hint: idxSpec})
                              .toArray();
    assert.eq(expectedLastSales, actualResults);
}
