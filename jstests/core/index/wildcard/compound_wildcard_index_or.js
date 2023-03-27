/**
 * Tests that compound wildcard indexes with queries on non-wildcard prefix are not scanned in OR
 * queries.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_concern_local,
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");     // For arrayEq().
load("jstests/libs/wildcard_index_helpers.js");  // For WildcardIndexHelpers.

const documentList = [
    {
        _id: 428,
        "str": "Chicken RAM Nepal",
    },
    {
        _id: 1311,
        "str": "navigate Stravenue",
        "obj": {
            _id: 1313,
            "obj": {
                _id: 1314,
                "obj": {
                    _id: 1315,
                    "obj": {
                        _id: 1316,
                        "obj": {},
                    },
                },
            },
        },
    },
];

const pipeline =
    [{$match: {$or: [{"str": {$regex: /^Chicken/}}, {"obj.obj.obj.obj.obj": {$exists: true}}]}}];

const indexList = [{"obj.obj.obj.$**": 1}, {"str": -1, "obj.obj.obj.obj.$**": -1}];

const coll = db[jsTestName() + "_wild"];
const wild = db[jsTestName() + "_no_idx"];

coll.drop();
wild.drop();

assert.commandWorked(coll.insertMany(documentList));
assert.commandWorked(wild.insertMany(documentList));

assert.commandWorked(wild.createIndexes(indexList));

const noIdxResult = coll.aggregate(pipeline).toArray();
const idxResult = wild.aggregate(pipeline).toArray();

assertArrayEq({expected: documentList, actual: noIdxResult});
assertArrayEq({expected: noIdxResult, actual: idxResult});

// We want to make sure we do not use an IXSCAN to answer the ineligible prefix.
const explain = assert.commandWorked(wild.explain('executionStats').aggregate(pipeline));
WildcardIndexHelpers.assertExpectedIndexIsNotUsed(explain, "str_-1_obj.obj.obj.obj.$**_-1");
})();
