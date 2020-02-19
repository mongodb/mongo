/**
 * If a 4.2 secondary attempts to initial sync from a primary while there is an index build in
 * progress, the index should be visible on the secondary.
 * @tags: [requires_replication]
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
        binVersion: 'last-stable',
    },
];
new IndexInitialSyncTest({nodes: nodes}).run();
})();
