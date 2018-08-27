/**
 * Tests for $out with bypassDocumentValidation.
 *
 * @tags: [assumes_unsharded_collection]
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

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
        sourceColl.aggregate([{$out: targetColl.getName()}], {bypassDocumentValidation: true});
        assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "replaceCollection"}}],
                             {bypassDocumentValidation: true});
        assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

        sourceColl.aggregate(
            [{$addFields: {a: 3}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}],
            {bypassDocumentValidation: true});
        assert.eq([{_id: 0, a: 3}], targetColl.find().toArray());

        sourceColl.aggregate(
            [
              {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
              {$out: {to: targetColl.getName(), mode: "insertDocuments"}}
            ],
            {bypassDocumentValidation: true});
        assert.eq([{_id: 0, a: 3}, {_id: 1, a: 4}], targetColl.find().sort({_id: 1}).toArray());
    }());

    // Test that mode "replaceDocuments" passes without the bypassDocumentValidation flag if the
    // updated doc is valid.
    (function testReplacementStyleUpdateWithoutBypass() {
        sourceColl.aggregate(
            [{$addFields: {a: 2}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}]);
        assert.eq([{_id: 0, a: 2}], targetColl.find({_id: 0}).toArray());
        sourceColl.aggregate(
            [{$addFields: {a: 2}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}],
            {bypassDocumentValidation: false});
        assert.eq([{_id: 0, a: 2}], targetColl.find({_id: 0}).toArray());
    }());

    function assertDocValidationFailure(cmdOptions) {
        targetColl.remove({});
        assertErrorCode(sourceColl,
                        [{$out: targetColl.getName()}],
                        ErrorCodes.DocumentValidationFailure,
                        "Expected failure without bypass set",
                        cmdOptions);

        assertErrorCode(sourceColl,
                        [{$out: {to: targetColl.getName(), mode: "replaceCollection"}}],
                        ErrorCodes.DocumentValidationFailure,
                        "Expected failure without bypass set",
                        cmdOptions);

        assertErrorCode(
            sourceColl,
            [{$addFields: {a: 3}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}],
            ErrorCodes.DocumentValidationFailure,
            "Expected failure without bypass set",
            cmdOptions);

        assertErrorCode(sourceColl,
                        [
                          {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
                          {$out: {to: targetColl.getName(), mode: "insertDocuments"}}
                        ],
                        ErrorCodes.DocumentValidationFailure,
                        "Expected failure without bypass set",
                        cmdOptions);
        assert.eq(0, targetColl.find().itcount());
    }

    // Test that $out fails if the output document is not valid, and the bypassDocumentValidation
    // flag is not set.
    assertDocValidationFailure({});

    // Test that $out fails if the output document is not valid, and the bypassDocumentValidation
    // flag is explicitly set to false.
    assertDocValidationFailure({bypassDocumentValidation: false});

    // Test that bypassDocumentValidation is *not* needed if the source collection has a
    // validator but the output collection does not.
    (function testDocValidatorOnSourceCollection() {
        targetColl.drop();
        assert.commandWorked(testDB.runCommand({collMod: sourceColl.getName(), validator: {a: 1}}));

        sourceColl.aggregate([{$out: targetColl.getName()}]);
        assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

        sourceColl.aggregate([{$out: {to: targetColl.getName(), mode: "replaceCollection"}}]);
        assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

        sourceColl.aggregate(
            [{$addFields: {a: 3}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}]);
        assert.eq([{_id: 0, a: 3}], targetColl.find().toArray());

        sourceColl.aggregate([
            {$replaceRoot: {newRoot: {_id: 1, a: 4}}},
            {$out: {to: targetColl.getName(), mode: "insertDocuments"}}
        ]);
        assert.eq([{_id: 0, a: 3}, {_id: 1, a: 4}], targetColl.find().sort({_id: 1}).toArray());
    }());

    // Test that the bypassDocumentValidation is casted to true if the value is non-boolean.
    (function testNonBooleanBypassDocValidationFlag() {
        targetColl.remove({});
        assert.commandWorked(testDB.runCommand({collMod: targetColl.getName(), validator: {a: 1}}));
        sourceColl.drop();
        assert.commandWorked(sourceColl.insert({_id: 0, a: 1}));

        sourceColl.aggregate([{$out: targetColl.getName()}], {bypassDocumentValidation: 5});
        assert.eq([{_id: 0, a: 1}], targetColl.find().toArray());

        sourceColl.aggregate(
            [{$addFields: {a: 3}}, {$out: {to: targetColl.getName(), mode: "replaceDocuments"}}],
            {bypassDocumentValidation: "false"});
        assert.eq([{_id: 0, a: 3}], targetColl.find().toArray());
    }());
}());
