/**
 * Test fixture for simulating a standby cluster's config server. Construction launches a normal
 * ShardingTest; `transitionToStandby()` then converts the config server replica set into a
 * non-configsvr "standby" replica set with node 0 tagged `processType: INJECTOR`.
 *
 * After the transition, a `failCommand` failpoint is enabled on node 0 that fails every command
 * except the small wire-protocol allowlist the real injector handles (hello, isMaster, ismaster,
 * ping, etc.).
 *
 * Example usage:
 *
 *      const fixture = new StandbyClusterTestFixture({name: "myTest", shards: 2});
 *      // Use fixture.st to interact with the cluster as a normal ShardingTest.
 *      fixture.st.s.getDB("test").foo.insert({_id: 1});
 *      fixture.transitionToStandby();
 *      // Interact with the standby config replica set via fixture.standbyRS.
 *      const injector = fixture.standbyRS.nodes[0]; // INJECTOR-tagged, failpoint-active
 *      fixture.teardown();
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
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

        /** @private Handle returned by configureFailPoint(); used to disable the failpoint. */
        this._injectorFailpoint = null;

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
     *  2. Stops all shard replica sets (data cleaned up). In embedded config server mode,
     *     the config shard (shard 0) is stopped last with its data preserved.
     *  3. If not in embedded config server mode, stops the config server RS (data preserved).
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

        if (this.st.isConfigShardMode) {
            // In embedded config server mode one of the shards is the config server. Stop the non-config
            // shards first (allowing data cleanup), then stop the config shard while preserving
            // its data on disk.
            for (const rs of this.st._rs) {
                if (rs.test !== configRS) {
                    rs.test.stopSet();
                }
            }
            configRS.stopSet(undefined, undefined, {noCleanData: true});
        } else {
            // Stop all shards (we don't need them anymore, data can be cleaned).
            this.st.stopAllShards();

            // Stop config server RS, preserving data on disk.
            this.st.stopAllConfigServers({noCleanData: true});
        }

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
        // newly-allocated ports, and remove the configsvr field. Only node 0 is electable
        // (priority 1; others priority 0), so it deterministically wins the election and becomes
        // primary -- this mirrors a real standby cluster where the injector is always primary.
        // Node 0 is tagged with processType: INJECTOR so the topology matches production: the
        // RSM keeps the INJECTOR-tagged primary visible for replication purposes but excludes it
        // from server selection, forcing client traffic to the secondaries.
        const newMembers = existingConfig.members.map((member, idx) => {
            const newMember = Object.extend({}, member, /*deep=*/ true);
            const hostParts = newMember.host.split(":");
            hostParts[hostParts.length - 1] = String(newPorts[idx]);
            newMember.host = hostParts.join(":");
            newMember.priority = idx === 0 ? 1 : 0;
            if (idx === 0) {
                newMember.tags = {processType: "INJECTOR"};
            }
            return newMember;
        });

        const newConfig = Object.extend({}, existingConfig, /*deep=*/ true);
        newConfig._id = "standby";
        newConfig.members = newMembers;
        delete newConfig.configsvr;

        // Node 0 is the only electable member (priority set in newMembers above) and remains
        // primary for the duration of the test, mirroring a real standby cluster where the
        // INJECTOR-tagged node is primary.
        newConfig.settings = newConfig.settings || {};
        newConfig.settings.electionTimeoutMillis = 10000;

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
            // Node 0 stays primary; the INJECTOR tag keeps client traffic from being routed to it
            // via server selection, while the failpoint fails any command that does slip through
            // (everything outside the wire-protocol allowlist the real injector handles).
            this._injectorFailpoint = configureFailPoint(nodes[0], "failCommand", {
                failAllCommands: true,
                failCommandsExcept: [
                    "hello",
                    "isMaster",
                    "ismaster",
                    "ping",
                    "find",
                    "getMore",
                    "killCursors",
                    "replSetHeartbeat",
                    "replSetUpdatePosition",
                    "saslStart",
                    "saslContinue",
                ],
                errorCode: 12319007,
            });
        });

        // Start a mongosentry on every retired port (old shard ports and old config server ports).
        // Any traffic to these ports indicates a bug - the sentry will invariant if it receives a
        // MongoDB wire protocol message.
        // In config shard mode, shardPorts already includes the config server ports (since shard 0
        // IS the config server), so we deduplicate to avoid binding sentries to the same port.
        const retiredPorts = [...new Set([...shardPorts, ...oldPorts])];
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
            // Disable the injector failpoint before stopSet (if it was enabled), otherwise the
            // shutdown command (not on the allowlist) would be rejected.
            if (this._injectorFailpoint) {
                try {
                    this._injectorFailpoint.off();
                } catch (e) {
                    jsTest.log.info(`Failed to disable injector failpoint during teardown: ${e}`);
                }
                this._injectorFailpoint = null;
            }
            this.standbyRS.stopSet();
        } else {
            this.st.stop();
        }

        if (failedPorts.length > 0) {
            throw new Error(`Commands were sent to retired ports after standby transition: ` + failedPorts.join(", "));
        }
    }
}
