/**
 * Test that we can connect to the maintenance port either through TCP or Unix socket (only on non-Windows
 * platforms) and they can handle connections in parallel with the main port.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   featureFlagDedicatedPortForMaintenanceOperations,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, before, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";

describe("Tests for maintenance port usage within JS test replica set helper", function () {
    before(function () {
        // Check that each node has the maintenance port and the corresponding unix socket set and listening
        this.checkMaintenancePortSetAndListening = (conn, rs) => {
            jsTest.log.info(`Checking maintenance port and corresponding unix socket are set and listening`);
            // Checking connection through TCP
            const connToMaintenancePort = rs
                ? rs.getNewConnectionToMaintenancePort(conn)
                : new Mongo(conn.host.split(":")[0] + ":" + conn.maintenancePort);
            const dbThroughPort = connToMaintenancePort.getDB("admin");
            assert.commandWorked(dbThroughPort.runCommand({ping: 1}));

            /**  TODO(SERVER-115111): Enable unix domain socket connection testing.
            if (!_isWindows()) {
                // Checking connection through unix socket
                const connToMaintenanceSocket = rs
                    ? rs.getNewConnectionToMaintenanceSocket(conn)
                    : new Mongo("/tmp/mongodb-" + conn.maintenancePort + ".sock");
                const dbThroughSocket = connToMaintenanceSocket.getDB("admin");
                assert.commandWorked(dbThroughSocket.runCommand({ping: 1}));
            }
            */
        };

        // Spawn a thread that 1) creates a connection, 2) issues a 'ping' command, and 3) closes
        // the connection. See `runParallelConnections`.
        this.spawnThreadForParallelConnections = (host, port) => {
            return new Thread(
                (host, port) => {
                    try {
                        const conn = new Mongo(host + ":" + port);

                        /** TODO(SERVER-115111): Enable unix domain socket connection testing.
                        // Randomly choose to connect via TCP or unix socket
                        const chooseTCP = Math.round(Math.random());
                        const conn =
                            _isWindows() || chooseTCP
                                ? new Mongo(host + ":" + port)
                                : new Mongo("/tmp/mongodb-" + port + ".sock");
                        */
                        const db = conn.getDB("admin");

                        assert.commandWorked(db.runCommand({ping: 1}));

                        conn.close();
                    } catch (opError) {
                        return {success: false, error: opError};
                    }
                    return {success: true};
                },
                host,
                port,
            );
        };

        // Check that we can run parallel connections on both main and maintenance ports without
        // race conditions
        this.runParallelConnections = (conn, rs) => {
            jsTest.log.info(`Checking parallel connections on main and maintenance ports`);
            const host = conn.host.split(":")[0];
            const workers = [];

            // Spawning threads for both main and maintenance ports (a low number of threads allows to check that
            // we are not triggering any thread sanitizer race condition while avoiding port exhaustion)
            const numThreadsPerPort = 4;
            for (let i = 0; i < numThreadsPerPort; i++) {
                workers.push({portName: "main", thread: this.spawnThreadForParallelConnections(host, conn.port)});
            }
            for (let i = 0; i < numThreadsPerPort; i++) {
                workers.push({
                    portName: "maintenance",
                    thread: this.spawnThreadForParallelConnections(
                        host,
                        rs ? rs.getMaintenancePort(conn) : conn.maintenancePort,
                    ),
                });
            }

            // Running parallel connections on both main and maintenance ports
            workers.forEach((worker) => worker.thread.start());

            // Joining threads
            for (const {portName, thread} of workers) {
                thread.join();
                const result = thread.returnData();
                assert(
                    result && result.success,
                    `Error while running multiple connection test on ${portName} port: ${tojson(result)}`,
                );
            }
        };
    });

    it("Starting up a replica set with maintenance port enabled on all nodes", () => {
        const rs = new ReplSetTest({
            nodes: 3,
            useMaintenancePorts: true,
        });
        rs.startSet();
        rs.initiate();

        jsTest.log.info("Testing replica set nodes for maintenance port availability");
        rs.nodes.forEach((conn) => {
            this.checkMaintenancePortSetAndListening(conn, rs);
            this.runParallelConnections(conn, rs);
        });

        rs.stopSet();
    });

    it("Starting up a replica set with a specific node having a maintenance port", () => {
        // The choice of which node has the maintenance port is arbitrary. Here we choose the second node
        // (index 1). The purpose of this test is to verify that only the node with the maintenance port
        // set can be listening to the port.
        const rs = new ReplSetTest({nodes: [{}, {maintenancePort: allocatePort()}, {}]});
        rs.startSet();
        rs.initiate();

        let countFoundPorts = 0;

        jsTest.log.info("Testing replica set nodes for maintenance port availability when it is set");
        rs.nodes.forEach((conn) => {
            try {
                this.checkMaintenancePortSetAndListening(conn, rs);
                countFoundPorts += 1;
            } catch (e) {
                // This should throw if the maintenance port is not set for the node.
                assert(!conn.maintenancePort);
            }
        });

        assert.eq(countFoundPorts, 1);

        jsTest.log.info("Testing parallel connections to maintenance port on the node that has it set");
        this.runParallelConnections(rs.nodes[1], rs);

        rs.stopSet();
    });

    it("Starting up a sharded cluster with maintenance port enabled on all nodes", () => {
        const st = new ShardingTest({
            shards: 1,
            mongos: 2,
            useMaintenancePorts: true,
        });

        jsTest.log.info("Testing config server nodes");
        st.configRS.nodes.forEach((conn) => {
            this.checkMaintenancePortSetAndListening(conn, st.configRS);
            this.runParallelConnections(conn, st.configRS);
        });

        jsTest.log.info("Testing shard server nodes");
        st.rs0.nodes.forEach((conn) => {
            this.checkMaintenancePortSetAndListening(conn, st.rs0);
            this.runParallelConnections(conn, st.rs0);
        });

        jsTest.log.info("Testing mongos nodes");
        st._mongos.forEach((conn) => {
            this.checkMaintenancePortSetAndListening(conn);
            this.runParallelConnections(conn);
        });

        st.stop();
    });
});
