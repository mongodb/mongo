/**
 * Tests initializing a mixed version replica set through the shell.
 */

(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');

const lastLTSVersion = "last-lts";
const latestVersion = "latest";

const nodes = {
    0: {binVersion: latestVersion},
    1: {binVersion: lastLTSVersion},
    2: {binVersion: lastLTSVersion}
};

const rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiate();

const latestBinVersion = MongoRunner.getBinVersionFor(latestVersion);
const lastLTSBinVersion = MongoRunner.getBinVersionFor(lastLTSVersion);

for (let i = 0; i < rst.nodes.length; i++) {
    const admin = rst.nodes[i].getDB("admin");
    const serverStatus = admin.serverStatus();
    const expectedVersion =
        nodes[i]["binVersion"] === latestVersion ? latestBinVersion : lastLTSBinVersion;
    const actualVersion = serverStatus["version"];
    assert(MongoRunner.areBinVersionsTheSame(actualVersion, expectedVersion));
}
rst.stopSet();
})();
