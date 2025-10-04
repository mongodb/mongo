/**
 * Test that createCollection logs for a validator that can't match documents.
 */

const conn = MongoRunner.runMongod({});
const db = conn.getDB(jsTestName());
const collName = jsTestName();

function containsValidatorWarning(logLine) {
    return logLine.includes("3216000");
}
// Validator that doesn't allow '_id'.
let validator = {
    validator: {$jsonSchema: {bsonType: "object", required: ["val"], additionalProperties: false}},
};

db.createCollection(collName, validator);
let coll = db.getCollection(collName);
let log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
const logLine = log.filter(containsValidatorWarning);
// The warning appears in two lines.
assert.eq(logLine.length, 2, logLine);

// Validator that specifies '_id' doesn't log.
coll.drop();
validator = {
    validator: {
        $jsonSchema: {bsonType: "object", patternProperties: {"^_id*": {}}, additionalProperties: false},
    },
};

db.createCollection(collName, validator);
coll = db.getCollection(collName);
log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
// Should be the same as above.
assert.eq(logLine.length, 2, logLine);

// Repeat for required properties.
coll.drop();
validator = {
    validator: {$jsonSchema: {bsonType: "object", required: ["val", "_id"], additionalProperties: false}},
};

db.createCollection(collName, validator);
coll = db.getCollection(collName);
log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
// Should be the same as above.
assert.eq(logLine.length, 2, logLine);

MongoRunner.stopMongod(conn);
