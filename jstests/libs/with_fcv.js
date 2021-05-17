/**
 * Utility to run a function with the feature compatibility version is set to a particular value.
 *
 * The feature compatibility version is temporarily modified while the function is executing.
 */
"use strict";

load("jstests/libs/discover_topology.js");  // For Topology and DiscoverTopology.

var {withFCV} = (function() {
    function getAllPrimaries(conn) {
        const topology = DiscoverTopology.findConnectedNodes(conn);
        const hostList = [];

        if (topology.type === Topology.kStandalone) {
            hostList.push(topology.mongod);
        } else if (topology.type === Topology.kReplicaSet) {
            hostList.push(topology.primary);
        } else if (topology.type === Topology.kShardedCluster) {
            hostList.push(topology.configsvr.primary);

            for (let shardName of Object.keys(topology.shards)) {
                const shard = topology.shards[shardName];

                if (shard.type === Topology.kStandalone) {
                    hostList.push(shard.mongod);
                } else if (shard.type === Topology.kReplicaSet) {
                    hostList.push(shard.primary);
                } else {
                    throw new Error("Unrecognized topology format: " + tojson(topology));
                }
            }
        } else {
            throw new Error("Unrecognized topology format: " + tojson(topology));
        }

        return hostList;
    }

    function withFCV(conn, targetFCV, callback) {
        const adminDB = conn.getDB("admin");

        // Running the setFeatureCompatibilityVersion command may implicitly involve running a
        // multi-statement transaction. We temporarily raise the transactionLifetimeLimitSeconds to
        // be 24 hours to avoid spurious failures from it having been set to a lower value.
        const hostList = getAllPrimaries(conn);
        const originalTransactionLifetimeLimitSeconds = hostList.map(hostStr => {
            const hostConn = new Mongo(hostStr);
            const res = assert.commandWorked(hostConn.adminCommand(
                {setParameter: 1, transactionLifetimeLimitSeconds: ReplSetTest.kForeverSecs}));
            return {conn: hostConn, originalValue: res.was};
        });

        // We explicitly specify a read concern to handle when the cluster-wide default read concern
        // has been set to something nonsensical.
        const originalFCV = adminDB.system.version.find({_id: "featureCompatibilityVersion"})
                                .limit(-1)
                                .readConcern("local")
                                .next();

        const runSetFCV = (version) => {
            // We explicitly specify a write concern to handle when the cluster-wide default write
            // concern has been set to something nonsensical.
            assert.commandWorked(adminDB.runCommand(
                {setFeatureCompatibilityVersion: version, writeConcern: {w: "majority"}}));
        };

        if (originalFCV.targetVersion !== undefined) {
            // If there is a "targetVersion" property in the feature compatibility version document,
            // then a previous FCV upgrade or downgrade was interrupted. We run the
            // setFeatureCompatibilityVersion command to complete the interrupted upgrade or
            // downgrade before attempting to set the feature compatibility version to `targetFCV`.
            runSetFCV(originalFCV.targetVersion);
            checkFCV(adminDB, originalFCV.targetVersion);
        }

        // We are now guaranteed no FCV upgrade or downgrade is in progress. We run the
        // setFeatureCompatibilityVersion command to ensure the feature compatibility version is set
        // to `targetFCV`.
        runSetFCV(targetFCV);

        const ret = callback();

        if (originalFCV.version !== targetFCV) {
            // Restore the original feature compatibility version.
            runSetFCV(originalFCV.version);
        }

        // Restore the original transactionLifetimeLimitSeconds setting.
        for (let {conn: hostConn, originalValue} of originalTransactionLifetimeLimitSeconds) {
            assert.commandWorked(hostConn.adminCommand(
                {setParameter: 1, transactionLifetimeLimitSeconds: originalValue}));
        }

        return ret;
    }

    return {withFCV};
})();
