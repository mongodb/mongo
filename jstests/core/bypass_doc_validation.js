// Test the bypassDocumentValidation flag with some database commands. The test uses relevant shell
// helpers when they're available for the respective server commands.

(function() {
    'use strict';

    var dbName = 'bypass_document_validation';
    var collName = 'bypass_document_validation';
    var myDb = db.getSiblingDB(dbName);
    var coll = myDb[collName];
    var docValidationErrorCode = ErrorCodes.DocumentValidationFailure;
    coll.drop();

    // Add a validator to an existing collection.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.insert({_id: 2}));
    assert.commandWorked(myDb.runCommand({collMod: collName, validator: {a: {$exists: true}}}));

    // Test applyOps with a simple insert if not on mongos.
    if (!db.runCommand({isdbgrid: 1}).isdbgrid) {
        var op = [{ts: Timestamp(0, 0), h: 1, v: 2, op: 'i', ns: coll.getFullName(), o: {_id: 9}}];
        assert.commandFailedWithCode(
            myDb.runCommand({applyOps: op, bypassDocumentValidation: false}),
            ErrorCodes.DocumentValidationFailure);
        assert.eq(0, coll.count({_id: 9}));
        assert.commandWorked(myDb.runCommand({applyOps: op, bypassDocumentValidation: true}));
        assert.eq(1, coll.count({_id: 9}));
    }

    // Test aggregation with $out collection.
    var outputCollName = 'bypass_output_coll';
    var outputColl = myDb[outputCollName];
    outputColl.drop();
    assert.commandWorked(myDb.createCollection(outputCollName, {validator: {a: {$exists: true}}}));

    // Test the aggregate shell helper.
    var pipeline =
        [{$match: {_id: 1}}, {$project: {aggregation: {$add: [1]}}}, {$out: outputCollName}];
    assert.throws(function() {
        coll.aggregate(pipeline, {bypassDocumentValidation: false});
    });
    assert.eq(0, outputColl.count({aggregation: 1}));
    coll.aggregate(pipeline, {bypassDocumentValidation: true});
    assert.eq(1, outputColl.count({aggregation: 1}));

    // Test the copyDb command.
    var copyDbName = dbName + '_copy';
    myDb.getSiblingDB(copyDbName).dropDatabase();
    assert.commandFailedWithCode(
        db.adminCommand(
            {copydb: 1, fromdb: dbName, todb: copyDbName, bypassDocumentValidation: false}),
        docValidationErrorCode);
    assert.eq(0, db.getSiblingDB(copyDbName)[collName].count());
    myDb.getSiblingDB(copyDbName).dropDatabase();
    assert.commandWorked(db.adminCommand(
        {copydb: 1, fromdb: dbName, todb: copyDbName, bypassDocumentValidation: true}));
    assert.eq(coll.count(), db.getSiblingDB(copyDbName)[collName].count());

    // Test the findAndModify shell helper.
    assert.throws(function() {
        coll.findAndModify({update: {$set: {findAndModify: 1}}, bypassDocumentValidation: false});
    });
    assert.eq(0, coll.count({findAndModify: 1}));
    coll.findAndModify({update: {$set: {findAndModify: 1}}, bypassDocumentValidation: true});
    assert.eq(1, coll.count({findAndModify: 1}));

    // Test the map/reduce command.
    var map = function() {
        emit(1, 1);
    };
    var reduce = function(k, vs) {
        return 'mapReduce';
    };
    assert.commandFailedWithCode(coll.runCommand({
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {replace: outputCollName},
        bypassDocumentValidation: false
    }),
                                 docValidationErrorCode);
    assert.eq(0, outputColl.count({value: 'mapReduce'}));
    var res = coll.runCommand({
        mapReduce: collName,
        map: map,
        reduce: reduce,
        out: {replace: outputCollName},
        bypassDocumentValidation: true
    });
    assert.commandWorked(res);
    assert.eq(1, outputColl.count({value: 'mapReduce'}));

    // Test the insert command.  Includes a test for a doc with no _id (SERVER-20859).
    res = myDb.runCommand({insert: collName, documents: [{}], bypassDocumentValidation: false});
    assert.eq(res.writeErrors[0].code, docValidationErrorCode, tojson(res));
    res = myDb.runCommand(
        {insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: false});
    assert.eq(0, coll.count({_id: 6}));
    assert.eq(res.writeErrors[0].code, docValidationErrorCode, tojson(res));
    res = myDb.runCommand(
        {insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: true});
    assert.commandWorked(res);
    assert.eq(null, res.writeErrors);
    assert.eq(1, coll.count({_id: 6}));

    // Test the update command.
    res = myDb.runCommand({
        update: collName,
        updates: [{q: {}, u: {$set: {update: 1}}}],
        bypassDocumentValidation: false
    });
    assert.eq(res.writeErrors[0].code, docValidationErrorCode, tojson(res));
    assert.eq(0, coll.count({update: 1}));
    res = myDb.runCommand({
        update: collName,
        updates: [{q: {}, u: {$set: {update: 1}}}],
        bypassDocumentValidation: true
    });
    assert.commandWorked(res);
    assert.eq(null, res.writeErrors);
    assert.eq(1, coll.count({update: 1}));
})();
