// Tests that manipulating the featureCompatibilityVersion document in admin.system.version changes
// the value of the featureCompatibilityVersion server parameter.

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

let adminDB = conn.getDB("admin");

// Initially the featureCompatibilityVersion is latestFCV.
checkFCV(adminDB, latestFCV);

// Updating the featureCompatibilityVersion document changes the featureCompatibilityVersion
// server parameter.
for (let oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    // Fully downgraded to oldVersion.
    assert.commandWorked(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {version: oldVersion}}),
    );
    checkFCV(adminDB, oldVersion);

    // Upgrading to latest.
    assert.commandWorked(
        adminDB.system.version.update(
            {_id: "featureCompatibilityVersion"},
            {$set: {version: oldVersion, targetVersion: latestFCV}},
        ),
    );
    checkFCV(adminDB, oldVersion, latestFCV);

    // Downgrading to oldVersion.
    assert.commandWorked(
        adminDB.system.version.update(
            {_id: "featureCompatibilityVersion"},
            {$set: {version: oldVersion, targetVersion: oldVersion, previousVersion: latestFCV}},
        ),
    );
    checkFCV(adminDB, oldVersion, oldVersion);

    // When present, "previousVersion" will always be the latestFCV.
    assert.writeErrorWithCode(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {previousVersion: oldVersion}}),
        4926901,
    );
    checkFCV(adminDB, oldVersion, oldVersion);

    // Downgrading FCV must have a 'previousVersion' field.
    assert.writeErrorWithCode(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$unset: {previousVersion: true}}),
        4926902,
    );
    checkFCV(adminDB, oldVersion, oldVersion);

    // Reset to latestFCV.
    assert.commandWorked(
        adminDB.system.version.update(
            {_id: "featureCompatibilityVersion"},
            {$set: {version: latestFCV}, $unset: {targetVersion: true, previousVersion: true}},
        ),
    );
    checkFCV(adminDB, latestFCV);
}

if (lastLTSFCV !== lastContinuousFCV) {
    // Test that we can update from last-lts to last-continuous when the two versions are not equal.
    // This upgrade path is exposed to users through the setFeatureCompatibilityVersion command with
    // fromConfigServer: true.
    assert.commandWorked(
        adminDB.system.version.update(
            {_id: "featureCompatibilityVersion"},
            {$set: {version: lastLTSFCV, targetVersion: lastContinuousFCV}},
        ),
    );
    checkFCV(adminDB, lastLTSFCV, lastContinuousFCV);

    // Reset to latestFCV.
    assert.commandWorked(
        adminDB.system.version.update(
            {_id: "featureCompatibilityVersion"},
            {$set: {version: latestFCV}, $unset: {targetVersion: true, previousVersion: true}},
        ),
    );
    checkFCV(adminDB, latestFCV);
}

// Updating the featureCompatibilityVersion document with an invalid version fails.
assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {version: "3.2"}}),
    4926900,
);
checkFCV(adminDB, latestFCV);

// Updating the featureCompatibilityVersion document with an invalid targetVersion fails.
assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {targetVersion: lastLTSFCV}}),
    4926904,
);
checkFCV(adminDB, latestFCV);

assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {targetVersion: lastContinuousFCV}}),
    4926904,
);
checkFCV(adminDB, latestFCV);

assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {targetVersion: latestFCV}}),
    4926904,
);
checkFCV(adminDB, latestFCV);

// Setting an unknown field.
assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {unknownField: "unknown"}}),
    ErrorCodes.IDLUnknownField,
);
checkFCV(adminDB, latestFCV);

MongoRunner.stopMongod(conn);
