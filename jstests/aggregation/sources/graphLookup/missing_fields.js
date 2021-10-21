// Test that missing values are not returned when the connectFrom value in a $graphLookup is null.
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");      // For isSharded.
load("jstests/aggregation/extras/utils.js");  // arrayEq

var local = db[jsTestName()];

// Do not run the the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(local) && !isShardedLookupEnabled) {
    return;
}

local.drop();

assert.commandWorked(local.insert([
    {"_id": 0, "x": 1, "y": 1},
    {"_id": 1, "x": 1, "y": null},
    {"_id": 2, "x": 1, "y": 1},
    {"_id": 3, "x": 1}
]));

let result = local.aggregate([
    {$graphLookup: {
        from: local.getName(),
        startWith: "$y",
        connectFromField: "x",
        connectToField: "y",
        as: "arr",
        maxDepth: 0}}
    ]).toArray();
let expected = [
    {"_id": 0, "x": 1, "y": 1, "arr": [{"_id": 2, "x": 1, "y": 1}, {"_id": 0, "x": 1, "y": 1}]},
    {"_id": 1, "x": 1, "y": null, "arr": [{"_id": 1, "x": 1, "y": null}]},
    {"_id": 2, "x": 1, "y": 1, "arr": [{"_id": 2, "x": 1, "y": 1}, {"_id": 0, "x": 1, "y": 1}]},
    {"_id": 3, "x": 1, "arr": []}
];
assert(arrayEq(expected, result), "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(result));
assert.commandWorked(local.insert({"_id": 4, "x": 1, "y": undefined}));

result = local.aggregate([
    {$graphLookup: {
        from: local.getName(),
        startWith: null,
        connectFromField: "x",
        connectToField: "y",
        as: "arr",
        maxDepth: 0}}
    ]).toArray();

expected = [
    {"_id": 0, "x": 1, "y": 1, "arr": [{"_id": 1, "x": 1, "y": null}]},
    {"_id": 1, "x": 1, "y": null, "arr": [{"_id": 1, "x": 1, "y": null}]},
    {"_id": 2, "x": 1, "y": 1, "arr": [{"_id": 1, "x": 1, "y": null}]},
    {"_id": 3, "x": 1, "arr": [{"_id": 1, "x": 1, "y": null}]},
    {"_id": 4, "x": 1, "y": undefined, "arr": [{"_id": 1, "x": 1, "y": null}]}
];
assert(arrayEq(expected, result), "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(result));

local.drop();

// Test missing doesn't match missing.
assert.commandWorked(
    local.insert([{"_id": 0, "x": 1, "y": 1}, {"_id": 1, "x": 2}, {"_id": 3, "x": 1}]));
result = local.aggregate([
    {$graphLookup: {
        from: local.getName(),
        startWith: "$y",
        connectFromField: "x",
        connectToField: "y",
        as: "arr",
        maxDepth: 0}}
    ]).toArray();
expected = [
    {"_id": 0, "x": 1, "y": 1, "arr": [{"_id": 0, "x": 1, "y": 1}]},
    {"_id": 1, "x": 2, "arr": []},
    {"_id": 3, "x": 1, "arr": []}
];
assert(arrayEq(expected, result));
local.drop();
assert.commandWorked(local.insert([{"_id": 0, "x": 1}, {"_id": 1, "x": 2}, {"_id": 3, "x": 1}]));
result = local.aggregate([
    {$graphLookup: {
        from: local.getName(),
        startWith: "$y",
        connectFromField: "x",
        connectToField: "y",
        as: "arr",
        maxDepth: 0}}
    ]).toArray();
expected =
    [{"_id": 0, "x": 1, "arr": []}, {"_id": 1, "x": 2, "arr": []}, {"_id": 3, "x": 1, "arr": []}];
assert(arrayEq(expected, result), "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(result));
}());
