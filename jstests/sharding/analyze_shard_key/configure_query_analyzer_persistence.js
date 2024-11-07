/**
 * Tests that the configureQueryAnalyzer command persists the configuration in a document
 * in config.queryAnalyzers and that the document is deleted when the associated collection
 * is dropped or renamed.
 *
 * @tags: [requires_fcv_70]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testConfigurationDeletionDropCollection,
    testConfigurationDeletionDropDatabase,
    testConfigurationDeletionRenameCollection,
    testPersistingConfiguration
} from "jstests/sharding/analyze_shard_key/libs/configure_query_analyzer_common.js";

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
    const isShardedCluster = true;
    const shardNames = [st.shard0.name, st.shard1.name];

    testPersistingConfiguration(st.s);
    for (let isShardedColl of [true, false]) {
        testConfigurationDeletionDropCollection(st.s,
                                                {isShardedColl, isShardedCluster, shardNames});
        testConfigurationDeletionDropDatabase(st.s, {isShardedColl, isShardedCluster, shardNames});
        testConfigurationDeletionRenameCollection(
            st.s, {sameDatabase: true, isShardedColl, isShardedCluster, shardNames});
    }
    // During renameCollection, the source database is only allowed to be different from the
    // destination database when the collection being renamed is unsharded.
    testConfigurationDeletionRenameCollection(
        st.s, {sameDatabase: false, isShardedColl: false, isShardedCluster, shardNames});

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testPersistingConfiguration(primary);
    testConfigurationDeletionDropCollection(primary, {});
    testConfigurationDeletionDropDatabase(primary, {});
    testConfigurationDeletionRenameCollection(primary, {sameDatabase: false});
    testConfigurationDeletionRenameCollection(primary, {sameDatabase: true});

    rst.stopSet();
}
