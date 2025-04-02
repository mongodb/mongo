/**
 * This test is to validate the 'historyStorageStats' sub-section of the 'wiredTiger' server status
 * section.
 *
 * @tags: [
 *   requires_fcv_81,
 *   requires_wiredTiger,
 *   # In memory variants does not have the History Storage file. As such, we do not run this test
 *   # on in-memory variants.
 *   requires_persistence,
 * ]
 */

let conn = MongoRunner.runMongod();
let adminDb = conn.getDB('admin');

assert.soon(() => {
    // Verify 'wiredTiger' section is present.
    let serverStatus = assert.commandWorked(adminDb.runCommand({serverStatus: 1}));
    if (!serverStatus.hasOwnProperty("wiredTiger")) {
        jsTestLog("does not have 'wiredTiger' in '" + tojson(serverStatus) + "'");
        return false;
    }

    // Verify that it contains 'wiredTiger'.
    assert(serverStatus["wiredTiger"].hasOwnProperty("historyStorageStats"),
           "does not have 'wiredTiger.historyStorageStats' in '" + tojson(serverStatus) + "'");

    // An error on the wiredTiger can represent that WT is not ready for statistics yet
    if (serverStatus["wiredTiger"]["historyStorageStats"].hasOwnProperty("error")) {
        let reason = "";
        if (serverStatus["wiredTiger"]["historyStorageStats"].hasOwnProperty("reason")) {
            reason = ": '" + serverStatus["wiredTiger"]["historyStorageStats"]["reason"] + "'";
        }
        jsTestLog("got error on 'wiredTiger.historyStorageStats': '" +
                  serverStatus["wiredTiger"]["historyStorageStats"]["error"] + "'" + reason);
        return false;
    }

    // Verify that 'historyStorageStats' is not empty and contains at least some of the important
    // fields are presents.

    // block-manager
    assert(serverStatus["wiredTiger"]["historyStorageStats"].hasOwnProperty("block-manager"),
           "does not have 'wiredTiger.historyStorageStats.block-manager' in '" +
               tojson(serverStatus) + "'");
    assert(
        serverStatus["wiredTiger"]["historyStorageStats"]["block-manager"].hasOwnProperty(
            "file size in bytes"),
        "does not have 'file size in bytes' in wiredTiger.historyStorageStats.block-manager in '" +
            tojson(serverStatus) + "'");
    assert(
        serverStatus["wiredTiger"]["historyStorageStats"]["block-manager"].hasOwnProperty(
            "file bytes available for reuse"),
        "does not have 'file bytes available for reuse' in wiredTiger.historyStorageStats.block-manager in '" +
            tojson(serverStatus) + "'");

    // btree
    assert(
        serverStatus["wiredTiger"]["historyStorageStats"].hasOwnProperty("btree"),
        "does not have 'wiredTiger.historyStorageStats.btree' in '" + tojson(serverStatus) + "'");
    assert(serverStatus["wiredTiger"]["historyStorageStats"]["btree"].hasOwnProperty(
               "maximum tree depth"),
           "does not have 'maximum tree depth' in wiredTiger.historyStorageStats.btree in '" +
               tojson(serverStatus) + "'");

    return true;
}, "Could not validate the historyStorageStats");

MongoRunner.stopMongod(conn);
