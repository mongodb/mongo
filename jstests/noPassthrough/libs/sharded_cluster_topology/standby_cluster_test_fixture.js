/**
 * Test fixture for simulating a standby cluster's config server.
 *
 * Example usage:
 *
 *      const fixture = new StandbyClusterTestFixture({name: "myTest", shards: 2});
 *      // Use fixture.st to interact with the cluster as a normal ShardingTest.
 *      fixture.st.s.getDB("test").foo.insert({_id: 1});
 *      fixture.transitionToStandby();
 *      // Interact with the standby config replica set via fixture.standbyRS.
 *      const primary = fixture.standbyRS.getPrimary();
 *      fixture.teardown();
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

export class StandbyClusterTestFixture {
    /**
     * @param {object} opts
     * @param {string} [opts.keyFile] - Path to keyfile for internal auth (e.g.
     *                                  "jstests/libs/key1"). When set, all components use
     *                                  keyFile-based auth, and transitionToStandby() starts
     *                                  intermediate standalone nodes with --noauth.
     *
     * All other properties are forwarded directly to the ShardingTest constructor.
     */
    constructor({keyFile = null, ...shardingTestOpts} = {}) {
        /** @private */
        this._keyFile = keyFile;
        /** @private */
        this._shardingTestOpts = shardingTestOpts;

        /** The underlying ShardingTest. @type {ShardingTest} */
        this.st = null;

        /**
         * The ReplSetTest for the standby config server replica set.
         * Available after transitionToStandby().
         * @type {ReplSetTest}
         */
        this.standbyRS = null;

        this._setup();
    }

    /** @private */
    _setup() {
        this.st = new ShardingTest({
            ...(this._keyFile ? {keyFile: this._keyFile} : {}),
            ...this._shardingTestOpts,
        });
    }

    /**
     * Transitions the fixture into a standby cluster simulation:
     *  1. Stops all mongos processes.
     *  2. Stops all shard replica sets (data cleaned up).
     *  3. Stops the config server replica set (data preserved).
     *  4. For each config server node: starts it as a standalone on its old port, rewrites
     *     local.system.replset to use the new ports and remove configsvr, then shuts it down.
     *  5. Starts all nodes back up as a new "standby" replica set on the fresh ports.
     */
    transitionToStandby() {
        const configRS = this.st.configRS;
        const numNodes = configRS.nodes.length;

        // Stop all mongos processes.
        this.st.stopAllMongos();

        // Stop all shards (we don't need them anymore, data can be cleaned).
        // TODO (SERVER-123326): handle the embedded config server case to ensure we don't clear up
        // the config server without preserving data on disk.
        this.st.stopAllShards();

        // Stop config server RS, preserving data on disk.
        this.st.stopAllConfigServers({noCleanData: true});

        // Snapshot dbPaths before nodes are shut down.
        const dbPaths = Array.from({length: numNodes}, (_, i) => configRS.getDbPath(i));
        const oldPorts = configRS.nodes.map((node) => node.port);

        // Allocate new ports for the standby replica set.
        const newPorts = Array.from({length: numNodes}, () => allocatePort());

        // Build the new config once by reading the replset config from node 0.
        // All nodes share the same config, so reading it once is sufficient.
        const node0 = MongoRunner.runMongod({
            port: oldPorts[0],
            dbpath: dbPaths[0],
            noReplSet: "",
            noCleanData: true,
            ...(this._keyFile ? {noauth: ""} : {}),
        });
        assert(node0, `Failed to start standalone mongod for config node 0 on port ${oldPorts[0]}`);

        const existingConfig = node0.getDB("local").getCollection("system.replset").findOne();
        assert(existingConfig, `No document in local.system.replset on config node 0`);

        MongoRunner.stopMongod(node0);

        // Build a new config: rename the set to "standby", replace all member ports with the
        // newly-allocated ports, and remove the configsvr field.
        const newMembers = existingConfig.members.map((member, idx) => {
            const newMember = Object.extend({}, member, /*deep=*/ true);
            const hostParts = newMember.host.split(":");
            hostParts[hostParts.length - 1] = String(newPorts[idx]);
            newMember.host = hostParts.join(":");
            return newMember;
        });

        const newConfig = Object.extend({}, existingConfig, /*deep=*/ true);
        newConfig._id = "standby";
        newConfig.members = newMembers;
        delete newConfig.configsvr;

        // Replace the hosts in each individual replSetConfig.
        for (let i = 0; i < numNodes; i++) {
            // Start as a standalone (no --replSet, no --configsvr) on the old port so we can
            // freely modify local.system.replset. When auth is enabled we use --noauth to
            // bypass credential checks for this transient rewrite step.
            const standalone = MongoRunner.runMongod({
                port: oldPorts[i],
                dbpath: dbPaths[i],
                noReplSet: "",
                noCleanData: true,
                ...(this._keyFile ? {noauth: ""} : {}),
            });
            assert(standalone, `Failed to start standalone mongod for config node ${i} on port ${oldPorts[i]}`);

            const replSetColl = standalone.getDB("local").getCollection("system.replset");

            // Swap documents: insert new, remove old.
            assert.commandWorked(replSetColl.insertOne(newConfig));
            assert.commandWorked(replSetColl.deleteOne({_id: existingConfig._id}));

            MongoRunner.stopMongod(standalone);
        }

        // Start each node on its new port with the existing data directory. noCleanData prevents
        // MongoRunner from wiping the dbpath on first start.
        this.standbyRS = new ReplSetTest({
            name: "standby",
            ports: newPorts,
            nodes: dbPaths.map((dbPath) => ({dbpath: dbPath, noCleanData: true})),
            ...(this._keyFile ? {keyFile: this._keyFile} : {}),
        });
        // startSet() launches the processes with --replSet standby but does NOT call
        // replSetInitiate. The nodes will self-elect using the config already in
        // local.system.replset from the previous step.
        this.standbyRS.startSet();
        this.standbyRS.awaitNodesAgreeOnPrimary();
    }

    /**
     * Shuts down the standby replica set (if transitioned) and cleans up.
     */
    teardown() {
        if (this.standbyRS) {
            this.standbyRS.stopSet();
        } else {
            this.st.stop();
        }
    }
}
