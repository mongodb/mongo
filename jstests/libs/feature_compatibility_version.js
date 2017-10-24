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