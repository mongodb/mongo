// SERVER-30665 Ensure that a non-empty collMod with a nonexistent UUID is not applied
// in applyOps.

(function() {
    "use strict";
    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up with empty options");

    let dbCollModName = "db_coll_mod";
    const dbCollMod = conn.getDB(dbCollModName);
    dbCollMod.dropDatabase();
    let collName = "collModTest";
    let coll = dbCollMod[collName];

    // Generate a random UUID that is distinct from collModTest's UUID.
    const randomUUID = UUID();
    assert.neq(randomUUID, coll.uuid);

    // Perform a collMod to initialize validationLevel to "off".
    assert.commandWorked(dbCollMod.createCollection(collName));
    let cmd = {"collMod": collName, "validationLevel": "off"};
    let res = dbCollMod.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    let collectionInfosOriginal = dbCollMod.getCollectionInfos()[0];
    assert.eq(collectionInfosOriginal.options.validationLevel, "off");

    // Perform an applyOps command with a nonexistent UUID and the same name as an existing
    // collection. applyOps should succeed because of idempotency but a NamespaceNotFound
    // uassert should be thrown during collMod application.
    let collModApplyOpsEntry = {
        "v": 2,
        "op": "c",
        "ns": dbCollModName + ".$cmd",
        "ui": randomUUID,
        "o2": {"collectionOptions_old": {"uuid": randomUUID}},
        "o": {"collMod": collName, "validationLevel": "moderate"}
    };
    assert.commandWorked(dbCollMod.adminCommand({"applyOps": [collModApplyOpsEntry]}));

    // Ensure the collection options of the existing collection were not affected.
    assert.eq(dbCollMod.getCollectionInfos()[0].name, collName);
    assert.eq(dbCollMod.getCollectionInfos()[0].options.validationLevel, "off");
}());
