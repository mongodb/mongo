// Tests the behavior of $merge with whenMatched: "fail" and whenNotMatched: "insert".
// @tags: [assumes_unsharded_collection, assumes_no_implicit_collection_creation_after_drop]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.isMongos.

const coll = db.merge_insert_only;
coll.drop();

const targetColl = db.merge_insert_only_out;
targetColl.drop();

const pipeline =
    [{$merge: {into: targetColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}];

//
// Test $merge with a non-existent output collection.
//
assert.commandWorked(coll.insert({_id: 0}));

coll.aggregate(pipeline);
assert.eq(1, targetColl.find().itcount());

//
// Test $merge with an existing output collection.
//
assert.commandWorked(coll.remove({_id: 0}));
assert.commandWorked(coll.insert({_id: 1}));
coll.aggregate(pipeline);
assert.eq(2, targetColl.find().itcount());

//
// Test that $merge fails if there's a duplicate key error.
//
assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

//
// Test that $merge will preserve the indexes and options of the output collection.
//
const validator = {
    a: {$gt: 0}
};
targetColl.drop();
assert.commandWorked(db.createCollection(targetColl.getName(), {validator: validator}));
assert.commandWorked(targetColl.createIndex({a: 1}));

coll.drop();
assert.commandWorked(coll.insert({a: 1}));

coll.aggregate(pipeline);
assert.eq(1, targetColl.find().itcount());
assert.eq(2, targetColl.getIndexes().length);

const listColl = db.runCommand({listCollections: 1, filter: {name: targetColl.getName()}});
assert.commandWorked(listColl);
assert.eq(validator, listColl.cursor.firstBatch[0].options["validator"]);

//
// Test that $merge fails if it violates a unique index constraint.
//
coll.drop();
assert.commandWorked(coll.insert([{_id: 0, a: 0}, {_id: 1, a: 0}]));
targetColl.drop();
assert.commandWorked(targetColl.createIndex({a: 1}, {unique: true}));

assertErrorCode(coll, pipeline, ErrorCodes.DuplicateKey);

//
// Test that a $merge aggregation succeeds even if the _id is stripped out and the "unique key"
// is the document key, which will be _id for a new collection.
//
coll.drop();
assert.commandWorked(coll.insert({a: 0}));
targetColl.drop();
assert.doesNotThrow(() => coll.aggregate([
    {$project: {_id: 0}},
    {$merge: {into: targetColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}},
]));
assert.eq(1, targetColl.find().itcount());

//
// Test that a $merge aggregation succeeds even if the _id is stripped out and _id is included
// in the "on" fields.
//
coll.drop();
assert.commandWorked(coll.insert([{_id: "should be projected away", name: "kyle"}]));
targetColl.drop();
assert.commandWorked(targetColl.createIndex({_id: 1, name: -1}, {unique: true}));
assert.doesNotThrow(() => coll.aggregate([
    {$project: {_id: 0}},
    {
        $merge: {
            into: targetColl.getName(),
            whenMatched: "fail",
            whenNotMatched: "insert",
            on: ["_id", "name"]
        }
    },
]));
assert.eq(1, targetColl.find().itcount());

//
// Tests for $merge to a database that differs from the aggregation database.
//
const foreignDb = db.getSiblingDB("merge_insert_only_foreign");
const foreignTargetColl = foreignDb.merge_insert_only_out;
const pipelineDifferentOutputDb = [
    {$project: {_id: 0}},
    {
        $merge: {
            into: {
                db: foreignDb.getName(),
                coll: foreignTargetColl.getName(),
            },
            whenMatched: "fail",
            whenNotMatched: "insert",
        }
    }
];

foreignDb.dropDatabase();
coll.drop();
assert.commandWorked(coll.insert({a: 1}));

if (!FixtureHelpers.isMongos(db)) {
    //
    // Test that $merge implicitly creates a new database when the output collection's database
    // doesn't exist.
    //
    coll.aggregate(pipelineDifferentOutputDb);
    assert.eq(foreignTargetColl.find().itcount(), 1);
} else {
    // Implicit database creation is prohibited in a cluster.
    const error = assert.throws(() => coll.aggregate(pipelineDifferentOutputDb));
    assert.commandFailedWithCode(error, ErrorCodes.NamespaceNotFound);

    // Explicitly create the collection and database, then fall through to the test below.
    assert.commandWorked(foreignTargetColl.insert({val: "forcing database creation"}));
}

//
// Re-run the $merge aggregation, which should merge with the existing contents of the
// collection. We rely on implicit _id generation to give us unique _id values.
//
assert.doesNotThrow(() => coll.aggregate(pipelineDifferentOutputDb));
assert.eq(foreignTargetColl.find().itcount(), 2);
}());
