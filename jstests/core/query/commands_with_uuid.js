/**
 * Tests that using a UUID as an argument to commands will retrieve results from the correct
 * collection.
 *
 * @tags: [
 *   requires_fastcount,
 *   # Runs listCollections and asserts on the output.
 *   assumes_no_implicit_index_creation,
 *   assumes_stable_collection_uuid,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const mainCollName = "main_coll";
const subCollName = "sub_coll";
const kOtherDbName = "commands_with_uuid_db";
db.runCommand({drop: mainCollName});
db.runCommand({drop: subCollName});
assert.commandWorked(db.runCommand({create: mainCollName}));
assert.commandWorked(db.runCommand({create: subCollName}));

// Check if UUIDs are enabled / supported.
let collectionInfos = db.getCollectionInfos({name: mainCollName});
let uuid = collectionInfos[0].info.uuid;
if (uuid == null) {
    quit();
}

// No support for UUIDs on mongos.
if (FixtureHelpers.isMongos(db)) {
    quit();
}

assert.commandWorked(db.runCommand({insert: mainCollName, documents: [{fooField: "FOO"}]}));
assert.commandWorked(db.runCommand({insert: subCollName, documents: [{fooField: "BAR"}, {fooField: "FOOBAR"}]}));

// Ensure passing a UUID to find retrieves results from the correct collection.
let cmd = {find: uuid};
let res = db.runCommand(cmd);
assert.commandWorked(res, "could not run " + tojson(cmd));
let cursor = new DBCommandCursor(db, res);
let errMsg = "expected more data from command " + tojson(cmd) + ", with result " + tojson(res);
assert(cursor.hasNext(), errMsg);
let doc = cursor.next();
assert.eq(doc.fooField, "FOO");
assert(!cursor.hasNext(), "expected to have exhausted cursor for results " + tojson(res));

// Although we check for both string type and BinData type for the collection identifier
// argument to a find command to accomodate for searching both by name and by UUID, if an
// invalid type is passed, the parsing error message should say the expected type is string and
// not BinData to avoid confusing the user.
cmd = {
    find: 1.0,
};
res = db.runCommand(cmd);
assert.commandFailed(res, "expected " + tojson(cmd) + " to fail.");
// In newer versions, the error message starts with "Collection name...", while in older versions it
// start with "collection name...". In order to accommodate multi-version configurations, we can
// ignore the first letter when asserting.
assert(
    res.errmsg.includes("ollection name has invalid type double"),
    "expected the error message of " + tojson(res) + " to include string type",
);

// Ensure passing a missing UUID to commands taking UUIDs uasserts that the UUID is not found.
const missingUUID = UUID();
for (cmd of [{count: missingUUID}, {find: missingUUID}, {listIndexes: missingUUID}]) {
    assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.NamespaceNotFound, "command: " + tojson(cmd));
}

// Ensure passing a UUID to listIndexes retrieves results from the correct collection.
cmd = {
    listIndexes: uuid,
};
res = db.runCommand(cmd);
assert.commandWorked(res, "could not run " + tojson(cmd));
cursor = new DBCommandCursor(db, res);
assert.eq(1, cursor.toArray().length);
assert.eq("_id_", cursor.toArray()[0].name);
assert.eq(bsonWoCompare({"_id": 1}, cursor.toArray()[0].key), 0);

// Ensure passing a UUID to count retrieves results from the correct collection.
cmd = {
    count: uuid,
};
res = db.runCommand(cmd);
assert.commandWorked(res, "could not run " + tojson(cmd));
assert.eq(res.n, 1, "expected to count a single document with command: " + tojson(cmd));

// Test that UUID resolution fails when the UUID belongs to a different database. First, we
// create a collection in another database.
const dbWithUUID = db.getSiblingDB(kOtherDbName);
dbWithUUID.getCollection(mainCollName).drop();
assert.commandWorked(dbWithUUID.runCommand({create: mainCollName}));
collectionInfos = dbWithUUID.getCollectionInfos({name: mainCollName});
uuid = collectionInfos[0].info.uuid;
assert.neq(null, uuid);
assert.commandWorked(dbWithUUID.runCommand({find: uuid}));

// Run read commands supporting UUIDs against the original database, passing the UUID from a
// different database, and verify that the UUID resolution fails with the correct error code. We
// also test that the same command succeeds when there is no database mismatch.
for (cmd of [{count: uuid}, {distinct: uuid, key: "a"}, {find: uuid}, {listIndexes: uuid}]) {
    assert.commandWorked(dbWithUUID.runCommand(cmd));
    assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.NamespaceNotFound, "command: " + tojson(cmd));
}
