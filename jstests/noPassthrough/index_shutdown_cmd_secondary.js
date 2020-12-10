/**
 * If a user attempts to shut down the server using the shutdown command without the force: true
 * option while there is an index build in progress, we should reject the shutdown request.
 * @tags: [
 *   requires_replication,
 *   sbe_incompatible,
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

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), 'a_1');

// Stop the secondary using the shutdown command without {force: true}.
try {
    assert.commandFailedWithCode(secondary.adminCommand({shutdown: 1, force: false}),
                                 ErrorCodes.ConflictingOperationInProgress);
} finally {
    IndexBuildTest.resumeIndexBuilds(secondary);
}

IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

createIdx();

IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

// This runs the shutdown command without {force: true} with additional handling for expected
// network errors when the command succeeds.
secondaryDB.shutdownServer();

rst.stopSet();
})();
