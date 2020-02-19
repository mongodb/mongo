/**
 * If a secondary attempts to initial sync from a 4.2 primary while there is an index build in
 * progress, the index should be visible on the secondary.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_initial_sync.js');

const nodes = [
    {
        binVersion: 'last-stable',
    },
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
