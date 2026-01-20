/*
 * Tests ReplSetReconfig with priorityPort specified.
 *
 * @tags: [
 *  featureFlagReplicationUsageOfPriorityPort,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";

describe("Tests for priority port usage within JS test helpers", function () {
    beforeEach(() => {
        // TODO (SERVER-112863) Remove tests of old LTS once 9.0 becomes lastLTS.
        this.checkShouldRunFCVGatedTest = function (conn) {
            let priorityPortEnabledFCV = FeatureFlagUtil.getFeatureFlagDoc(
                conn,
                "ReplicationUsageOfPriorityPort",
            ).version;
            return MongoRunner.compareBinVersions(lastLTSFCV, priorityPortEnabledFCV) == -1;
        };

        this.dropAllConns = function (rs) {
            rs.nodes.forEach((conn) => {
                const cfg = conn.getDB("local").system.replset.findOne();
                const allHosts = cfg.members.map((x) => x.host);
                assert.commandWorked(conn.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
            });
        };
    });

    it("ReplSetInitiate with priority port on FCV 8.0 will fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
            nodeOptions: {setParameter: {defaultStartupFCV: lastLTSFCV}},
        });
        let conns = rs.startSet();
        if (this.checkShouldRunFCVGatedTest(conns[0])) {
            assert.commandFailedWithCode(
                conns[0].adminCommand({
                    replSetInitiate: rs.getReplSetConfig(),
                }),
                ErrorCodes.InvalidReplicaSetConfig,
            );
        }
        rs.stopSet();
    });

    it("Reconfig with priority port on FCV 8.0 will fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
            nodeOptions: {setParameter: {defaultStartupFCV: lastLTSFCV}},
        });
        rs.startSet();
        // Initiate without priority port since including on FCV 8.0 would fail.
        let config = rs.getReplSetConfig(true /* ignorePriorityPort */);
        rs.initiate(config);

        if (this.checkShouldRunFCVGatedTest(rs.getPrimary())) {
            let config = rs.getReplSetConfigFromNode();
            let newConfig = rs.getReplSetConfig();
            config.members = newConfig.members;
            config.version += 1;

            assert.commandFailedWithCode(
                rs.getPrimary().adminCommand({replSetReconfig: config}),
                ErrorCodes.NewReplicaSetConfigurationIncompatible,
            );
        }

        rs.stopSet();
    });

    it("Initiate with incorrect port for priority port should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
        });
        let conns = rs.startSet();
        let config = rs.getReplSetConfig();
        // Change the priority port to the wrong port
        config.members[0].priorityPort += 1;

        assert.commandFailedWithCode(
            conns[0].adminCommand({replSetInitiate: config}),
            ErrorCodes.InvalidReplicaSetConfig,
        );
        rs.stopSet();
    });

    it("Initiate with priority port when none is available should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
        });
        let conns = rs.startSet();
        let config = rs.getReplSetConfig();
        // Specify a priority port
        config.members[0].priorityPort = 30;

        assert.commandFailedWithCode(
            conns[0].adminCommand({replSetInitiate: config}),
            ErrorCodes.InvalidReplicaSetConfig,
        );
        rs.stopSet();
    });

    it("Reconfig with incorrect port for priority port should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
        });
        rs.startSet();
        let config = rs.getReplSetConfig();
        rs.initiate(config);

        let newConfig = rs.getReplSetConfigFromNode();
        newConfig.members[0].priorityPort += 1;
        newConfig.version += 1;

        assert.commandFailedWithCode(
            rs.getPrimary().adminCommand({replSetReconfig: newConfig}),
            ErrorCodes.NodeNotFound,
        );

        rs.stopSet();
    });

    it("Initiate with wrong priority port on a majority of nodes should fail", () => {
        const rs = new ReplSetTest({nodes: [{priorityPort: allocatePort()}, {}, {}]});
        rs.startSet();

        jsTest.log.info("Initiate should fail because we specify priority ports which are not open on the secondaries");
        let config = rs.getReplSetConfig();
        config.members[1].priorityPort = 27022;
        config.members[2].priorityPort = 27023;

        assert.commandFailedWithCode(rs.nodes[0].adminCommand({replSetInitiate: config}), ErrorCodes.NodeNotFound);

        rs.stopSet();
    });

    it("Reconfig with wrong priority port on a majority of nodes should fail", () => {
        const rs = new ReplSetTest({nodes: [{priorityPort: allocatePort()}, {}, {}]});
        rs.startSet();
        rs.initiate();

        jsTest.log.info("Initiate should fail because we specify priority ports which are not open on the secondaries");
        let config = rs.getReplSetConfigFromNode();
        config.members[1].priorityPort = 27022;
        config.members[2].priorityPort = 27023;
        config.version += 1;

        assert.commandFailedWithCode(rs.getPrimary().adminCommand({replSetReconfig: config}), ErrorCodes.NodeNotFound);

        rs.stopSet();
    });

    it("Initiate when we can only reach the priority port on a majority of nodes should fail", () => {
        const rs = new ReplSetTest({nodes: 3, usePriorityPorts: true});
        rs.startSet();

        jsTest.log.info("Block connections on the main ports");
        let fps = configureFailPointForRS(rs.nodes, "rejectNewNonPriorityConnections");

        jsTest.log.info("Initiate should fail because we can only connect on the priority ports");
        let config = rs.getReplSetConfig();
        assert.commandFailedWithCode(rs.nodes[0].adminCommand({replSetInitiate: config}), ErrorCodes.NodeNotFound);

        rs.stopSet();
    });

    it("Reconfig when we can only reach the priority port on a majority of nodes should fail", () => {
        const rs = new ReplSetTest({nodes: 3, usePriorityPorts: true});
        rs.startSet();
        rs.initiate();

        let config = rs.getReplSetConfigFromNode();

        jsTest.log.info("Block connections on the main ports and drop existing connections");
        let fps = configureFailPointForRS(rs.nodes, "rejectNewNonPriorityConnections");
        this.dropAllConns(rs);

        jsTest.log.info("Reconfig should fail because we can only connect on the priority ports");
        config.version += 1;
        assert.commandFailedWithCode(rs.getPrimary().adminCommand({replSetReconfig: config}), ErrorCodes.NodeNotFound);

        fps.off();
        rs.stopSet();
    });

    it("Initiate with priority port plus bindIp works when fast resolution works", () => {
        let ips = "localhost," + get_ipaddr();
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
            nodeOptions: {bind_ip: ips},
        });
        rs.startSet();
        rs.initiate();

        rs.stopSet();
    });

    it("Initiate with priority port plus bindIp works when fast resolution does not", () => {
        let ips = "localhost," + getHostName();
        const rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
            nodeOptions: {bind_ip: ips},
        });
        rs.startSet();
        rs.initiate();

        rs.stopSet();
    });
});
