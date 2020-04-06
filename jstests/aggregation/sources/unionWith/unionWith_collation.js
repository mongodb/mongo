/**
 * Tests that $unionWith respects the user-specified collation or the inherited local collation
 * when performing comparisons on a foreign collection/view with a different default collation.
 *
 * We are explicitly creating collections with collation as part of this test. So we will receive
 * 'NamespaceExists' error if we allow implicit creation.
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {

"use strict";

const testDB = db.getSiblingDB(jsTestName());
const noCollationColl = testDB.no_collation;
const caseInsensitiveColl = testDB.case_insensitive;

caseInsensitiveColl.drop();
noCollationColl.drop();

const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1
};
const simpleCollation = {
    locale: "simple"
};

// Create a case-sensitive/simple collection and a case-insensitive collection.
assert.commandWorked(testDB.createCollection(noCollationColl.getName()));
assert.commandWorked(
    testDB.createCollection(caseInsensitiveColl.getName(), {collation: caseInsensitiveCollation}));

assert.commandWorked(noCollationColl.insert([
    {_id: 0, val: "a"},
    {_id: 1, val: "b", caseSensitiveColl: true},
    {_id: 2, val: "B", caseSensitiveColl: true},
    {_id: 3, val: "c"}
]));
assert.commandWorked(caseInsensitiveColl.insert([
    {_id: 0, val: "a"},
    {_id: 1, val: "B", caseSensitiveColl: false},
    {_id: 2, val: "b", caseSensitiveColl: false},
    {_id: 3, val: "c"}
]));

const unionWith = (foreignCollName, values) => {
    return [
        {$match: {val: {$in: values}}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$match: {val: {$in: values}}}]}},
        {
            $sort: {"val": 1, caseSensitiveColl: 1, _id: 1},
        },
        {$project: {_id: 0}}
    ];
};

// Verify that a $unionWith whose local collection has no default collation uses the simple
// collation for comparisons on a foreign collection with a non-simple default collation.
let results = noCollationColl.aggregate(unionWith(caseInsensitiveColl.getName(), ["B"])).toArray();
assert.docEq(results, [{val: "B", caseSensitiveColl: false}, {val: "B", caseSensitiveColl: true}]);
// Verify that a $unionWith whose local collection has no default collation but which is running in
// a pipeline with a non-simple user-specified collation uses the latter for comparisons on the
// foreign collection.
results = noCollationColl
              .aggregate(unionWith(caseInsensitiveColl.getName(), ["B"]),
                         {collation: caseInsensitiveCollation})
              .toArray();
assert.docEq(results, [
    {val: "B", caseSensitiveColl: false},  // Case insensitive match on local collection.
    {val: "b", caseSensitiveColl: false},
    {val: "b", caseSensitiveColl: true},
    {val: "B", caseSensitiveColl: true}  // Case insensitive match on foreign collection.
]);

// Verify that a $unionWith whose local collection has a non-simple collation uses the latter for
// comparisons on a foreign collection with no default collation.
results = caseInsensitiveColl.aggregate(unionWith(noCollationColl.getName(), ["B"])).toArray();
assert.docEq(results, [
    {val: "B", caseSensitiveColl: false},  // Case insensitive match on local collection.
    {val: "b", caseSensitiveColl: false},
    {val: "b", caseSensitiveColl: true},
    {val: "B", caseSensitiveColl: true}  // Case insensitive match on foreign collection.
]);

// Verify that a $unionWith whose local collection has a non-simple collation but which is running
// in a pipeline with a user-specified simple collation uses the latter for comparisons on the
// foreign collection.
results = caseInsensitiveColl
              .aggregate(unionWith(noCollationColl.getName(), ["B"]), {collation: simpleCollation})
              .toArray();
assert.docEq(results, [{val: "B", caseSensitiveColl: false}, {val: "B", caseSensitiveColl: true}]);

// Create a case-sensitive/simple view and a case-insensitive view.
testDB.noCollationView.drop();
testDB.caseInsensitiveView.drop();
assert.commandWorked(testDB.runCommand({
    create: "noCollationView",
    viewOn: noCollationColl.getName(),
    pipeline: [{$project: {val: 1, caseSensitiveView: '$caseSensitiveColl'}}]
}));
assert.commandWorked(testDB.runCommand({
    create: "caseInsensitiveView",
    viewOn: caseInsensitiveColl.getName(),
    pipeline: [{$project: {val: 1, caseSensitiveView: '$caseSensitiveColl'}}],
    collation: caseInsensitiveCollation
}));

// Verify that the command succeeds if both the pipeline and the $unionWith'd view uses a simple
// collation.
results =
    caseInsensitiveColl.aggregate(unionWith("noCollationView", ["B"]), {collation: simpleCollation})
        .toArray();
assert.docEq(results, [{val: "B", caseSensitiveView: true}, {val: "B", caseSensitiveColl: false}]);

// Verify that the command fails if the collation of the pipeline doesn't match the collation of the
// view.
assert.commandFailedWithCode(noCollationColl.runCommand({
    aggregate: noCollationColl.getName(),
    pipeline: unionWith("caseInsensitiveView", ["B"]),
    cursor: {}
}),
                             ErrorCodes.OptionNotSupportedOnView);
assert.commandFailedWithCode(noCollationColl.runCommand({
    aggregate: noCollationColl.getName(),
    pipeline: unionWith("caseInsensitiveView", ["B"]),
    collation: simpleCollation,
    cursor: {}
}),
                             ErrorCodes.OptionNotSupportedOnView);

// Verify that the command succeeds if both the pipeline and the $unionWith'd view uses a
// case-insensitive collation.
results = caseInsensitiveColl.aggregate(unionWith("caseInsensitiveView", ["B"])).toArray();
assert.docEq(results, [
    {val: "B", caseSensitiveView: false},
    {val: "b", caseSensitiveView: false},
    {val: "B", caseSensitiveColl: false},
    {val: "b", caseSensitiveColl: false}
]);
})();