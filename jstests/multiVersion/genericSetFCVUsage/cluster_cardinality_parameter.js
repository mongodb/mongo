/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after upgrading.
 */

import "jstests/multiVersion/libs/multi_cluster.js";
import {checkClusterParameter} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

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

        checkClusterParameter(st.configRS, true);
        checkClusterParameter(st.rs0, true);

        jsTest.log("Start upgrading the binaries for the cluster");
        st.upgradeCluster("latest");
        jsTest.log("Finished upgrading the binaries for the cluster");

        checkClusterParameter(st.configRS, true);
        checkClusterParameter(st.rs0, true);
        if (hasTwoOrMoreShardsPriorToUpgrade) {
            checkClusterParameter(st.rs1, true);
        }

        jsTest.log("Start upgrading the FCV for the cluster");
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        jsTest.log("Finished upgrading the FCV for the cluster");

        checkClusterParameter(st.configRS, hasTwoOrMoreShardsPriorToUpgrade);
        checkClusterParameter(st.rs0, hasTwoOrMoreShardsPriorToUpgrade);
        if (hasTwoOrMoreShardsPriorToUpgrade) {
            checkClusterParameter(st.rs1, true);
        }

        st.stop();
    }
}

runTest(false /* hasTwoOrMoreShardsPriorToUpgrade */);
runTest(true /* hasTwoOrMoreShardsPriorToUpgrade */);
