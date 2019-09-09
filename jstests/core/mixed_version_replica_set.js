/**
 * Tests initializing a mixed version replica set through resmoke.
 *
 */

(function() {
"use strict";

const latestBinVersion = MongoRunner.getBinVersionFor("latest");
const lastStableBinVersion = MongoRunner.getBinVersionFor("last-stable");

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
            TestData.mixedBinVersions[i] === "new" ? latestBinVersion : lastStableBinVersion;
        assert(MongoRunner.areBinVersionsTheSame(actualVersion, expectedVersion));
    }
} else {
    jsTestLog(
        "This tests initializing a mixed version replica set through resmoke. Skipping test run" +
        " because testingReplication and TestData.mixedBinVersion are not set.");
}
})();
