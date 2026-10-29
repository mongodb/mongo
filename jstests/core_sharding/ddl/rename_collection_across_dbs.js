/*
 * Tests renameCollection across two databases that share the same primary shard on a sharded
 * cluster. This exercises the renameCollectionAcrossDatabases path and, when run in a stepdown
 * suite, verifies it is resilient to failovers.
 *
 * The scenarios are shared with functional passthrough tests via
 * rename_collection_across_dbs_helpers.js; here they are run in a loop so that, under continuous
 * stepdowns, a failover is likely to interleave with a cross-database rename.
 *
 * @tags: [
 *   # Test assertions require collection placement stability
 *   assumes_balancer_off,
 *   # Cross-database rename requires unsharded source/target collections; this test controls their
 *   # placement explicitly and must not have its collections implicitly sharded.
 *   assumes_unsharded_collection,
 * ]
 */

import {
    runRenameUnsplittableCollectionAcrossDbsScenarios,
    runRenameUntrackedCollectionAcrossDbsScenarios,
    setupSamePrimaryDatabases,
} from "jstests/libs/cluster_helpers/rename_collection_across_dbs_helpers.js";

// Set the databases up once; the scenarios use unique collection names per iteration so repeated
// runs don't collide.
const {sourceDb, targetDb, primaryShard} = setupSamePrimaryDatabases(
    db,
    jsTestName() + "_from",
    jsTestName() + "_to",
);

const kIterations = 10;
for (let i = 0; i < kIterations; ++i) {
    jsTest.log.info("Running cross-database rename scenarios", {iteration: i});
    runRenameUnsplittableCollectionAcrossDbsScenarios(
        db,
        sourceDb,
        targetDb,
        primaryShard,
        "iter" + i,
    );
    runRenameUntrackedCollectionAcrossDbsScenarios(sourceDb, targetDb, "iter" + i);
}
