/**
 * IPs cannot be used as hostnames for split horizon configurations; replSetInitiate will not work
 * correctly. If previously configured using IP, mongod will still be allowed to start
 * @tags: [ requires_persistence ]
 */

(function() {
"use strict";

// Start and configure mongod with invalid split horizons using override setting

let startupConfig = {
    replSet: "test",
    startClean: true,
    dbpath: MongoRunner.dataPath + 'split-hz-test',
    setParameter: {disableSplitHorizonIPCheck: true}
};

let mongod = MongoRunner.runMongod(startupConfig);
assert(mongod);

const replConfig = {
    _id: "test",
    members: [{_id: 0, host: "127.0.0.1:" + mongod.port, horizons: {horizon_name: "127.0.0.0/20"}}]
};

// Make sure replSetInitiate works

let output = mongod.adminCommand({replSetInitiate: replConfig});
jsTestLog("Command result: " + tojson(output));
assert.commandWorked(output);

let rsConfig1 = mongod.adminCommand({replSetGetConfig: 1});
jsTestLog("rsConfig1: " + tojson(rsConfig1));
assert.commandWorked(rsConfig1);

MongoRunner.stopMongod(mongod);

// Restart mongod without override setting and see that it will still work because it was previously
// configured

startupConfig.startClean = false;
startupConfig.restart = true;
startupConfig.noCleanData = true;
startupConfig.setParameter.disableSplitHorizonIPCheck = false;
startupConfig.port = mongod.port;

mongod = MongoRunner.runMongod(startupConfig);
assert(mongod);

let rsConfig2;
assert.soon(() => {
    rsConfig2 = mongod.adminCommand({replSetGetConfig: 1});
    return rsConfig2.ok;
}, "Failed to get replset config from node");
jsTestLog("rsConfig2: " + tojson(rsConfig2));
assert.commandWorked(rsConfig2);
assert.eq(tojson(rsConfig2.config.members), tojson(rsConfig1.config.members));

// Verify that the node can accept writes before running the reconfig.

assert.soon(() => mongod.adminCommand({isMaster: 1}).ismaster, "Node failed to become a master");

// Make sure that configuration can not be applied manually as it is invalid and no override is
// specified

output = mongod.adminCommand({replSetReconfig: rsConfig1.config});
jsTestLog("replSetReconfig output: " + tojson(output));
assert.commandFailed(output);
assert(output.errmsg.includes("Found split horizon configuration using IP"));

MongoRunner.stopMongod(mongod);
}());
