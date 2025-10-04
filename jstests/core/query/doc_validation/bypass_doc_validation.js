/**
 * Tests that various database commands respect the 'bypassDocumentValidation' flag:
 *
 * - aggregation with $out
 * - applyOps (when not sharded)
 * - findAndModify
 * - insert
 * - mapReduce
 * - update
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps, mapReduce.
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fastcount,
 *   # 6.2 removes support for atomic applyOps
 *   requires_fcv_62,
 *   requires_non_retryable_commands,
 *   uses_map_reduce_with_temp_collections,
 *   # This test has statements that do not support non-local read concern.
 *   does_not_support_causal_consistency,
 *   references_foreign_collection,
 *   # TODO SERVER-88275: mapReduce can fail with QueryPlanKilled in suites with random migrations
 *   # because moveCollection change the collection UUID # by dropping and re-creating the
 *   # collection.
 *   assumes_balancer_off,
 *   # Uses mapReduce
 *   requires_scripting,
 * ]
 */

import {
    assertDocumentValidationFailure,
    assertDocumentValidationFailureCheckLogs,
} from "jstests/libs/doc_validation_utils.js";

const dbName = "bypass_document_validation";
const collName = "bypass_document_validation";
const myDb = db.getSiblingDB(dbName);
const coll = myDb[collName];

/**
 * Tests that we can bypass document validation when appropriate when a collection has validator
 * 'validator', which should enforce the existence of a field "a".
 */
function runBypassDocumentValidationTest(validator) {
    // Use majority write concern to clear the drop-pending that can cause lock conflicts with
    // transactions.
    coll.drop({writeConcern: {w: "majority"}});

    // Insert documents into the collection that would not be valid before setting 'validator'.
    assert.commandWorked(coll.insert({_id: 1}));
    assert.commandWorked(coll.insert({_id: 2}));
    assert.commandWorked(myDb.runCommand({collMod: collName, validator: validator}));

    const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
    // Test applyOps with a simple insert if not on mongos.
    if (!isMongos) {
        const op = [{op: "i", ns: coll.getFullName(), o: {_id: 9}}];
        const res = myDb.runCommand({applyOps: op, bypassDocumentValidation: false});
        assert.commandFailedWithCode(res, ErrorCodes.UnknownError, tojson(res));
        assertDocumentValidationFailureCheckLogs(myDb);
        assert.eq(0, coll.count({_id: 9}));
        assert.commandWorked(myDb.runCommand({applyOps: op, bypassDocumentValidation: true}));
        assert.eq(1, coll.count({_id: 9}));
    }

    // Test the aggregation command with a $out stage.
    const outputCollName = "bypass_output_coll";
    const outputColl = myDb[outputCollName];
    outputColl.drop();
    assert.commandWorked(myDb.createCollection(outputCollName, {validator: validator}));
    const pipeline = [{$match: {_id: 1}}, {$project: {aggregation: {$add: [1]}}}, {$out: outputCollName}];
    const cmd = {aggregate: collName, cursor: {}, pipeline: pipeline, bypassDocumentValidation: false};
    assertDocumentValidationFailure(myDb.runCommand(cmd), coll);
    assert.eq(0, outputColl.count({aggregation: 1}));
    coll.aggregate(pipeline, {bypassDocumentValidation: true});
    assert.eq(1, outputColl.count({aggregation: 1}));

    // Test the findAndModify command.
    assert.throws(function () {
        coll.findAndModify({update: {$set: {findAndModify: 1}}, bypassDocumentValidation: false});
    });
    assert.eq(0, coll.count({findAndModify: 1}));
    coll.findAndModify({update: {$set: {findAndModify: 1}}, bypassDocumentValidation: true});
    assert.eq(1, coll.count({findAndModify: 1}));

    // Test the mapReduce command.
    const map = function () {
        emit(1, 1);
    };
    const reduce = function () {
        return "mapReduce";
    };
    let res = myDb.runCommand({
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {replace: outputCollName},
        bypassDocumentValidation: false,
    });
    assertDocumentValidationFailure(res, coll);
    assert.eq(0, outputColl.count({value: "mapReduce"}));
    res = myDb.runCommand({
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {replace: outputCollName},
        bypassDocumentValidation: true,
    });
    assert.commandWorked(res);
    assert.eq(1, outputColl.count({value: "mapReduce"}));

    // Test the mapReduce command if it is reading from a different database and collection without
    // validation.
    const otherDb = myDb.getSiblingDB("mr_second_input_db");
    const otherDbColl = otherDb.mr_second_input_coll;
    assert.commandWorked(otherDbColl.insert({val: 1}));
    outputColl.drop();
    assert.commandWorked(myDb.createCollection(outputCollName, {validator: validator}));
    res = otherDb.runCommand({
        mapReduce: otherDbColl.getName(),
        map: map,
        reduce: reduce,
        out: {replace: outputCollName, db: myDb.getName()},
        bypassDocumentValidation: false,
    });
    assertDocumentValidationFailure(res, coll);
    assert.eq(0, outputColl.count({value: "mapReduce"}));
    res = otherDb.runCommand({
        mapReduce: otherDbColl.getName(),
        map: map,
        reduce: reduce,
        out: {replace: outputCollName, db: myDb.getName()},
        bypassDocumentValidation: true,
    });
    assert.commandWorked(res);
    assert.eq(1, outputColl.count({value: "mapReduce"}));
    // Test the insert command. Includes a test for a document with no _id (SERVER-20859).
    res = myDb.runCommand({insert: collName, documents: [{}], bypassDocumentValidation: false});
    assertDocumentValidationFailure(BulkWriteResult(res), coll);
    res = myDb.runCommand({insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: false});
    assertDocumentValidationFailure(BulkWriteResult(res), coll);
    res = myDb.runCommand({insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: true});
    assert.commandWorked(res);

    // Test the update command.
    res = myDb.runCommand({
        update: collName,
        updates: [{q: {}, u: {$set: {update: 1}}}],
        bypassDocumentValidation: false,
    });
    assertDocumentValidationFailure(BulkWriteResult(res), coll);
    assert.eq(0, coll.count({update: 1}));
    res = myDb.runCommand({
        update: collName,
        updates: [{q: {}, u: {$set: {update: 1}}}],
        bypassDocumentValidation: true,
    });
    assert.commandWorked(res);
    assert.eq(1, coll.count({update: 1}));

    // Pipeline-style update is only supported for commands and not for OP_UPDATE which cannot
    // differentiate between an update object and an array.
    res = myDb.runCommand({
        update: collName,
        updates: [{q: {}, u: [{$set: {pipeline: 1}}]}],
        bypassDocumentValidation: false,
    });
    assertDocumentValidationFailure(BulkWriteResult(res), coll);
    assert.eq(0, coll.count({pipeline: 1}));

    assert.commandWorked(
        myDb.runCommand({
            update: collName,
            updates: [{q: {}, u: [{$set: {pipeline: 1}}]}],
            bypassDocumentValidation: true,
        }),
    );
    assert.eq(1, coll.count({pipeline: 1}));

    assert.commandFailed(
        myDb.runCommand({
            findAndModify: collName,
            update: [{$set: {findAndModifyPipeline: 1}}],
            bypassDocumentValidation: false,
        }),
    );
    assert.eq(0, coll.count({findAndModifyPipeline: 1}));

    assert.commandWorked(
        myDb.runCommand({
            findAndModify: collName,
            update: [{$set: {findAndModifyPipeline: 1}}],
            bypassDocumentValidation: true,
        }),
    );
    assert.eq(1, coll.count({findAndModifyPipeline: 1}));
}

// Run the test using a normal validator.
runBypassDocumentValidationTest({a: {$exists: true}});

// Run the test again with an equivalent JSON Schema validator.
runBypassDocumentValidationTest({$jsonSchema: {required: ["a"]}});
