/**
 * Tests that the validate command reports documents not adhering to collection schema rules.
 */
(function() {
"use strict";

// Disable the testing proctor. When the testing proctor is enabled, 'validate' will only warn about
// non-compliant documents, even when the validation action is 'error'.
TestData.testingDiagnosticsEnabled = false;

const conn = MongoRunner.runMongod();

const dbName = "test";
const collName = "validate_doc_schema";

const db = conn.getDB(dbName);

function testSchemaValidation(validationAction) {
    assert.commandWorked(db.createCollection(
        collName, {validator: {a: {$exists: true}}, validationAction: validationAction}));
    const coll = db.getCollection(collName);

    assert.commandWorked(db.runCommand(
        {insert: collName, documents: [{a: 1}, {b: 1}, {c: 1}], bypassDocumentValidation: true}));

    // Validation detects documents not adhering to the collection schema rules.
    let res = assert.commandWorked(coll.validate());
    jsTestLog(res);

    // Even though there are two documents violating the collection schema rules, the message about
    // non-compliant documents should only be shown once.
    if (validationAction == "warn") {
        assert(res.valid);
        assert.eq(res.warnings.length, 1);
        assert.eq(res.errors.length, 0);
        assert.eq(res.nNonCompliantDocuments, 2);
    } else if (validationAction == "error") {
        assert(!res.valid);
        assert.eq(res.warnings.length, 0);
        assert.eq(res.errors.length, 1);
        assert.eq(res.nNonCompliantDocuments, 2);
    }

    checkLog.containsJson(conn, 5363500, {recordId: "2"});
    checkLog.containsJson(conn, 5363500, {recordId: "3"});

    // Remove the documents violating the collection schema rules.
    assert.commandWorked(coll.remove({b: 1}));
    assert.commandWorked(coll.remove({c: 1}));

    res = assert.commandWorked(coll.validate());
    assert(res.valid);
    assert.eq(res.warnings.length, 0);
    assert.eq(res.errors.length, 0);
    assert.eq(res.nNonCompliantDocuments, 0);

    assert(coll.drop());
}

testSchemaValidation("warn");
testSchemaValidation("error");

MongoRunner.stopMongod(conn);
}());
