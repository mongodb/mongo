// Tests that manipulating the featureCompatibilityVersion document in admin.system.version changes
// the value of the featureCompatibilityVersion server parameter.

(function() {
"use strict";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

let adminDB = conn.getDB("admin");

// Initially the featureCompatibilityVersion is latestFCV.
checkFCV(adminDB, latestFCV);

// Updating the featureCompatibilityVersion document changes the featureCompatibilityVersion
// server parameter.
assert.commandWorked(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                   {$set: {version: lastStableFCV}}));
checkFCV(adminDB, lastStableFCV);

assert.commandWorked(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                  {$set: {version: lastStableFCV, targetVersion: latestFCV}}));
checkFCV(adminDB, lastStableFCV, latestFCV);

assert.commandWorked(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                  {$set: {version: lastStableFCV, targetVersion: lastStableFCV}}));
checkFCV(adminDB, lastStableFCV, lastStableFCV);

assert.commandWorked(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                  {$set: {version: latestFCV}, $unset: {targetVersion: true}}));
checkFCV(adminDB, latestFCV);

// Updating the featureCompatibilityVersion document with an invalid version fails.
assert.writeErrorWithCode(
    adminDB.system.version.update({_id: "featureCompatibilityVersion"}, {$set: {version: "3.2"}}),
    ErrorCodes.BadValue);
checkFCV(adminDB, latestFCV);

// Updating the featureCompatibilityVersion document with an invalid targetVersion fails.
assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                        {$set: {targetVersion: lastStableFCV}}),
                          ErrorCodes.BadValue);
checkFCV(adminDB, latestFCV);

assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                        {$set: {targetVersion: latestFCV}}),
                          ErrorCodes.BadValue);
checkFCV(adminDB, latestFCV);

MongoRunner.stopMongod(conn);
}());
