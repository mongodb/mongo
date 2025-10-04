/**
 * This file contains helpers for testing sharding state with various operations in the system.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

export var ShardingStateTest = (function () {
    /**
     * Adds a node to the given shard or config server replica set. Also waits for the node
     * to become a steady-state secondary.
     *
     * @param {Object} replSet the ReplSetTest instance for the corresponding replica set
     * @param {bool} elect whether to automatically elect the new node
     * @param {String} serverTypeFlag startup flag for sharding members ["shardsvr"|"configsvr"]
     * @param {Object} newNodeParams setParameter-s to start the node with
     *
     * @returns a connection to the new node added
     */
    function addReplSetNode({replSet, serverTypeFlag, newNodeParams = {}}) {
        jsTestLog("[ShardingStateTest] Adding new member to replica set.");

        const addParams = {setParameter: newNodeParams};

        if (serverTypeFlag !== undefined) {
            addParams[serverTypeFlag] = "";
        }

        const newNode = replSet.add(addParams);

        replSet.reInitiate();
        replSet.awaitSecondaryNodes(null, [newNode]);
        replSet.waitForAllNewlyAddedRemovals();

        return newNode;
    }

    /**
     * Forces a node to go into startup recovery.
     *
     * @param {Object} replSet the ReplSetTest instance for the corresponding replica set
     * @param {Object} node the ReplSetTest instance for the corresponding replica set
     * @param {bool} elect whether to automatically elect the new node
     * @param {Object} startupParams setParameter-s to restart the node with
     *
     * @returns a connection to the restarted node
     */
    function putNodeInStartupRecovery({replSet, node, startupParams = {}}) {
        const dbName = "testDB";
        const collName = "testColl";

        const primary = replSet.getPrimary();

        const testDB = primary.getDB(dbName);
        const testColl = testDB.getCollection(collName);

        jsTestLog("[ShardingStateTest] Adding a write to pin the stable timestamp at.");

        const ts = assert.commandWorked(
            testDB.runCommand({insert: testColl.getName(), documents: [{a: 1}]}),
        ).operationTime;

        configureFailPoint(node, "holdStableTimestampAtSpecificTimestamp", {timestamp: ts});

        jsTestLog("[ShardingStateTest] Adding more data before restarting member.");
        assert.commandWorked(testColl.insert([{b: 2}, {c: 3}]));
        replSet.awaitReplication();

        jsTestLog("[ShardingStateTest] Restarting node. It should go into startup recovery.");
        replSet.restart(node, {setParameter: startupParams});
        replSet.waitForState(node, ReplSetTest.State.PRIMARY);

        return node;
    }

    /**
     * Internal function to failover to a new primary. Waits for the election to complete
     * and for the rest of the set to acknowledge the new primary.
     *
     * @param {Object} replSet replica set to operate on
     * @param {Object} node an electable target
     */
    function failoverToMember(replSet, node) {
        jsTestLog("[ShardingStateTest] Electing new primary: " + node.host);
        assert.soonNoExcept(function () {
            assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
            return true;
        });

        assert.eq(node, replSet.awaitNodesAgreeOnWriteablePrimary(node /* expected primary node */));
    }

    /**
     * Performs sharding state checks.
     *
     * @param {Object} st the sharded cluster to perform checks on
     */
    function checkShardingState(st) {
        jsTestLog("[ShardingStateTest] Performing sharding state checks.");

        const mongos = st.s;
        const dbConn = mongos.getDB("sstDB");

        jsTestLog("[ShardingStateTest] Check 1: Write to unsharded collection.");
        assert.commandWorked(dbConn.getCollection("sstColl").insert({"sstDoc": 1}));

        jsTestLog("[ShardingStateTest] Check 2: Query hashed collection.");

        const shards = assert.commandWorked(mongos.adminCommand("listShards")).shards;

        assert.commandWorked(dbConn.getCollection("sstHashedColl").createIndex({sstKey: "hashed"}));
        assert.commandWorked(
            mongos.adminCommand({
                shardCollection: "sstDB.sstHashedColl",
                key: {sstKey: "hashed"},
                numInitialChunks: shards.length,
            }),
        );

        assert.eq(0, dbConn.getCollection("sstHashedColl").find().itcount());

        jsTestLog("[ShardingStateTest] Sharding state checks succeeded.");
    }

    return {
        addReplSetNode: addReplSetNode,
        putNodeInStartupRecovery: putNodeInStartupRecovery,
        failoverToMember: failoverToMember,
        checkShardingState: checkShardingState,
    };
})();
