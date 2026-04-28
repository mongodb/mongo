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

        /**
         * Array of {pid, port} objects for mongosentry processes guarding retired ports.
         * Available after transitionToStandby().
         * @type {Array<{pid: number, port: number}>|null}
         */
        this._sentries = null;

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

        // Capture shard ports before stopping shards so we can guard them with sentries.
        const shardPorts = this.st._rs.flatMap((shard) => shard.test.ports);

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
            newMember.priority = idx === 0 ? 1 : 0;
            return newMember;
        });

        const newConfig = Object.extend({}, existingConfig, /*deep=*/ true);
        newConfig._id = "standby";
        newConfig.members = newMembers;
        delete newConfig.configsvr;

        // We use a single electable member (priority set in newMembers above) and a very long
        // cluster-wide election timeout so that after the forced stepdown no new primary is elected
        // for the duration of the test.
        if (!newConfig.settings) {
            newConfig.settings = {};
        }
        newConfig.settings.electionTimeoutMillis = ReplSetTest.kForeverMillis;

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
        let nodes = this.standbyRS.startSet();
        this.standbyRS.asCluster(nodes, () => {
            this.standbyRS.stepUp(nodes[0], {awaitReplicationBeforeStepUp: false});
            this.standbyRS.awaitNodesAgreeOnPrimary();
            // Write a majority-committed noop before stepping down to ensure the committed snapshot
            // is established. The primary's drain-completion noop may not yet be journaled, and
            // without a journaled entry the commit point (and thus the committed snapshot) never
            // advances. Once the node steps down to secondary, the commit point can no longer
            // advance, so majority reads would be permanently blocked.
            const primary = this.standbyRS.getPrimary();
            assert.commandWorked(
                primary.adminCommand({
                    appendOplogNote: 1,
                    data: {msg: "standby transition commit point advance"},
                    writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS},
                }),
            );
            assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
        });

        // Start a mongosentry on every retired port (old shard ports and old config server ports).
        // Any traffic to these ports indicates a bug - the sentry will invariant if it receives a
        // MongoDB wire protocol message.
        const retiredPorts = [...shardPorts, ...oldPorts];
        this._sentries = retiredPorts.map((port) => {
            const pid = _startMongoProgram("mongosentry", "--port", port.toString());
            return {pid, port};
        });
        // Wait for each sentry process to be alive before continuing.
        for (const {pid, port} of this._sentries) {
            assert.soon(() => checkProgram(pid).alive, `mongosentry failed to start on retired port ${port}`);
        }
    }

    /**
     * Shuts down the standby replica set (if transitioned) and cleans up.
     *
     * Checks that no mongosentry process received traffic on a retired port before stopping
     * them. Throws if any sentry hit its invariant (i.e. received a command it should not have).
     */
    teardown() {
        const failedPorts = [];
        for (const {pid, port} of this._sentries || []) {
            const {alive} = checkProgram(pid);
            if (!alive) {
                jsTest.log.info(`mongosentry on retired port ${port} received a command and hit an invariant`);
                failedPorts.push(port);
            } else {
                stopMongoProgramByPid(pid);
            }
        }

        if (this.standbyRS) {
            this.standbyRS.stopSet();
        } else {
            this.st.stop();
        }

        if (failedPorts.length > 0) {
            throw new Error(`Commands were sent to retired ports after standby transition: ` + failedPorts.join(", "));
        }
    }
}
