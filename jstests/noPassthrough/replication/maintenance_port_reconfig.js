/*
 * Tests ReplSetReconfig with maintenancePort specified.
 *
 * @tags: [
 *  featureFlagReplicationUsageOfMaintenancePort,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";

describe("Tests for maintenance port usage within JS test helpers", function () {
    beforeEach(() => {
        // TODO (SERVER-112863) Remove tests of old LTS once 9.0 becomes lastLTS.
        this.checkShouldRunFCVGatedTest = function (conn) {
            let maintenancePortEnabledFCV = FeatureFlagUtil.getFeatureFlagDoc(
                conn,
                "ReplicationUsageOfMaintenancePort",
            ).version;
            return MongoRunner.compareBinVersions(lastLTSFCV, maintenancePortEnabledFCV) == -1;
        };
    });

    it("ReplSetInitiate with maintenance port on FCV 8.0 will fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
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

    it("Reconfig with maintenance port on FCV 8.0 will fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
            nodeOptions: {setParameter: {defaultStartupFCV: lastLTSFCV}},
        });
        rs.startSet();
        // Initiate without maintenance port since including on FCV 8.0 would fail.
        let config = rs.getReplSetConfig(true /* ignoreMaintenancePort */);
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

    it("Initiate with incorrect port for maintenance port should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
        });
        let conns = rs.startSet();
        let config = rs.getReplSetConfig();
        // Change the maintenance port to the wrong port
        config.members[0].maintenancePort += 1;

        assert.commandFailedWithCode(
            conns[0].adminCommand({replSetInitiate: config}),
            ErrorCodes.InvalidReplicaSetConfig,
        );
        rs.stopSet();
    });

    it("Initiate with maintenance port when none is available should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
        });
        let conns = rs.startSet();
        let config = rs.getReplSetConfig();
        // Specify a maintenance port
        config.members[0].maintenancePort = 30;

        assert.commandFailedWithCode(
            conns[0].adminCommand({replSetInitiate: config}),
            ErrorCodes.InvalidReplicaSetConfig,
        );
        rs.stopSet();
    });

    it("Reconfig with incorrect port for maintenance port should fail", () => {
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
        });
        rs.startSet();
        let config = rs.getReplSetConfig();
        rs.initiate(config);

        let newConfig = rs.getReplSetConfigFromNode();
        newConfig.members[0].maintenancePort += 1;
        newConfig.version += 1;

        assert.commandFailedWithCode(
            rs.getPrimary().adminCommand({replSetReconfig: newConfig}),
            ErrorCodes.NodeNotFound,
        );

        rs.stopSet();
    });

    it("Initiate with maintenance port plus bindIp works when fast resolution works", () => {
        let ips = "localhost," + get_ipaddr();
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
            nodeOptions: {bind_ip: ips},
        });
        rs.startSet();
        rs.initiate();

        rs.stopSet();
    });

    it("Initiate with maintenance port plus bindIp works when fast resolution does not", () => {
        let ips = "localhost," + getHostName();
        const rs = new ReplSetTest({
            nodes: 1,
            useMaintenancePorts: true,
            nodeOptions: {bind_ip: ips},
        });
        rs.startSet();
        rs.initiate();

        rs.stopSet();
    });
});
