/**
 * If a secondary attempts to initial sync from a primary while there is an index build in progress,
 * the index should not be visible on the secondary until it has processed the commitIndexBuild
 * oplog entry.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_initial_sync.js');

const nodes = [
    {},
    {
        // Disallow elections on secondary.
        rsConfig: {
            priority: 0,
            votes: 0,
        },
    },
];
new IndexInitialSyncTest({nodes: nodes}).run();
})();
