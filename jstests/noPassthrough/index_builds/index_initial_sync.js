/**
 * If a secondary attempts to initial sync from a primary while there is an index build in progress,
 * the index should not be visible on the secondary until it has processed the commitIndexBuild
 * oplog entry.
 * @tags: [
 *   # TODO(SERVER-109667): Primary-driven index builds don't support draining side writes yet.
 *   primary_driven_index_builds_incompatible,
 *   requires_replication,
 * ]
 */
import {IndexInitialSyncTest} from "jstests/noPassthrough/libs/index_builds/index_initial_sync.js";

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
