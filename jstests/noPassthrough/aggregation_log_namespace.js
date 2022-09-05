// Tests that a source collection namespace is correctly logged in the global log for an aggregate
// command when a pipeline contains a stage that can write into an output collection.
// @tags: [requires_profiling]
(function() {
'use strict';

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachKindOfWriteStage.

// Runs the given 'pipeline' and verifies that the namespace is correctly logged in the global
// log for the aggregate command. The 'comment' parameter is used to match a log entry against
// the aggregate command.
function verifyLoggedNamespace({pipeline, comment}) {
    assert.commandWorked(db.runCommand(
        {aggregate: source.getName(), comment: comment, pipeline: pipeline, cursor: {}}));
    checkLog.containsWithCount(
        conn,
        `"appName":"MongoDB Shell",` +
            `"command":{"aggregate":"${source.getName()}","comment":"${comment}"`,
        1);
}

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);
assert.neq(null, conn, `mongod failed to start with options ${tojson(mongodOptions)}`);

const db = conn.getDB(`${jsTest.name()}_db`);
const source = db.getCollection(`${jsTest.name()}_source`);
source.drop();
const target = db.getCollection(`${jsTest.name()}_target`);
target.drop();

// Make sure each command gets logged.
assert.commandWorked(db.setProfilingLevel(1, {slowms: 0}));

// Test stages that can write into an output collection.
withEachKindOfWriteStage(
    target, (stage) => verifyLoggedNamespace({pipeline: [stage], comment: Object.keys(stage)[0]}));

// Test each $merge mode.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => verifyLoggedNamespace({
                      pipeline: [{
                          $merge: {
                              into: target.getName(),
                              whenMatched: whenMatchedMode,
                              whenNotMatched: whenNotMatchedMode
                          }
                      }],
                      comment: `merge_${whenMatchedMode}_${whenNotMatchedMode}`
                  }));

MongoRunner.stopMongod(conn);
})();
