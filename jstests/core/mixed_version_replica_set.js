/**
 * Tests initializing a mixed version replica set through resmoke.
 *
 * @tags: [multiversion_sanity_check]
 */

(function() {
"use strict";

const latestBinVersion = MongoRunner.getBinVersionFor("latest");

// TODO: SERVER-50389 Support both lastLTS and lastContinuous.
const lastContiuousBinVersion = MongoRunner.getBinVersionFor("last-continuous");

if (testingReplication && TestData && TestData.mixedBinVersions) {
    const replSetStatus = db.adminCommand({"replSetGetStatus": 1});
    const members = replSetStatus["members"];
    assert.eq(TestData.mixedBinVersions.length, replSetStatus["members"].length);
    for (let i = 0; i < TestData.mixedBinVersions.length; i++) {
        const conn = new Mongo(members[i]["name"]);
        const admin = conn.getDB("admin");
        const serverStatus = admin.serverStatus();
        const actualVersion = serverStatus["version"];
        const expectedVersion =
            TestData.mixedBinVersions[i] === "new" ? latestBinVersion : lastContiuousBinVersion;
        assert(MongoRunner.areBinVersionsTheSame(actualVersion, expectedVersion));
    }
} else {
    jsTestLog(
        "This tests initializing a mixed version replica set through resmoke. Skipping test run" +
        " because testingReplication and TestData.mixedBinVersion are not set.");
}
})();
