let standalone = MongoRunner.runMongod();
let adminDB = standalone.getDB("admin");

// Get the uuid of the original admin.system.version.
let res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
assert.commandWorked(res, "failed to list collections");
assert.eq(1, res.cursor.firstBatch.length);
let originalUUID = res.cursor.firstBatch[0].info.uuid;
let newUUID = UUID();

// Create new collection, insert new FCV document and then delete the
// original collection.
let createNewAdminSystemVersionCollection = {op: "c", ns: "admin.$cmd", ui: newUUID, o: {create: "system.version"}};
let insertFCVDocument = {
    op: "i",
    ns: "admin.system.version",
    o: {_id: "featureCompatibilityVersion", version: latestFCV},
};
let dropOriginalAdminSystemVersionCollection = {
    op: "c",
    ns: "admin.$cmd",
    ui: originalUUID,
    o: {drop: "admin.tmp_system_version"},
};
let cmd = {
    applyOps: [createNewAdminSystemVersionCollection, insertFCVDocument, dropOriginalAdminSystemVersionCollection],
};
assert.commandWorked(adminDB.runCommand(cmd), "failed command " + tojson(cmd));

// Now admin.system.version is overwritten with the new entry.
res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
assert.commandWorked(res, "failed to list collections");
assert.eq(1, res.cursor.firstBatch.length);
assert.eq(newUUID, res.cursor.firstBatch[0].info.uuid);

MongoRunner.stopMongod(standalone);
