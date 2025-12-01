/**
 * Tests an extension source stage.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

coll.insert({breadType: "sourdough"});

// Source stage must still be run against a collection.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: 1,
        pipeline: [{$toast: {temp: 350.0, numSlices: 2}}],
        cursor: {},
    }),
    ErrorCodes.InvalidNamespace,
);

// Source stage must be first in the pipeline.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$match: {breadType: "brioche"}}, {$toast: {temp: 350.0, numSlices: 2}}],
        cursor: {},
    }),
    40602,
);

// Can't have two source stages at the same level in a pipeline.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$toast: {temp: 350.0, numSlices: 2}}, {$toast: {temp: 350.0, numSlices: 2}}],
        cursor: {},
    }),
    40602,
);

// EOF source stage.
let results = coll.aggregate([{$toast: {temp: 350.0, numSlices: 0}}]).toArray();
assert.eq(results.length, 0, results);

// Top-level source stage.
if (!FixtureHelpers.isMongos(db)) {
    results = coll.aggregate([{$toast: {temp: 425.0, numSlices: 4}}]).toArray();
    assert.eq(results, [
        {slice: 0, isBurnt: true},
        {slice: 1, isBurnt: true},
        {slice: 2, isBurnt: true},
        {slice: 3, isBurnt: true},
    ]);
} else {
    // This source stage will run on every shard, producing variable numbers of
    // documents depending on how many shards exist in the cluster. Relax the expected
    // results assertion for that case.
    // TODO SERVER-114234 Remove these relaxed assertions once we can run this properly as a collectionless aggregate.
    results = coll.aggregate([{$toast: {temp: 425.0, numSlices: 4}}]).toArray();
    assert.gt(results.length, 0, results);
}

// TODO SERVER-113930 Remove failure cases and enable success cases for $lookup and $unionWith.
// Source stage in $lookup.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$lookup: {from: collName, pipeline: [{$toast: {temp: 350.0, numSlices: 2}}], as: "slices"}}],
        cursor: {},
    }),
    51047,
);
// results = coll.aggregate([{$lookup: {from: collName, pipeline: [{$toast: {temp: 350.0, numSlices: 2}}], as: "slices"}}]).toArray();
// assert.eq(results, [{breadType: "sourdough", slices: [{slice: 0, isBurnt: false}, {slice: 1, isBurnt: false}]}]);

// Source stage in $unionWith.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$unionWith: {coll: collName, pipeline: [{$toast: {temp: 350.0, numSlices: 2}}]}}],
        cursor: {},
    }),
    31441,
);
// results = coll.aggregate([{$unionWith: {coll: collName, pipeline: [{$toast: {temp: 350.0, numSlices: 2}}]}}]).toArray();
// assert.eq(results, [{breadType: "sourdough"}, {slice: 0, isBurnt: false}, {slice: 1, isBurnt: false}]);

// Source stage is not allowed in $facet.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: collName,
        pipeline: [{$facet: {slices: [{$toast: {temp: 250.0, numSlices: 2}}]}}],
        cursor: {},
    }),
    40600,
);

// TODO SERVER-113930 Enable this test.
// Two source stages in the pipeline.
// results = coll.aggregate([{$toast: {temp: 100.0, numSlices: 1}}, {$unionWith: {coll: collName, pipeline: [{$toast: {temp: 350.0, numSlices: 2}}]}}]).toArray();
// assert.eq(results, [{slice: 0, notToasted: true}, {slice: 0, isBurnt: false}, {slice: 1, isBurnt: false}]);
