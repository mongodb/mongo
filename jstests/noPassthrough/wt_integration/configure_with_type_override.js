/**
 * Verify that a user cannot override the WiredTiger 'type' option via a collection or index
 * 'configString'.
 *
 * @tags: [
 *  requires_wiredtiger
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const collName = "coll";

// A bare, unquoted type=file (the default) must still be accepted.
assert.commandWorked(
    db.createCollection(collName, {storageEngine: {wiredTiger: {configString: "type=file"}}}),
);
db.getCollection(collName).drop();

// A quoted type override must be rejected at creation time.
assert.commandFailedWithCode(
    db.createCollection(collName, {storageEngine: {wiredTiger: {configString: 'type="file"'}}}),
    ErrorCodes.IllegalOperation,
);

// Non-file type values, quoted or bare, must also be rejected.
assert.commandFailedWithCode(
    db.createCollection(collName, {storageEngine: {wiredTiger: {configString: 'type="lsm"'}}}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.createCollection(collName, {storageEngine: {wiredTiger: {configString: "type=lsm"}}}),
    ErrorCodes.IllegalOperation,
);

// A syntactically malformed configString must be rejected as a parse error rather than crashing
// the server (the type check runs after WiredTiger's own config validation succeeds).
assert.commandFailedWithCode(
    db.createCollection(collName, {storageEngine: {wiredTiger: {configString: "type=("}}}),
    ErrorCodes.BadValue,
);

// Index creation flows through the same validator.
assert.commandWorked(db.createCollection(collName));
const coll = db.getCollection(collName);
assert.commandFailedWithCode(
    coll.createIndex({field: 1}, {storageEngine: {wiredTiger: {configString: 'type="file"'}}}),
    ErrorCodes.IllegalOperation,
);

// A normally-created collection is unaffected and the server is still healthy.
assert.commandWorked(db.runCommand({collStats: collName}));

MongoRunner.stopMongod(conn);
