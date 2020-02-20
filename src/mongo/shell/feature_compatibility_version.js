// Contains helpers for checking the featureCompatibilityVersion and constants for the current
// featureCompatibilityVersion values.

/**
 * These constants represent the current "latest" and "last-stable" values for the
 * featureCompatibilityVersion parameter. They should only be used for testing of upgrade-downgrade
 * scenarios that are intended to be maintained between releases.
 *
 * We cannot use `const` when declaring them because it must be possible to load() this file
 * multiple times.
 */

var latestFCV = "4.4";
var lastStableFCV = "4.2";

/**
 * Checks the featureCompatibilityVersion document and server parameter. The
 * featureCompatibilityVersion document is of the form {_id: "featureCompatibilityVersion", version:
 * <required>, targetVersion: <optional>}. The getParameter result is of the form
 * {featureCompatibilityVersion: {version: <required>, targetVersion: <optional>}, ok: 1}.
 */
function checkFCV(adminDB, version, targetVersion) {
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion.version, version, tojson(res));
    assert.eq(res.featureCompatibilityVersion.targetVersion, targetVersion, tojson(res));

    // This query specifies an explicit readConcern because some FCV tests pass a connection that
    // has manually run isMaster with internalClient, and mongod expects internalClients (ie. other
    // cluster members) to include read/write concern (on commands that accept read/write concern).
    let doc = adminDB.system.version.find({_id: "featureCompatibilityVersion"})
                  .limit(1)
                  .readConcern("local")
                  .next();
    assert.eq(doc.version, version, tojson(doc));
    assert.eq(doc.targetVersion, targetVersion, tojson(doc));
}

/**
 * Since SERVER-29453 disallowed removal of the FCV document, we need to do this hack to remove it.
 */
function removeFCVDocument(adminDB) {
    let res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
    assert.commandWorked(res, "failed to list collections");
    let originalUUID = res.cursor.firstBatch[0].info.uuid;
    let newUUID = UUID();

    // Create new collection with no FCV document, and then delete the
    // original collection.
    let createNewAdminSystemVersionCollection =
        {op: "c", ns: "admin.$cmd", ui: newUUID, o: {create: "system.version"}};
    let dropOriginalAdminSystemVersionCollection =
        {op: "c", ns: "admin.$cmd", ui: originalUUID, o: {drop: "admin.tmp_system_version"}};
    assert.commandWorked(adminDB.runCommand({
        applyOps: [createNewAdminSystemVersionCollection, dropOriginalAdminSystemVersionCollection]
    }));

    res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
    assert.commandWorked(res, "failed to list collections");
    assert.eq(newUUID, res.cursor.firstBatch[0].info.uuid);
}
