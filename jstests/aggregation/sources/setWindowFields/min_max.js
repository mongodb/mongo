/**
 * Test that $min/max works as a window function.
 */
import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $min function.
testAccumAgainstGroup(coll, "$min");

// Run the suite of partition and bounds tests against the $max function.
testAccumAgainstGroup(coll, "$max");

// Test the behavior of min/max over a non-numeric field. Note that $min and $max order values by
// type per the BSON spec, so there will always be a value to return.
let documents = [
    {_id: 1, "str": "Inlet"},
    {_id: 2, "str": "compressing Supervisor Synchronised"},
    {_id: 3, "str": "fuchsia"},
    {_id: 4, "str": "transmit Ohio AI"},
    {_id: 5, "str": "Louisiana system-worthy Borders"},
    {_id: 6, "str": "Security Mouse"},
];

let expectedResults = [
    {maxStr: "Security Mouse", minStr: "Security Mouse"},
    {maxStr: "Security Mouse", minStr: "Louisiana system-worthy Borders"},
    {maxStr: "transmit Ohio AI", minStr: "Louisiana system-worthy Borders"},
    {maxStr: "transmit Ohio AI", minStr: "Louisiana system-worthy Borders"},
    {maxStr: "transmit Ohio AI", minStr: "compressing Supervisor Synchronised"},
    {maxStr: "fuchsia", minStr: "Inlet"},
];

coll.drop();
for (let i = 0; i < documents.length; i++) {
    assert.commandWorked(coll.insert(documents[i]));
}

let results = coll.aggregate([{
                      $setWindowFields: {
                          sortBy: {_id: -1},
                          output: {
                              "maxStr": {$max: "$str", window: {documents: [-2, 0]}},
                              "minStr": {$min: "$str", window: {documents: [-2, 0]}}
                          }
                      }
                  }])
                  .toArray();

assert.eq(expectedResults.length, results.length);
for (let index = 0; index < results.length; index++) {
    assert.eq(expectedResults[index].maxStr, results[index].maxStr);
    assert.eq(expectedResults[index].minStr, results[index].minStr);
}

// The test below is to ensure that memory estimation does not return negative values (SERVER-89408
// ).
documents = [
    {_id: 0, "num": 10, "str": "ABCDEFGHIJK"},
    {_id: 1, "num": 3, "str": "ABCDE"},
    {_id: 2, "num": 5, "str": "AB"},
];

expectedResults = [{minStr: "ABCDEFGHIJK"}, {minStr: "AB"}, {minStr: "AB"}];

coll.drop();
for (let i = 0; i < documents.length; i++) {
    assert.commandWorked(coll.insert(documents[i]));
}

results =
    coll.aggregate([{
            $setWindowFields: {
                sortBy: {"num": -1},
                output: {"minStr": {$min: "$str", window: {documents: ["unbounded", "current"]}}}
            }
        }])
        .toArray();

assert.eq(expectedResults.length, results.length);
for (let index = 0; index < results.length; index++) {
    assert.eq(expectedResults[index].minStr, results[index].minStr);
}
