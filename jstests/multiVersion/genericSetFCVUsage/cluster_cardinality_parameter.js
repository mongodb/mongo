/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after upgrading.
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkClusterParameter} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

function isRSEndpointEnabled(conn) {
    return FeatureFlagUtil.isPresentAndEnabled(conn, "RSEndpointClusterCardinalityParameter");
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
                shardOptions: {binVersion: oldVersion},

                rsOptions: {binVersion: oldVersion},
                rs: true,
            }
        });

        checkClusterParameter(st.configRS, true);
        checkClusterParameter(st.rs0, true);
        checkClusterParameter(st.rs1, true);

        if (!hasTwoOrMoreShardsPriorToUpgrade) {
            removeShard(st, st.shard1.name);
        }

        if (oldVersion == "last-lts") {
            // In v7.0, the cluster parameter 'hasTwoOrMoreShards' gets set to true when the number
            // of shards goes from 1 to 2 but doesn't get set to false when the number of shards
            // goes down to 1.
            checkClusterParameter(st.configRS, true);
            checkClusterParameter(st.rs0, true);
        }

        jsTest.log("Start upgrading the binaries for the cluster");
        st.upgradeCluster("latest");
        jsTest.log("Finished upgrading the binaries for the cluster");

        if (oldVersion == "last-lts") {
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

        // The feature flag will only be updated on upgrade if the RSEndpoint feature flag is
        // enabled.
        let expectedValue =
            (isRSEndpointEnabled(st.configRS.getPrimary()) || oldVersion == "last-continuous")
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
