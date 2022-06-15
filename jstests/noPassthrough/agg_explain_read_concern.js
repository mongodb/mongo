/**
 * Test that explained aggregation commands behave correctly with the readConcern option.
 * @tags: [requires_majority_read_concern]
 */
(function() {
"use strict";

const rst = new ReplSetTest(
    {name: "aggExplainReadConcernSet", nodes: 1, nodeOptions: {enableMajorityReadConcern: ""}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const session = primary.getDB("test").getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase("test");
const coll = sessionDB.agg_explain_read_concern;

// Test that explain is legal with readConcern "local".
assert.commandWorked(coll.explain().aggregate([], {readConcern: {level: "local"}}));
assert.commandWorked(sessionDB.runCommand(
    {aggregate: coll.getName(), pipeline: [], explain: true, readConcern: {level: "local"}}));
assert.commandWorked(sessionDB.runCommand({
    explain: {aggregate: coll.getName(), pipeline: [], cursor: {}},
    readConcern: {level: "local"}
}));

// Test that explain is illegal with other readConcern levels.
const nonLocalReadConcerns = ["majority", "available", "linearizable"];
nonLocalReadConcerns.forEach(function(readConcernLevel) {
    let aggCmd = {
        aggregate: coll.getName(),
        pipeline: [],
        explain: true,
        readConcern: {level: readConcernLevel}
    };
    let explainCmd = {
        explain: {aggregate: coll.getName(), pipeline: [], cursor: {}},
        readConcern: {level: readConcernLevel}
    };

    assert.throws(() => coll.explain().aggregate([], {readConcern: {level: readConcernLevel}}));

    let cmdRes = sessionDB.runCommand(aggCmd);
    assert.commandFailedWithCode(cmdRes, ErrorCodes.InvalidOptions, tojson(cmdRes));
    let expectedErrStr = "aggregate command cannot run with a readConcern other than 'local'";
    assert.neq(cmdRes.errmsg.indexOf(expectedErrStr), -1, tojson(cmdRes));

    cmdRes = sessionDB.runCommand(explainCmd);
    assert.commandFailedWithCode(cmdRes, ErrorCodes.InvalidOptions, tojson(cmdRes));
    expectedErrStr = "read concern not supported";
    assert.neq(cmdRes.errmsg.indexOf(expectedErrStr), -1, tojson(cmdRes));
});

session.endSession();
rst.stopSet();
}());
