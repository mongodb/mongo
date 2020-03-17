/**
 * Tests initializing a mixed version replica set through the shell.
 *
 * @tags: [fix_for_fcv_46]
 */

(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');

const lastStableVersion = "last-stable";
const latestVersion = "latest";

const nodes = {
    0: {binVersion: latestVersion},
    1: {binVersion: lastStableVersion},
    2: {binVersion: lastStableVersion}
};

const rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiate();

const latestBinVersion = MongoRunner.getBinVersionFor(latestVersion);
const lastStableBinVersion = MongoRunner.getBinVersionFor(lastStableVersion);

for (let i = 0; i < rst.nodes.length; i++) {
    const admin = rst.nodes[i].getDB("admin");
    const serverStatus = admin.serverStatus();
    const expectedVersion =
        nodes[i]["binVersion"] === latestVersion ? latestBinVersion : lastStableBinVersion;
    const actualVersion = serverStatus["version"];
    assert(MongoRunner.areBinVersionsTheSame(actualVersion, expectedVersion));
}
rst.stopSet();
})();
