/**
 * Test $_internalShredDocuments parsing and make sure it doesn't modify documents.
 * this test assumes {$meta: "indexKey"} will not be missing.
 * @tags: [
 *     do_not_wrap_aggregations_in_facets
 * ]
 */
"use strict";

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.insertMany(
    [{a: 1, obj: {a: 1}, arr: [{a: 1}]}, {a: 2, obj: {a: 1}, arr: [{a: 1}]}, {}, {a: 3}]);
coll.createIndex({a: 1});
assert.commandFailedWithCode(assert.throws(() => coll.aggregate({$_internalShredDocuments: 1})),
                                          7997500);
assert.commandFailedWithCode(
    assert.throws(() => coll.aggregate(
                      {$_internalShredDocuments: {burnEvidence: true, paperType: "legal"}})),
                 7997501);

const assertNoop = (pipeline, position) => {
    const expected = coll.aggregate(pipeline).toArray();
    pipeline.splice(position, 0, {$_internalShredDocuments: {}});
    const actual = coll.aggregate(pipeline).toArray();
    assertArrayEq({expected, actual});
};

assertNoop([], 0);
assertNoop([{$project: {a: 1}}], 1);
assertNoop([{$addFields: {b: 2, "obj.b": 2, "arr.0.b": 2}}], 1);

const addFieldGroup = [
    {$addFields: {b: 2, "obj.b": 2, "arr.0.b": 2}},
    {$group: {_id: {a: "$a", b: "$b", obj: "$obj"}}}
];
assertNoop(addFieldGroup, 0);
assertNoop(addFieldGroup, 1);
assertNoop(addFieldGroup, 2);

const matchExcludeGroup =
    [{$match: {a: 1}}, {$project: {obj: 0}}, {$group: {_id: {a: "$a", b: "$b", obj: "$obj"}}}];
assertNoop(matchExcludeGroup, 0);
assertNoop(matchExcludeGroup, 1);
assertNoop(matchExcludeGroup, 2);
assertNoop(matchExcludeGroup, 3);

assertNoop([{$match: {a: 1}}, {$addFields: {key: {$meta: "indexKey"}}}], 1);

// The shred() function is also used by set windowFields so lets test that too.
const res = coll.aggregate([
                    {$match: {a: 1}},
                    {$addFields: {key: {$meta: "indexKey"}}},
                    {$setWindowFields: {sortBy: {a: 1}, output: {w: {$rank: {}}}}},
                    {$limit: 1}
                ])
                .toArray();
assert.eq(1, res.length);
assert(res[0].hasOwnProperty("key"));
assert.eq({a: 1}, res[0]["key"]);
