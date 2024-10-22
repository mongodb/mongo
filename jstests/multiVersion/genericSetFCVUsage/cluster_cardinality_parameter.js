/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after upgrading.
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkClusterParameter} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

function isRSEndpointEnabled(conn) {
    return FeatureFlagUtil.isPresentAndEnabled(conn, "ReplicaSetEndpoint");
}

function runTest(hasTwoOrMoreShardsPriorToUpgrade) {
    for (let oldVersion of ["last-lts", "last-continuous"]) {
        jsTest.log("Running test for " +
                   tojsononeline({oldVersion, hasTwoOrMoreShardsPriorToUpgrade}));
        const st = new ShardingTest({
            shards: 2,
            other: {
                mongosOptions: {binVersion: oldVersion},
                configOptions: {binVersion: oldVersion},
                rsOptions: {binVersion: oldVersion},
                rs: true,
            },
            // By default, our test infrastructure sets the election timeout to a very high value
            // (24 hours). For this test, we need a shorter election timeout because it relies on
            // nodes running an election when they do not detect an active primary. Therefore, we
            // are setting the electionTimeoutMillis to its default value.
            initiateWithDefaultElectionTimeout: true
        });

        const oldVersionIs73 = MongoRunner.areBinVersionsTheSame("7.3", oldVersion);

        checkClusterParameter(st.configRS, true);
        checkClusterParameter(st.rs0, true);
        checkClusterParameter(st.rs1, true);

        if (!hasTwoOrMoreShardsPriorToUpgrade) {
            removeShard(st, st.shard1.name);
        }

        if (!oldVersionIs73) {
            // In v7.3, the cluster parameter 'hasTwoOrMoreShards' gets reset to 1 when the number
            // of shards goes from 2 to 1. In all other versions (assuming
            // featureFlagReplicaSetEndpoint is disabled), the cluster parameter
            // will not be reset on removeShard.
            checkClusterParameter(st.configRS, true);
            checkClusterParameter(st.rs0, true);
        }

        jsTest.log("Start upgrading the binaries for the cluster");
        st.upgradeCluster("latest");
        jsTest.log("Finished upgrading the binaries for the cluster");

        if (!oldVersionIs73) {
            // In v7.0, the cluster parameter 'hasTwoOrMoreShards' gets set to true when the number
            // of shards goes from 1 to 2 but doesn't get set to false when the number of shards
            // goes down to 1.
            checkClusterParameter(st.configRS, true);
            checkClusterParameter(st.rs0, true);
        }
        if (hasTwoOrMoreShardsPriorToUpgrade) {
            checkClusterParameter(st.rs1, true);
        }

        jsTest.log("Start upgrading the FCV for the cluster");
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        jsTest.log("Finished upgrading the FCV for the cluster");

        // The cluster parameter will only be updated on upgrade if the RSEndpoint feature flag is
        // enabled. The parameter will also have the updated value if the old version is 7.3.
        let expectedValue = (isRSEndpointEnabled(st.configRS.getPrimary()) || oldVersionIs73)
            ? hasTwoOrMoreShardsPriorToUpgrade
            : true;
        checkClusterParameter(st.configRS, expectedValue);
        checkClusterParameter(st.rs0, expectedValue);
        if (hasTwoOrMoreShardsPriorToUpgrade) {
            checkClusterParameter(st.rs1, true);
        }

        st.stop();
    }
}

runTest(false /* hasTwoOrMoreShardsPriorToUpgrade */);
runTest(true /* hasTwoOrMoreShardsPriorToUpgrade */);
