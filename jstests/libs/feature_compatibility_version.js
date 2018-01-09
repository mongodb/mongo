// Contains helpers for checking the featureCompatibilityVersion.

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

    let doc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    assert.eq(doc.version, version, tojson(doc));
    assert.eq(doc.targetVersion, targetVersion, tojson(doc));
}

/**
 * Checks the featureCompatibilityVersion document and server parameter for a 3.4 binary. In 3.4,
 * the featureCompatibilityVersion document is of the form {_id: "featureCompatibilityVersion",
 * version: <value>}. The getParameter result is of the form {featureCompatibilityVersion: <value>,
 * ok: 1}.
 */
function checkFCV34(adminDB, version) {
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, version, tojson(res));

    let doc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    assert.eq(doc.version, version, tojson(doc));
}

/**
 * Since SERVER-29453 disallows us to remove the FCV document in 3.6, we need to
 * do this hack to remove it. Notice this is only for 3.6. For 3.4, we can
 * simply remove the FCV document.
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
        applyOps:
            [createNewAdminSystemVersionCollection, dropOriginalAdminSystemVersionCollection]
    }));

    res = adminDB.runCommand({listCollections: 1, filter: {name: "system.version"}});
    assert.commandWorked(res, "failed to list collections");
    assert.eq(newUUID, res.cursor.firstBatch[0].info.uuid);
}
