// Tests that when the featureCompatibilityVersion is not equal to the downgrade version, running
// isMaster with internalClient returns a response with minWireVersion == maxWireVersion. This
// ensures that an older version mongod/mongos will fail to connect to the node when it is upgraded,
// upgrading, or downgrading.
//
(function() {
"use strict";

const adminDB = db.getSiblingDB("admin");

// This test manually runs isMaster with internalClient, which means that to the mongod, the
// connection appears to be from another server. Since mongod expects other cluster members to
// always include explicit read/write concern (on commands that accept read/write concern), this
// test must be careful to mimic this behavior.
const isMasterCommand = {
    isMaster: 1,
    internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
};

// When the featureCompatibilityVersion is equal to the upgrade version, running isMaster with
// internalClient returns minWireVersion == maxWireVersion.
checkFCV(adminDB, latestFCV);
let res = adminDB.runCommand(isMasterCommand);
assert.commandWorked(res);
assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));

// When the featureCompatibilityVersion is upgrading, running isMaster with internalClient
// returns minWireVersion == maxWireVersion.
assert.commandWorked(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                  {$set: {version: lastStableFCV, targetVersion: latestFCV}},
                                  {writeConcern: {w: 1}}));
res = adminDB.runCommand(isMasterCommand);
assert.commandWorked(res);
assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));

// When the featureCompatibilityVersion is downgrading, running isMaster with internalClient
// returns minWireVersion == maxWireVersion.
assert.commandWorked(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                  {$set: {version: lastStableFCV, targetVersion: lastStableFCV}},
                                  {writeConcern: {w: 1}}));
res = adminDB.runCommand(isMasterCommand);
assert.commandWorked(res);
assert.eq(res.minWireVersion, res.maxWireVersion, tojson(res));

// When the featureCompatibilityVersion is equal to the downgrade version, running isMaster with
// internalClient returns minWireVersion + 1 == maxWireVersion.
assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV, writeConcern: {w: 1}}));
res = adminDB.runCommand(isMasterCommand);
assert.commandWorked(res);
assert.eq(res.minWireVersion + 1, res.maxWireVersion, tojson(res));

// When the internalClient field is missing from the isMaster command, the response returns the
// full wire version range from minWireVersion == 0 to maxWireVersion == latest version, even if
// the featureCompatibilityVersion is equal to the upgrade version.
assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, writeConcern: {w: 1}}));
res = adminDB.runCommand({isMaster: 1});
assert.commandWorked(res);
assert.eq(res.minWireVersion, 0, tojson(res));
assert.lt(res.minWireVersion, res.maxWireVersion, tojson(res));
})();
