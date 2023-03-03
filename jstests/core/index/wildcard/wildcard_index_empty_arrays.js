/**
 * Tests that wildcard indexes will correctly match for empty arrays.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/feature_flag_util.js");    // For "FeatureFlagUtil"

const coll = db.wildcard_empty_arrays;
coll.drop();

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

const wildcardIndexes =
    [{keyPattern: {"$**": 1}}, {keyPattern: {"$**": 1, other: 1}, wildcardProjection: {other: 0}}];
// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes =
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompoundWildcardIndexes");

for (const indexSpec of wildcardIndexes) {
    if (!allowCompoundWildcardIndexes && indexSpec.wildcardProjection) {
        continue;
    }
    coll.drop();
    const option = {};
    if (indexSpec.wildcardProjection) {
        option['wildcardProjection'] = indexSpec.wildcardProjection;
    }
    assert.commandWorked(coll.createIndex(indexSpec.keyPattern, option));

    assert.commandWorked(coll.insert({a: 1, b: 1, c: [], d: {e: [5, 6]}}));
    assert.commandWorked(coll.insert({a: 2, b: 2, c: [1, 2], d: {e: []}}));
    assert.commandWorked(coll.insert({a: 1, b: 2, c: [3, 4], d: {e: [7, 8]}, f: [{g: []}]}));
    assert.commandWorked(coll.insert({a: 2, b: [[]], c: 1, d: 4}));

    // $** index matches empty array.
    assertArrayEq(coll.find({c: []}, {_id: 0}).hint(indexSpec.keyPattern).toArray(),
                  [{a: 1, b: 1, c: [], d: {e: [5, 6]}}]);

    // $** index supports equality to array offset.
    assertArrayEq(coll.find({"c.0": 1}, {_id: 0}).hint(indexSpec.keyPattern).toArray(),
                  [{a: 2, b: 2, c: [1, 2], d: {e: []}}]);

    // $** index matches empty array nested in object.
    assertArrayEq(coll.find({"d.e": []}, {_id: 0}).hint(indexSpec.keyPattern).toArray(),
                  [{a: 2, b: 2, c: [1, 2], d: {e: []}}]);

    // $** index matches empty array nested within an array of objects.
    assertArrayEq(coll.find({"f.0.g": []}, {_id: 0}).hint(indexSpec.keyPattern).toArray(),
                  [{a: 1, b: 2, c: [3, 4], d: {e: [7, 8]}, f: [{g: []}]}]);

    // $** index matches empty array nested within an array.
    assertArrayEq(coll.find({"b": []}, {_id: 0}).hint(indexSpec.keyPattern).toArray(),
                  [{a: 2, b: [[]], c: 1, d: 4}]);
}
})();
