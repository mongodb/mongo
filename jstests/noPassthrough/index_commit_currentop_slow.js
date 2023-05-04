/**
 * Confirms slow currentOp logging does not conflict with processing commitIndexBuild, which may
 * block replication.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

const secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true});

// Wait for secondary to start processing commitIndexBuild oplog entry from the primary.
const secondaryDB = secondary.getDB(testDB.getName());
assert.soon(function() {
    const filter = {
        'command.commitIndexBuild': {$exists: true},
        '$all': true,
    };
    const result = assert.commandWorked(secondaryDB.currentOp(filter));
    assert.lte(
        result.inprog.length,
        1,
        'expected at most one commitIndexBuild entry in currentOp() output: ' + tojson(result));
    if (result.inprog.length == 0) {
        return false;
    }
    jsTestLog('Secondary started processing commitIndexBuild: ' + tojson(result));
    return true;
}, 'secondary did not receive commitIndexBuild oplog entry');

jsTestLog('Running currentOp() with slow operation logging.');
// Lower slowms to make currentOp() log slow operation while the secondary is procesing the
// commitIndexBuild oplog entry during oplog application.
const profileResult = assert.commandWorked(secondaryDB.setProfilingLevel(0, {slowms: -1}));
jsTestLog('Configured profiling to always log slow ops: ' + tojson(profileResult));
const currentOpResult = assert.commandWorked(secondaryDB.currentOp());
jsTestLog('currentOp() with slow operation logging: ' + tojson(currentOpResult));
assert.commandWorked(secondaryDB.setProfilingLevel(0, {slowms: 30000}));
jsTestLog('Completed currentOp() with slow operation logging.');

const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

// Wait for the index build to stop.
IndexBuildTest.resumeIndexBuilds(secondary);
IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

// Wait for parallel shell to stop.
createIdx();

rst.stopSet();
})();
