/**
 * Tests that we may not use reconfig to remove a custom write concern definition that
 * is currently set to the default. We are free to remove that definition once it is no longer in
 * use.
 */

(function() {
"use strict";
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name: name,
    nodes: [
        {rsConfig: {tags: {region: "us"}}},
        {rsConfig: {tags: {region: "us"}}},
        {rsConfig: {tags: {region: "eu"}}}
    ],
    settings: {getLastErrorModes: {multiRegion: {region: 2}}}
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

jsTestLog("Setting the write concern to match the custom definition.");
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));

let cfg = rst.getReplSetConfigFromNode();
cfg.settings.getLastErrorModes = {};

jsTestLog("Attempting reconfig to remove the definition while it is still in use (should fail).");
assert.throwsWithCode(() => {
    reconfig(rst, cfg);
}, ErrorCodes.NewReplicaSetConfigurationIncompatible);

jsTestLog("Resetting the write concern to majority.");
assert.commandWorked(primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 2}}));

jsTestLog(
    "Removing custom write concern definition now that it is no longer in use (should succceed).");
cfg = rst.getReplSetConfigFromNode();
cfg.settings.getLastErrorModes = {};
reconfig(rst, cfg);

jsTestLog("Checking writeability.");
const coll = primary.getDB("db").getCollection("coll");
assert.commandWorked(coll.insert({a: 1}));
rst.awaitReplication();

rst.stopSet();
})();
