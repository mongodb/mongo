// Tests that when the featureCompatibilityVersion is not equal to the downgrade version, running
// isMaster with internalClient returns a response with minWireVersion == maxWireVersion. This
// ensures that an older version mongod/mongos will fail to connect to the node when it is upgraded,
// upgrading, or downgrading.
//
(function() {
"use strict";

const adminDB = db.getSiblingDB("admin");

// This test manually runs isMaster with the 'internalClient' field, which means that to the
// mongod, the connection appears to be from another server. This makes mongod to return an
// isMaster response with a real 'minWireVersion' for internal clients instead of 0.
//
// The value of 'internalClient.maxWireVersion' in the isMaster command does not matter for the
// purpose of this test and the isMaster command will succeed regardless because this is run
// through the shell and the shell is always compatible talking to the server. In reality
// though, a real internal client with a lower binary version is expected to hang up immediately
// after receiving the response to the isMaster command from a latest server with an upgraded FCV.
//
// And we need to use a side connection to do so in order to prevent the test connection from
// being closed on FCV changes.
function isMasterAsInternalClient() {
    const isMasterCommand = {
        isMaster: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
    };
    const connInternal = new Mongo(adminDB.getMongo().host);
    const res = assert.commandWorked(connInternal.adminCommand(isMasterCommand));
    connInternal.close();
    return res;
}

// When the featureCompatibilityVersion is equal to the upgrade version, running isMaster with
// internalClient returns a response with minWireVersion == maxWireVersion.
checkFCV(adminDB, latestFCV);
let res = isMasterAsInternalClient();
assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));

// Test wire version for upgrade/downgrade.
function runTest(downgradeFCV, downgradeWireVersion, maxWireVersion) {
    // When the featureCompatibilityVersion is upgrading, running isMaster with internalClient
    // returns a response with minWireVersion == maxWireVersion.
    assert.commandWorked(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                      {$set: {version: downgradeFCV, targetVersion: latestFCV}}));
    let res = isMasterAsInternalClient();
    assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));
    assert.eq(maxWireVersion, res.maxWireVersion, tojson(res));

    // When the featureCompatibilityVersion is downgrading, running isMaster with internalClient
    // returns a response with minWireVersion == maxWireVersion.
    assert.commandWorked(adminDB.system.version.update(
        {_id: "featureCompatibilityVersion"},
        {$set: {version: downgradeFCV, targetVersion: downgradeFCV, previousVersion: latestFCV}}));
    res = isMasterAsInternalClient();
    assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));
    assert.eq(maxWireVersion, res.maxWireVersion, tojson(res));

    // When the featureCompatibilityVersion is equal to the downgrade version, running isMaster with
    // internalClient returns a response with minWireVersion + 1 == maxWireVersion.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    res = isMasterAsInternalClient();
    assert.eq(downgradeWireVersion, res.minWireVersion, tojson(res));
    assert.eq(maxWireVersion, res.maxWireVersion, tojson(res));

    // When the internalClient field is missing from the isMaster command, the response returns the
    // full wire version range from minWireVersion == 0 to maxWireVersion == latest version, even if
    // the featureCompatibilityVersion is equal to the upgrade version.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    res = adminDB.runCommand({isMaster: 1});
    assert.commandWorked(res);
    assert.eq(res.minWireVersion, 0, tojson(res));
    assert.lt(res.minWireVersion, res.maxWireVersion, tojson(res));
    assert.eq(maxWireVersion, res.maxWireVersion, tojson(res));
}

// Test upgrade/downgrade between 'latest' and 'last-continuous' if 'last-continuous' is not
// 'last-lts'.
if (lastContinuousFCV !== lastLTSFCV) {
    runTest(lastContinuousFCV, res.maxWireVersion - 1, res.maxWireVersion);
}

// Test upgrade/downgrade between 'latest' and 'last-lts'.
runTest(lastLTSFCV, res.maxWireVersion - numVersionsSinceLastLTS, res.maxWireVersion);
})();
