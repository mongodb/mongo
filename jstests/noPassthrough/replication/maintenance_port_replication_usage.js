/*
 * Tests replication internals (such as heartbeats, initial sync, elections, etc.) via the
 * maintenance port.
 *
 * @tags: [
 *  featureFlagReplicationUsageOfMaintenancePort,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPointForRS, configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ElectionHandoffTest} from "jstests/replsets/libs/election_handoff.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

describe("Tests for maintenance port usage within replication internals", function () {
    beforeEach(() => {
        let verbosity = {setParameter: {logComponentVerbosity: {replication: {heartbeats: 2}}}};
        this.rs = new ReplSetTest({nodes: 3, useMaintenancePorts: true, nodeOptions: verbosity});
        this.rs.startSet();
    });

    afterEach(() => {
        this.rs.stopSet();
    });

    function dropAllConns(rs) {
        rs.nodes.forEach((conn) => {
            const cfg = conn.getDB("local").system.replset.findOne();
            const allHosts = cfg.members.map((x) => x.host);
            assert.commandWorked(conn.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
        });
    }

    function killAllOplogFetchings(rs) {
        rs.nodes.forEach((conn) => {
            const oplogFetcherOps = conn
                .getDB("admin")
                .aggregate([
                    {$currentOp: {}},
                    {$match: {"appName": "OplogFetcher"}},
                    {$project: {cursorId: "$cursor.cursorId"}},
                ])
                .toArray();
            oplogFetcherOps.forEach((doc) => {
                conn.adminCommand({killCursors: "oplog.rs", cursors: [doc.cursorId]});
            });
        });
    }

    function getHostsAndMaintenancePorts(config) {
        let hostAndMaintenancePorts = [];
        config.members.forEach((member) => {
            let host = member.host.slice(0, member.host.indexOf(":"));
            let hostAndMaintenancePort = host + ":" + member.maintenancePort;
            hostAndMaintenancePorts.push(hostAndMaintenancePort);
        });
        return hostAndMaintenancePorts;
    }

    function checkSyncingViaHostAndPorts(rs, hostAndPorts) {
        rs.nodes.forEach((node) => {
            assert.soon(() => {
                let status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
                // Primaries don't sync so return true immediately.
                if (status.myState == ReplSetTest.State.PRIMARY) {
                    return true;
                }
                return status.members.some((member) => {
                    return member.self && hostAndPorts.includes(member.syncSourceHost);
                });
            });
        });
    }

    function doWritesAndCheckReplication(primaryConn, secondaryConns) {
        jsTest.log.info("Do some writes on the primary with local write concern");
        for (let i = 0; i < 100; i++) {
            assert.commandWorked(
                primaryConn.getDB("test").runCommand({insert: "foo", documents: [{x: i}], writeConcern: {w: 1}}),
            );
        }

        jsTest.log.info(
            "Check that we succcessfully replicate these writes to the secondaries despite the main port being closed",
        );
        assert.soon(() => {
            let count = 0;
            secondaryConns.forEach((conn) => {
                count += conn.getDB("test").getCollection("foo").count();
            });
            jsTest.log.info("Current document count: " + count);
            return count == 100 * secondaryConns.length;
        });
    }

    it("Election via maintenance port", () => {
        this.rs.initiate();

        jsTest.log.info("Wait for replication so that we can step up a new primary");
        this.rs.waitForStepUpWrites();
        this.rs.awaitReplication();

        jsTest.log.info("Block connections on the main port and drop all existing connections from within the RS");
        let fps = configureFailPointForRS(this.rs.nodes, "rejectNewNonPriorityConnections");
        dropAllConns(this.rs);

        jsTest.log.info("Make a connection to a secondary's maintenance port and step it up");
        let secondary = this.rs.getSecondary();
        let connstring = secondary.hostNoPort + ":" + secondary.maintenancePort;
        let conn = newMongoWithRetry(connstring);
        assert.soon(() => {
            assert.commandWorked(conn.adminCommand({replSetStepUp: 1}));
            let res = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
            return res.members.some((member) => {
                return member.self && member.state === ReplSetTest.State.PRIMARY;
            });
        });

        fps.off();
    });

    it("Heartbeats via maintenance port", () => {
        let config = this.rs.getReplSetConfig();
        config.settings = {
            heartbeatTimeoutSecs: 1,
            electionTimeoutMillis: 1000,
        };
        this.rs.initiate(config);

        let connstring = this.rs.getPrimary().host;
        let host = connstring.slice(0, connstring.indexOf(":"));

        jsTest.log.info("Block connections on the main port");
        let fps = configureFailPointForRS(this.rs.nodes, "rejectNewNonPriorityConnections");
        dropAllConns(this.rs);

        jsTest.log.info("Ensure that the primary doesn't step down due to no heartbeat responses");
        clearRawMongoProgramOutput();
        let waitForHeartbeats = 10;
        const heartbeatResponseReceivedLogID = '"id":4615620';
        const heartbeatRequestSentLogID = '"id":4615670';
        assert.soon(() => {
            // Check for sending heartbeats on the primary
            return (
                rawMongoProgramOutput(heartbeatResponseReceivedLogID).split("\n").length >= waitForHeartbeats &&
                rawMongoProgramOutput(heartbeatRequestSentLogID).split("\n").length >= waitForHeartbeats
            );
        });

        for (let i = 0; i < this.rs.nodes.length; i++) {
            let maintenanceConnString = host + ":" + this.rs.getMaintenancePort(i);
            let conn = newMongoWithRetry(maintenanceConnString);
            let res = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
            assert(
                res.members.some((member) => {
                    return member.state === ReplSetTest.State.PRIMARY;
                }),
            );
        }

        fps.off();
    });

    it("Steady state replication via maintenance port", () => {
        this.rs.initiate();

        jsTest.log.info("Block connections and kill ongoing oplog fetching to force a new connection");
        let fps = configureFailPointForRS(this.rs.nodes, "rejectNewNonPriorityConnections");
        dropAllConns(this.rs);
        killAllOplogFetchings(this.rs);

        doWritesAndCheckReplication(this.rs.getPrimary(), this.rs.getSecondaries());

        fps.off();
    });

    it("ForceSyncSourceCandidate with host and main port", () => {
        this.rs.initiate();

        jsTest.log.info(
            "Set `forceSyncSourceCandidate` with the main port and kill oplog fetchings to force a new choice",
        );
        let secondary = this.rs.getSecondary();
        let primaryHostAndPort = this.rs.getPrimary().host;
        let fp = configureFailPoint(secondary, "forceSyncSourceCandidate", {hostAndPort: primaryHostAndPort});
        killAllOplogFetchings(this.rs);

        jsTest.log.info("Do some writes and check replication works");
        doWritesAndCheckReplication(this.rs.getPrimary(), this.rs.getSecondaries());

        jsTest.log.info("Ensure the sync source is the one we forced");
        let status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        assert(
            status.members.some((member) => {
                return member.self && member.syncSourceHost == primaryHostAndPort;
            }),
        );

        fp.off();
    });

    it("Initial sync via maintenance port", () => {
        this.rs.initiate();

        jsTest.log.info("Block new connections");
        let fps = configureFailPointForRS(this.rs.nodes, "rejectNewNonPriorityConnections");

        jsTest.log.info("Add a new node to the replica set configuration");
        let newNode = this.rs.add();
        let config = this.rs.getReplSetConfigFromNode();
        config.members.push({_id: 3, host: newNode.host, maintenancePort: parseInt(newNode.maintenancePort)});
        config.version++;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));

        jsTest.log.info("Ensure the new node successfully becomes a secondary");
        this.rs.awaitSecondaryNodes(60 * 1000, [newNode]);

        fps.off();
    });

    it("Election handoff via maintenance port", () => {
        this.rs.initiate();

        jsTest.log.info("Block connections on the main port and drop all existing connections from within the RS");
        let fps = configureFailPointForRS(this.rs.nodes, "rejectNewNonPriorityConnections");
        dropAllConns(this.rs);

        jsTest.log.info("Test election handoff");
        ElectionHandoffTest.testElectionHandoff(this.rs, 0, 1);

        fps.off();
    });

    it("Replication will switch to the maintenance port if it is added", () => {
        let config = this.rs.getReplSetConfig(true);
        this.rs.initiate(config);

        jsTest.log.info("Reconfigure with maintenance ports");
        let newConfig = this.rs.getReplSetConfigFromNode();
        newConfig.members = this.rs.getReplSetConfig().members;
        newConfig.version += 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: newConfig}));

        jsTest.log.info("Do some writes to make sure there is someone to sync from which is ahead of us");
        doWritesAndCheckReplication(this.rs.getPrimary(), this.rs.getSecondaries());

        jsTest.log.info("Check that we switch to replicating via the maintenance port");
        let hostsAndMaintenancePorts = getHostsAndMaintenancePorts(newConfig);
        checkSyncingViaHostAndPorts(this.rs, hostsAndMaintenancePorts);
    });

    it("Replication will switch back to the main port if the maintenance port is removed", () => {
        this.rs.initiate();

        jsTest.log.info("Check that current syncing is being done via maintenance ports");
        let hostsAndMaintenancePorts = getHostsAndMaintenancePorts(this.rs.getReplSetConfigFromNode());
        checkSyncingViaHostAndPorts(this.rs, hostsAndMaintenancePorts);

        jsTest.log.info("Reconfigure without the maintenance ports");
        let config = this.rs.getReplSetConfigFromNode();
        config.members.forEach((member) => {
            delete member.maintenancePort;
        });
        config.version += 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));

        this.rs.awaitReplication();

        jsTest.log.info("Do a rolling restart to remove the maintenance ports");
        jsTest.log.info("Restarting secondaries");
        this.rs.getSecondaries().forEach((secondary) => {
            const id = this.rs.getNodeId(secondary);
            jsTest.log.info("Stopping node " + id);
            this.rs.stop(id, null, {}, {forRestart: true, waitPid: true});
            jsTest.log.info(
                "Full options, " +
                    tojson(this.rs.nodes[id].fullOptions) +
                    ", node options: " +
                    tojson(this.rs.nodeOptions["n" + id]),
            );
            delete this.rs.nodes[id].fullOptions.maintenancePort;
            jsTest.log.info("Starting node " + id);
            assert.doesNotThrow(() => {
                this.rs.start(id, {restart: true, remember: true});
            });
        });
        jsTest.log.info("Stepping up secondary");
        const primaryId = this.rs.getNodeId(this.rs.getPrimary());
        this.rs.stepUp(this.rs.getSecondary());
        jsTest.log.info("Restart old primary");
        this.rs.stop(
            primaryId,
            null,
            {},
            {
                forRestart: true,
                waitPid: true,
            },
        );
        delete this.rs.nodes[primaryId].fullOptions.maintenancePort;
        assert.doesNotThrow(() => {
            this.rs.start(primaryId, {restart: true, remember: true});
        });

        jsTest.log.info("Check steady state replication");
        doWritesAndCheckReplication(this.rs.getPrimary(), this.rs.getSecondaries());
    });
});
