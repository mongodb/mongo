/**
 * Test $_internalShredDocuments parsing and make sure it doesn't modify documents.
 */
"use strict";

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.insertMany(
    [{a: 1, obj: {a: 1}, arr: [{a: 1}]}, {a: 1, obj: {a: 1}, arr: [{a: 1}]}, {}, {a: 1}]);
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
