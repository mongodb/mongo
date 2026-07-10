/**
 * Tests that featureFlagAssignUUIDToShard drives the generation and removal of shard UUID metadata
 * on FCV upgrade/downgrade and addShard.
 * TODO SERVER-126212 Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   featureFlagAssignUUIDToShard,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {assertShardUuidMetadataConsistency} from "jstests/libs/sharded_cluster_topology/shard_uuid_helpers.js";

describe("FCV upgrade/downgrade uuid fields", function () {
    let st;

    before(function () {
        st = new ShardingTest({shards: 0, mongos: 1, rs: {nodes: 1}});
    });

    after(function () {
        st.stop();
    });

    function getMaxTopologyTimeInConfigShards() {
        const result = st.s
            .getDB("config")
            .shards.aggregate([{$group: {_id: null, maxTopologyTime: {$max: "$topologyTime"}}}])
            .toArray();
        return result.length > 0 ? result[0].maxTopologyTime : null;
    }

    function getConfigShardsDoc(shardName) {
        return st.s.getDB("config").shards.findOne({_id: shardName});
    }

    it("FCV transitions set/clear UUID metadata on a dedicated config server", function () {
        // Initial state. On startup at the latest FCV, the config server has a shardIdentity
        // document with its uuid field set and a secondary index on the uuid field of config.shards.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);

        // Downgrade the FCV to the last LTS version and verify that the above metadata have been dropped.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, false /* expectedToExist */);

        // Upgrade the FCV back to latest and verify that the expected metadata have been regenerated.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);
    });

    it("FCV transitions set/clear UUID metadata on an embedded config server", function () {
        // Perform a transition to embedded config server.
        assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

        // Check the existence and consistency of the expected shard UUID metadata.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        // An embedded config server is represented by its own config.shards document.
        assert.neq(null, getConfigShardsDoc("config"));
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);

        // Downgrade the FCV to the last LTS version and verify that the above metadata have been dropped.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, false /* expectedToExist */);

        // Upgrade the FCV back to latest and verify that the expected metadata have been regenerated.
        // Since now the cluster contains a shard, the write of uuid to the config.shards is expected to also bump the topology time.
        const topologyTimeBeforeUpgrade = getMaxTopologyTimeInConfigShards();
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);
        const topologyTimeAfterUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterUpgrade, topologyTimeBeforeUpgrade) > 0,
            "Expected topologyTime to be bumped on FCV upgrade",
        );
        // NOTE: this test leaves the config server in embedded mode (being also the only shard in the cluster, it cannot transition back to dedicated mode).
    });

    it("FCV transitions set/clear UUID metadata on regular shards", function () {
        const replicaSetsByShardName = {"config": st.configRS};

        function addShardToCluster() {
            const shardName = `uuidTestShard${Object.keys(replicaSetsByShardName).length}`;
            const rs = new ReplSetTest({name: shardName, nodes: 1});
            rs.startSet({shardsvr: ""});
            rs.initiate();
            assert.commandWorked(st.s.adminCommand({addShard: rs.getURL(), name: shardName}));
            replicaSetsByShardName[shardName] = rs;
        }

        // Add a first shard to the cluster. The operation is expected to also assign a uuid value.
        addShardToCluster();
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);

        // Downgrade the FCV to the last LTS version and verify that the above metadata have been dropped.
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, false /* expectedToExist */);

        // Add a second shard to the cluster. The operation is now expected not assign a uuid value.
        addShardToCluster();
        assertShardUuidMetadataConsistency(st.s, false /* expectedToExist */);

        // Upgrade the FCV back to latest and verify that consistent UUIDs have been regenerated for each shard in the cluster.
        // (Also, the write on config.shards is expected to also bump the topology time).
        const topologyTimeBeforeUpgrade = getMaxTopologyTimeInConfigShards();
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assertShardUuidMetadataConsistency(st.s, true /* expectedToExist */);
        const topologyTimeAfterUpgrade = getMaxTopologyTimeInConfigShards();
        assert(
            timestampCmp(topologyTimeAfterUpgrade, topologyTimeBeforeUpgrade) > 0,
            "Expected topologyTime to be bumped on FCV upgrade",
        );

        // Restore the initial fixture state.
        for (const [shardName, rs] of Object.entries(replicaSetsByShardName)) {
            if (shardName === "config") {
                continue;
            }
            ShardTransitionUtil.retryOnShardTransitionErrors(() => {
                assert.soon(() => {
                    const res = assert.commandWorked(st.s.adminCommand({removeShard: shardName}));
                    return res.state === "completed";
                });
            });

            rs.stopSet();
        }
    });
});
