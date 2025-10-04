// Test that internalRenameIfOptionsAndIndexesMatch command works as expected.
//
// This command cannot be run against mongos.
// This test does not send dbVersion with internalRenameIfOptionsAndIndexeMatch, so cannot be run
// directly against shardsvrs.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   directly_against_shardsvrs_incompatible,
//   requires_capped,
//   requires_non_retryable_commands,
//   uses_rename,
//   incompatible_with_preimages_by_default,
//   # TODO (SERVER-89668): Remove tag. Currently incompatible due to collection
//   # options containing the recordIdsReplicated:true option, which
//   # this test dislikes.
//   exclude_when_record_ids_replicated,
//   requires_getmore,
// ]

const sourceColl = db.irap_cmd;
const adminDB = db.getSiblingDB("admin");
const destDB = db.getSiblingDB("irap_out_db");
const destColl = destDB.irap_out_coll;
sourceColl.drop();
destColl.drop();

assert.commandWorked(sourceColl.insert({"val": 1, "backwards": 10}));

assert.commandWorked(destColl.createIndex({"val": 1, "backwards": -1}));
let options = assert.commandWorked(destDB.runCommand({"listCollections": 1}));
let optionsArray = new DBCommandCursor(db, options).toArray();
jsTestLog("Testing against initial starting options: " + tojson(optionsArray));

let commandObj = {
    "internalRenameIfOptionsAndIndexesMatch": 1,
    from: sourceColl.getFullName(),
    to: destColl.getFullName(),
    indexes: [],
    collectionOptions: {uuid: optionsArray[0].info.uuid},
};
// Destination has an extra index.
assert.commandFailedWithCode(adminDB.runCommand(commandObj), ErrorCodes.CommandFailed);

let destIndexes = assert.commandWorked(destDB.runCommand({"listIndexes": destColl.getName()}));
commandObj.indexes = new DBCommandCursor(db, destIndexes).toArray();
jsTestLog("Testing against destination collection with indexes: " + tojson(commandObj.indexes));
assert.commandWorked(adminDB.runCommand(commandObj));

assert.commandWorked(sourceColl.insert({"val": 1, "backwards": 10}));

// Source has an extra index.
commandObj.indexes.push({"garbage": 1});
assert.commandFailedWithCode(adminDB.runCommand(commandObj), ErrorCodes.CommandFailed);

destColl.drop();

assert.commandWorked(destDB.runCommand({"create": destColl.getName(), capped: true, size: 256, max: 2}));
destIndexes = assert.commandWorked(destDB.runCommand({"listIndexes": destColl.getName()}));
commandObj.indexes = new DBCommandCursor(db, destIndexes).toArray();

// Source is missing collection options.
assert.commandFailedWithCode(adminDB.runCommand(commandObj), ErrorCodes.CommandFailed);

options = assert.commandWorked(destDB.runCommand({"listCollections": 1}));
optionsArray = new DBCommandCursor(db, options).toArray();
commandObj.collectionOptions = {
    uuid: optionsArray[0].info.uuid,
    capped: true,
    size: 256,
    max: 2,
};
assert.commandWorked(adminDB.runCommand(commandObj));
