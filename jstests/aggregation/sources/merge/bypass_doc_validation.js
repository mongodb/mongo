/**
 * Tests for $merge with bypassDocumentValidation.
 *
 * @tags: [assumes_unsharded_collection]
 */
(function() {
"use strict";

load("jstests/libs/doc_validation_utils.js");  // For assertDocumentValidationFailure.

const testDB = db.getSiblingDB("out_bypass_doc_val");
const sourceColl = testDB.getCollection("source");
const targetColl = testDB.getCollection("target");

targetColl.drop();
assert.commandWorked(testDB.createCollection(targetColl.getName(), {validator: {a: 2}}));

sourceColl.drop();
assert.commandWorked(sourceColl.insert({_id: 0, a: 1}));

// Test that the bypassDocumentValidation flag is passed through to the writes on the output
// collection.
(function testBypassDocValidationTrue() {
    sourceColl.aggregate([{$merge: targetColl.getName()}], {bypassDocumentValidation: true});
    assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

    sourceColl.aggregate(
        [
            {$addFields: {a: 3}},
            {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
        ],
        {bypassDocumentValidation: true});
    assert.eq([{_id: 0, a: 3}], targetColl.find().toArray());

    sourceColl.aggregate(
        [
            {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
            {$merge: {into: targetColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}
        ],
        {bypassDocumentValidation: true});
    assert.eq([{_id: 0, a: 3}, {_id: 1, a: 4}], targetColl.find().sort({_id: 1}).toArray());
}());

// Test that mode "replaceDocuments" passes without the bypassDocumentValidation flag if the
// updated doc is valid.
(function testReplacementStyleUpdateWithoutBypass() {
    sourceColl.aggregate([
        {$addFields: {a: 2}},
        {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
    ]);
    assert.eq([{_id: 0, a: 2}], targetColl.find({_id: 0}).toArray());
    sourceColl.aggregate(
        [
            {$addFields: {a: 2}},
            {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
        ],
        {bypassDocumentValidation: false});
    assert.eq([{_id: 0, a: 2}], targetColl.find({_id: 0}).toArray());
}());

function assertFailsValidation(cmdOptions) {
    assert.commandWorked(targetColl.remove({}));
    let cmd = {aggregate: sourceColl.getName(), cursor: {}};
    for (let opt of Object.keys(cmdOptions)) {
        cmd[opt] = cmdOptions[opt];
    }

    cmd.pipeline = [{$merge: targetColl.getName()}];
    assertDocumentValidationFailure(testDB.runCommand(cmd), sourceColl);

    cmd.pipeline = [
        {$addFields: {a: 3}},
        {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
    ];
    assertDocumentValidationFailure(testDB.runCommand(cmd), sourceColl);

    cmd.pipeline = [
        {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
        {$merge: {into: targetColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}
    ];
    assertDocumentValidationFailure(testDB.runCommand(cmd), sourceColl);

    assert.eq(0, targetColl.find().itcount());
}

// Test that $merge fails if the output document is not valid, and the bypassDocumentValidation
// flag is not set.
assertFailsValidation({});

// Test that $merge fails if the output document is not valid, and the bypassDocumentValidation
// flag is explicitly set to false.
assertFailsValidation({bypassDocumentValidation: false});

// Test that bypassDocumentValidation is *not* needed if the source collection has a
// validator but the output collection does not.
(function testDocValidatorOnSourceCollection() {
    targetColl.drop();
    assert.commandWorked(testDB.runCommand({collMod: sourceColl.getName(), validator: {a: 1}}));

    sourceColl.aggregate([{$merge: targetColl.getName()}]);
    assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

    sourceColl.aggregate([
        {$addFields: {a: 3}},
        {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
    ]);
    assert.eq([{_id: 0, a: 3}], targetColl.find().toArray());

    sourceColl.aggregate([
        {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
        {$merge: {into: targetColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}
    ]);
    assert.eq([{_id: 0, a: 3}, {_id: 1, a: 4}], targetColl.find().sort({_id: 1}).toArray());
}());

// Test that the bypassDocumentValidation is casted to true if the value is an integer and the value
// is not 0. Note that the value type can _not_ be string.
(function testNonBooleanBypassDocValidationFlag() {
    assert.commandWorked(targetColl.remove({}));
    assert.commandWorked(testDB.runCommand({collMod: targetColl.getName(), validator: {a: 1}}));
    sourceColl.drop();
    assert.commandWorked(sourceColl.insert({_id: 0, a: 1}));

    sourceColl.aggregate([{$merge: targetColl.getName()}], {bypassDocumentValidation: 5});
    assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: sourceColl.getName(),
        pipeline: [
            {$addFields: {a: 3}},
            {$merge: {into: targetColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}
        ],
        bypassDocumentValidation: "false"
    }),
                                 ErrorCodes.TypeMismatch);
}());

// Test bypassDocumentValidation with $merge to a collection in a foreign database.
(function testForeignDb() {
    const foreignDB = db.getSiblingDB("foreign_db");
    const foreignColl = foreignDB.foreign_coll;
    foreignColl.drop();
    assert.commandWorked(foreignDB.createCollection(foreignColl.getName(), {validator: {a: 2}}));

    sourceColl.aggregate(
        [
            {$addFields: {a: 3}},
            {
                $merge: {
                    into: {
                        db: foreignDB.getName(),
                        coll: foreignColl.getName(),
                    },
                    whenMatched: "replace",
                    whenNotMatched: "insert"
                }
            }
        ],
        {bypassDocumentValidation: true});
    assert.eq([{_id: 0, a: 3}], foreignColl.find().toArray());

    sourceColl.aggregate(
        [
            {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
            {
                $merge: {
                    into: {
                        db: foreignDB.getName(),
                        coll: foreignColl.getName(),
                    },
                    whenMatched: "fail",
                    whenNotMatched: "insert"
                }
            }
        ],
        {bypassDocumentValidation: true});
    assert.eq([{_id: 0, a: 3}, {_id: 1, a: 4}], foreignColl.find().sort({_id: 1}).toArray());

    assert.commandWorked(foreignColl.remove({}));

    let cmd = {aggregate: sourceColl.getName(), cursor: {}};

    cmd.pipeline = [
        {$addFields: {a: 3}},
        {
            $merge: {
                into: {
                    db: foreignDB.getName(),
                    coll: foreignColl.getName(),
                },
                whenMatched: "replace",
                whenNotMatched: "insert"
            }
        }
    ];
    assertDocumentValidationFailure(testDB.runCommand(cmd), sourceColl);

    cmd.pipeline = [
        {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
        {
            $merge: {
                into: {
                    db: foreignDB.getName(),
                    coll: foreignColl.getName(),
                },
                whenMatched: "fail",
                whenNotMatched: "insert"
            }
        }
    ];
    assertDocumentValidationFailure(testDB.runCommand(cmd), sourceColl);

    assert.eq(0, foreignColl.find().itcount());
}());
}());
