/**
 * Test that the maintenance port option is specified correctly in a ReplSetTest and a ShardingTest.
 *
 * TODO (SERVER-112674): Extend the integration to expose connections on both the main and
 * maintenance ports and add replace the testing coverage via logging with connection based tests.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   featureFlagDedicatedPortForMaintenanceOperations,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("Tests for maintenance port usage within JS test helpers", function () {
    const checkMaintenancePortSet = (conn, port) => {
        assert.eq(port, conn.maintenancePort);
        assert(
            checkLog.checkContainsOnceJson(conn, 21951, {
                options: (opts) => {
                    return opts.net.maintenancePort == port;
                },
            }),
        );
    };

    // TODO (SERVER-112674): Remove this. For now, we set the log verbosity very low to ensure we still see the startup logs
    const verbosityOptions = {setParameter: {logComponentVerbosity: {verbosity: 0}}};

    it("Starting up a replica set with maintenance ports", () => {
        const rs = new ReplSetTest({nodes: 3, useMaintenancePorts: true, nodeOptions: verbosityOptions});
        rs.startSet();
        rs.initiate();

        rs.nodes.forEach((conn) => {
            let maintenancePort = rs.getMaintenancePort(conn);
            checkMaintenancePortSet(conn, maintenancePort);
        });

        rs.stopSet();
    });

    it("Starting up a replica set with a specific node having a maintenance port", () => {
        const rs = new ReplSetTest({nodes: [{}, {maintenancePort: 27021}, {}], nodeOptions: verbosityOptions});
        rs.startSet();
        rs.initiate();

        let countFoundPorts = 0;

        rs.nodes.forEach((conn) => {
            try {
                assert.eq(27021, rs.getMaintenancePort(conn));
                checkMaintenancePortSet(conn, 27021);
                countFoundPorts += 1;
            } catch (e) {
                // This should throw if the maintenance port is not set for the node.
                assert(!conn.maintenancePort);
            }
        });

        assert.eq(countFoundPorts, 1);

        rs.stopSet();
    });

    it("Starting up a sharded cluster with maintenance ports", () => {
        const st = new ShardingTest({
            shards: 1,
            mongos: 2,
            useMaintenancePorts: true,
            other: {nodeOptions: verbosityOptions, configOptions: verbosityOptions, mongosOptions: verbosityOptions},
        });

        st.configRS.nodes.forEach((conn) => {
            let maintenancePort = st.configRS.getMaintenancePort(conn);
            checkMaintenancePortSet(conn, maintenancePort);
        });

        st.rs0.nodes.forEach((conn) => {
            let maintenancePort = st.rs0.getMaintenancePort(conn);
            checkMaintenancePortSet(conn, maintenancePort);
        });

        st._mongos.forEach((conn) => {
            let maintenancePort = conn.maintenancePort;
            checkMaintenancePortSet(conn, maintenancePort);
        });

        st.stop();
    });
});
