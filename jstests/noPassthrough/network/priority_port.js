/**
 * Test that we can connect to the priority port either through TCP or Unix socket (only on non-Windows
 * platforms) and they can handle connections in parallel with the main port.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_fcv_83,
 *   featureFlagReplicationUsageOfPriorityPort,
 *   # The priority port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, before, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";

describe("Tests for priority port usage within JS test replica set helper", function () {
    before(function () {
        // Check that each node has the priority port and the corresponding unix socket set and listening
        this.checkPriorityPortSetAndListening = (conn, rs) => {
            jsTest.log.info(`Checking priority port and corresponding unix socket are set and listening`);
            // Checking connection through TCP
            const connToPriorityPort = rs
                ? rs.getNewConnectionToPriorityPort(conn)
                : new Mongo(conn.host.split(":")[0] + ":" + conn.priorityPort);
            const dbThroughPort = connToPriorityPort.getDB("admin");
            assert.commandWorked(dbThroughPort.runCommand({ping: 1}));

            /**  TODO(SERVER-115111): Enable unix domain socket connection testing.
            if (!_isWindows()) {
                // Checking connection through unix socket
                const connToPrioritySocket = rs
                    ? rs.getNewConnectionToPrioritySocket(conn)
                    : new Mongo("/tmp/mongodb-" + conn.priorityPort + ".sock");
                const dbThroughSocket = connToPrioritySocket.getDB("admin");
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

        // Check that we can run parallel connections on both main and priority ports without
        // race conditions
        this.runParallelConnections = (conn, rs) => {
            jsTest.log.info(`Checking parallel connections on main and priority ports`);
            const host = conn.host.split(":")[0];
            const workers = [];

            // Spawning threads for both main and priority ports (a low number of threads allows to check that
            // we are not triggering any thread sanitizer race condition while avoiding port exhaustion)
            const numThreadsPerPort = 4;
            for (let i = 0; i < numThreadsPerPort; i++) {
                workers.push({portName: "main", thread: this.spawnThreadForParallelConnections(host, conn.port)});
            }
            for (let i = 0; i < numThreadsPerPort; i++) {
                workers.push({
                    portName: "priority",
                    thread: this.spawnThreadForParallelConnections(
                        host,
                        rs ? rs.getPriorityPort(conn) : conn.priorityPort,
                    ),
                });
            }

            // Running parallel connections on both main and priority ports
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

    it("Starting up a replica set with priority port enabled on all nodes", () => {
        const rs = new ReplSetTest({
            nodes: 3,
            usePriorityPorts: true,
        });
        rs.startSet();
        rs.initiate();

        jsTest.log.info("Testing replica set nodes for priority port availability");
        rs.nodes.forEach((conn) => {
            this.checkPriorityPortSetAndListening(conn, rs);
            this.runParallelConnections(conn, rs);
        });

        rs.stopSet();
    });

    it("Starting up a replica set with a specific node having a priority port", () => {
        // The choice of which node has the priority port is arbitrary. Here we choose the second node
        // (index 1). The purpose of this test is to verify that only the node with the priority port
        // set can be listening to the port.
        const rs = new ReplSetTest({nodes: [{}, {priorityPort: allocatePort()}, {}]});
        rs.startSet();
        rs.initiate();

        let countFoundPorts = 0;

        jsTest.log.info("Testing replica set nodes for priority port availability when it is set");
        rs.nodes.forEach((conn) => {
            try {
                this.checkPriorityPortSetAndListening(conn, rs);
                countFoundPorts += 1;
            } catch (e) {
                // This should throw if the priority port is not set for the node.
                assert(!conn.priorityPort);
            }
        });

        assert.eq(countFoundPorts, 1);

        jsTest.log.info("Testing parallel connections to priority port on the node that has it set");
        this.runParallelConnections(rs.nodes[1], rs);

        rs.stopSet();
    });

    it("Starting up a sharded cluster with priority port enabled on all nodes", () => {
        const st = new ShardingTest({
            shards: 1,
            mongos: 2,
            usePriorityPorts: true,
        });

        jsTest.log.info("Testing config server nodes");
        st.configRS.nodes.forEach((conn) => {
            this.checkPriorityPortSetAndListening(conn, st.configRS);
            this.runParallelConnections(conn, st.configRS);
        });

        jsTest.log.info("Testing shard server nodes");
        st.rs0.nodes.forEach((conn) => {
            this.checkPriorityPortSetAndListening(conn, st.rs0);
            this.runParallelConnections(conn, st.rs0);
        });

        jsTest.log.info("Testing mongos nodes");
        st._mongos.forEach((conn) => {
            this.checkPriorityPortSetAndListening(conn);
            this.runParallelConnections(conn);
        });

        st.stop();
    });
});
