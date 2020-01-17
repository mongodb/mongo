/**
 * If a user attempts to shut down the server using the shutdown command without the force: true
 * option while there is an index build in progress, we should reject the shutdown request.
 * @tags: [requires_replication]
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
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a_1');

// Stop the primary using the shutdown command without {force: true}.
try {
    assert.commandFailedWithCode(primary.adminCommand({shutdown: 1, force: false}),
                                 ErrorCodes.ExceededTimeLimit);
} finally {
    IndexBuildTest.resumeIndexBuilds(primary);
}

IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Two phased index build would resume after stepped down primary node is re-elected.
    IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);
} else {
    // Single-phased ndex build would be aborted by step down triggered by the shutdown command.
    IndexBuildTest.assertIndexes(coll, 1, ['_id_']);
}

// This runs the shutdown command without {force: true} with additional handling for expected
// network errors when the command succeeds.
testDB.shutdownServer();

rst.stopSet();
})();
