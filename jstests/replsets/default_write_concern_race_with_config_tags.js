/**
 * Test that default write concern changes are always synchronized with condig write concern tag
 * changes. In this case the reconfig command would win the potential race.
 */

(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/write_concern_util.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name: name,
    nodes: [
        {rsConfig: {tags: {region: "us", category: "A"}}},
        {rsConfig: {tags: {region: "us", category: "B"}}},
        {rsConfig: {tags: {region: "eu", category: "A"}}}
    ],
    settings: {getLastErrorModes: {multiRegion: {region: 2}}}
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

jsTestLog("Setting up reconfig thread.");
let hangDuringReconfigFP = configureFailPoint(primary, "hangBeforeNewConfigValidationChecks");
let cfg = rst.getReplSetConfigFromNode();
cfg.version += 1;
cfg.settings.getLastErrorModes = {
    multiRegion: {region: 2},
    multiCategory: {category: 2}
};

function runReconfigFn(cfg) {
    jsTestLog("Running reconfig");
    assert.commandWorked(
        db.adminCommand({replSetReconfig: cfg, maxTimeMS: ReplSetTest.kDefaultTimeoutMS}));
    jsTestLog("Finished running reconfig");
}

jsTestLog("Waiting for reconfig thread to hit failpoint.");
let waitForReconfig = startParallelShell(funWithArgs(runReconfigFn, cfg), primary.port);
hangDuringReconfigFP.wait();

jsTestLog("Attempting setDefaultRWC (should fail).");
assert.commandFailedWithCode(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}),
    ErrorCodes.ConfigurationInProgress);

jsTestLog("Allowing reconfig to finish.");
hangDuringReconfigFP.off();
waitForReconfig();

jsTestLog("Attempting setDefaultRWC again (should succeed).");
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));

jsTestLog("Checking writeability.");
const coll = primary.getDB("db").getCollection("coll");
assert.commandWorked(coll.insert({a: 1}));
rst.awaitReplication();

rst.stopSet();
})();
