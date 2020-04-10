/**
 * Sets up a test for initial sync when there are index builds in progress on the primary.
 */
var IndexInitialSyncTest = function(options) {
    "use strict";

    load('jstests/noPassthrough/libs/index_build.js');

    if (!(this instanceof IndexInitialSyncTest)) {
        return new IndexInitialSyncTest(options);
    }

    // Capture the 'this' reference
    var self = this;

    self.options = options;

    /**
     * Runs the test.
     */
    this.run = function() {
        const options = this.options;

        jsTestLog("IndexInitialSyncTest - " + tojson(options));

        assert.eq(2, options.nodes.length);

        const rst = new ReplSetTest({nodes: options.nodes});
        const nodes = rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const testDB = primary.getDB('test');
        const coll = testDB.getCollection('test');

        assert.commandWorked(coll.insert({a: 1}));

        IndexBuildTest.pauseIndexBuilds(primary);

        const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
        IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

        // Restart the secondary with a clean data directory to start the initial sync process.
        const secondary = rst.restart(1, {
            startClean: true,
        });

        // Wait for the secondary to complete initial sync and transition to SECONDARY state.
        rst.awaitReplication();
        rst.awaitSecondaryNodes();

        // Ensure that the index on the secondary is in an unfinished state.
        const secondaryDB = secondary.getDB(testDB.getName());
        const secondaryColl = secondaryDB.getCollection(coll.getName());
        try {
            if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
                IndexBuildTest.assertIndexes(
                    secondaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true});
            } else {
                IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);
            }
        } finally {
            IndexBuildTest.resumeIndexBuilds(primary);
        }

        IndexBuildTest.waitForIndexBuildToStop(testDB);

        createIdx();

        IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

        rst.awaitReplication();
        IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

        rst.stopSet();
    };
};
