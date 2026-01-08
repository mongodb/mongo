/*
 * Tests replication internals (such as heartbeats, initial sync, elections, etc.) via the
 * maintenance port.
 *
 * @tags: [
 *  featureFlagReplicationUsageOfMaintenancePort,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

describe("Tests for maintenance port usage within replication internals", function () {
    function dropAllConns(rs) {
        rs.nodes.forEach((conn) => {
            const cfg = conn.getDB("local").system.replset.findOne();
            const allHosts = cfg.members.map((x) => x.host);
            assert.commandWorked(conn.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
        });
    }

    it("Election via maintenance port", () => {
        const rs = new ReplSetTest({nodes: 3, useMaintenancePorts: true});
        rs.startSet();
        rs.initiate();

        jsTest.log.info("Wait for replication so that we can step up a new primary");
        rs.waitForStepUpWrites();
        rs.awaitReplication();

        jsTest.log.info("Block connections on the main port and drop all existing connections from within the RS");
        let fps = configureFailPointForRS(rs.nodes, "rejectNewNonPriorityConnections");
        dropAllConns(rs);

        jsTest.log.info("Make a connection to a secondary's maintenance port and step it up");
        let secondary = rs.getSecondary();
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
        rs.stopSet();
    });

    it("Heartbeats via maintenance port", () => {
        let verbosity = {setParameter: {logComponentVerbosity: {replication: {heartbeats: 2}}}};
        const rs = new ReplSetTest({nodes: 3, useMaintenancePorts: true, nodeOptions: verbosity});
        rs.startSet();
        let config = rs.getReplSetConfig();
        config.settings = {
            heartbeatTimeoutSecs: 1,
            electionTimeoutMillis: 1000,
        };
        rs.initiate(config);

        let connstring = rs.getPrimary().host;
        let host = connstring.slice(0, connstring.indexOf(":"));

        // TODO (SERVER-114060): Drop existing connections once we sync via maintenance port
        jsTest.log.info("Block connections on the main port");
        let fps = configureFailPointForRS(rs.nodes, "rejectNewNonPriorityConnections");

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

        for (let i = 0; i < rs.nodes.length; i++) {
            let maintenanceConnString = host + ":" + rs.getMaintenancePort(i);
            let conn = newMongoWithRetry(maintenanceConnString);
            let res = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
            assert(
                res.members.some((member) => {
                    return member.state === ReplSetTest.State.PRIMARY;
                }),
            );
        }

        fps.off();
        rs.stopSet();
    });

    // TODO (SERVER-114060): Test election handoffs once we can select a sync source via the maintenance port.
});
