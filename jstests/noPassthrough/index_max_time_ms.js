/**
 * Confirms that index builds on a primary are aborted when the createIndexes operation times out
 * (based on expiration derived from maxTimeMS).
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

const createIndexesCmd = {
    createIndexes: coll.getName(),
    indexes: [
        {
            name: 'a_1',
            key: {a: 1},
        },
    ],
    // This timeout value should be long enough for this test to locate the index build in the
    // db.currentOp() output.
    maxTimeMS: 10000,
};

const createIdx =
    startParallelShell('assert.commandFailedWithCode(db.getSiblingDB("' + testDB.getName() +
                           '").runCommand(' + tojson(createIndexesCmd) + '), ' +
                           'ErrorCodes.MaxTimeMSExpired' +
                           ');',
                       primary.port);

try {
    IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

    // Index build will be interrupted when the createIndexes command times out.
    IndexBuildTest.waitForIndexBuildToStop(testDB);
} finally {
    IndexBuildTest.resumeIndexBuilds(primary);
}

createIdx();

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

rst.stopSet();
})();
