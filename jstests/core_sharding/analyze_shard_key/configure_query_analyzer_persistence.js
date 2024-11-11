/**
 * Tests that the configureQueryAnalyzer command persists the configuration in a document
 * in config.queryAnalyzers and that the document is deleted when the associated collection
 * is dropped or renamed.
 *
 * @tags: [
 *   requires_fcv_70,
 *   # Balancer will perform random moveCollections that change the collUuid.
 *   assumes_balancer_off,
 *   # Stepdown test coverage is already provided by the analyze shard key FSM suites.
 *   does_not_support_stepdowns,
 * ]
 */
import {
    testConfigurationDeletionDropCollection,
    testConfigurationDeletionDropDatabase,
    testConfigurationDeletionRenameCollection,
    testPersistingConfiguration
} from "jstests/sharding/analyze_shard_key/libs/configure_query_analyzer_common.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";

const mongos = db.getMongo();
const isShardedCluster = true;
const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("Exiting early as this test requires at least two shards.");
    quit();
}

testPersistingConfiguration(mongos);
for (let isShardedColl of [true, false]) {
    testConfigurationDeletionDropCollection(mongos, {isShardedColl, shardNames, isShardedCluster});
    testConfigurationDeletionDropDatabase(mongos, {isShardedColl, shardNames, isShardedCluster});
    testConfigurationDeletionRenameCollection(
        mongos, {sameDatabase: true, isShardedColl, shardNames, isShardedCluster});
}
// During renameCollection, the source database is only allowed to be different from the
// destination database when the collection being renamed is unsharded.
testConfigurationDeletionRenameCollection(
    mongos, {sameDatabase: false, isShardedColl: false, isShardedCluster, shardNames});
