/**
 * Test that config write concern tag changes are always synchronized with default write concern
 * changes. In this case the reconfig command would win the potential race.
 */

(function() {
"use strict";
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

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

jsTestLog("Setting up setDefaultRWC thread.");
let hangDuringSetRWCFP = configureFailPoint(primary, "hangWhileSettingDefaultRWC");

let waitForSetDefaultRWC = startParallelShell(function() {
    jsTestLog("Begin setDefaultRWC.");
    assert.commandWorked(
        db.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "multiRegion"}}));
    jsTestLog("End setDefaultRWC.");
}, primary.port);

jsTestLog("Waiting for setDefaultRWC thread to hit failpoint.");
hangDuringSetRWCFP.wait();

jsTestLog("Attempting a non-WC tag changing reconfig which should not be affected.");
let cfg = rst.getReplSetConfigFromNode();
cfg.settings.catchUpTimeoutMillis = 99999;  // unrelated setting
reconfig(rst, cfg);

jsTestLog("Attempting WC tag-changing reconfig (should fail).");
cfg = rst.getReplSetConfigFromNode();
cfg.settings.getLastErrorModes = {
    multiRegion: {region: 2},
    multiCategory: {category: 2}
};
assert.throwsWithCode(() => {
    reconfig(rst, cfg);
}, ErrorCodes.ConflictingOperationInProgress);

jsTestLog("Allowing setDefaultRWC to finish.");
hangDuringSetRWCFP.off();
waitForSetDefaultRWC();

jsTestLog("Attempting reconfig again (should succeed).");
reconfig(rst, cfg);

jsTestLog("Checking writeability.");
const coll = primary.getDB("db").getCollection("coll");
assert.commandWorked(coll.insert({a: 1}));
rst.awaitReplication();

rst.stopSet();
})();
