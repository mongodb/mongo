/**
 *  Test that connections on the priority port through TCP or Unix socket (only on non-Windows platforms)
 *  are exempt from connection (session) establishment rate limiting
 *
 * @tags: [
 *      # The priority port is based on ASIO, so gRPC testing is excluded
 *      grpc_incompatible,
 *      requires_replication,
 *      requires_sharding,
 *      requires_fcv_83,
 *      featureFlagReplicationUsageOfPriorityPort,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";

describe("Tests for priority port exemption from connection (session) establishment rate limiter within JS test replica set and sharded cluster helpers", function () {
    this.nonExemptIP = get_ipaddr();
    this.exemptIP = "127.0.0.1";

    this.kParamsConnectionEstablishmentRateLimiter = {
        ingressConnectionEstablishmentRatePerSec: 1,
        ingressConnectionEstablishmentBurstCapacitySecs: 1,
        ingressConnectionEstablishmentMaxQueueDepth: 0,
        ingressConnectionEstablishmentRateLimiterBypass: {ranges: [this.exemptIP]},
    };

    this.getEstablishmentRateLimitStats = (conn) => {
        const serverStatus = conn.adminCommand({serverStatus: 1});
        assert(serverStatus, "Failed to get server status");
        return serverStatus.connections.establishmentRateLimit;
    };

    this.testConnectionEstablishmentRateLimiter = (conn, rs) => {
        const exemptConn = new Mongo(`mongodb://${this.exemptIP}:${conn.port}`);
        // Enable connection (session) establishment request rate limiter. We enable the rate limiter
        // here rather than during test setup to ensure the test setup does not interfere with the
        // metrics being validated below, specifically the rejected connection count. If enabled earlier,
        // the _isSelf commands executed during test setup would trigger connection rejections,
        // incrementing the establishmentRateLimit counter. As a result, the rejected counter would
        // already be non-zero at this point, invalidating our test assumptions.
        assert.commandWorked(
            exemptConn
                .getDB("admin")
                .adminCommand({setParameter: 1, ingressConnectionEstablishmentRateLimiterEnabled: true}),
        );
        jsTest.log.info("Enabled connection (session) establishment rate limiter");

        // Make one non exempt connection on the main port (the only available token is consumed).
        const connToMainPort = new Mongo(`mongodb://${this.nonExemptIP}:${conn.port}`);
        assert.commandWorked(connToMainPort.getDB("admin").runCommand({ping: 1}));
        jsTest.log.info("Request to main port successful");

        // Since the ingressConnectionEstablishmentRatePerSec is set to one connection per second,
        // the following connection attempt will outpace the refreshRate and fail because queue
        // depth is set to zero.
        assert.soon(() => {
            try {
                const testConn = new Mongo(`mongodb://${this.nonExemptIP}:${conn.port}`);
            } catch (e) {
                jsTest.log.info("New connection to main port rejected with error: " + e);
                // Check that we exceed the rate limit by inspecting the logs and the related
                // error message
                return (
                    e.message.includes("Connection closed by peer") ||
                    e.message.includes("Connection reset by peer") ||
                    e.message.includes("established connection was aborted") ||
                    e.message.includes("Broken pipe")
                );
            }
            return false;
        });
        jsTest.log.info("New connection to main port rate limited as expected");

        // Check that the rejected count has increased
        assert.soon(() => {
            return this.getEstablishmentRateLimitStats(conn)["rejected"] == 1;
        });

        // Check new connection over a non exempt IP on the priority port
        // succeeds
        const connToPriorityPort = rs
            ? new Mongo(`mongodb://${this.nonExemptIP}:${rs.getPriorityPort(conn)}`)
            : new Mongo(`mongodb://${this.nonExemptIP}:${conn.priorityPort}`);
        assert.commandWorked(connToPriorityPort.getDB("admin").runCommand({ping: 1}));
        jsTest.log.info("Request to priority port successful");

        // Check that the exempted count has increased
        assert(this.getEstablishmentRateLimitStats(conn)["exempted"] >= 1);

        // Disable connection (session) establishment request rate limiter
        assert.commandWorked(
            exemptConn
                .getDB("admin")
                .adminCommand({setParameter: 1, ingressConnectionEstablishmentRateLimiterEnabled: false}),
        );
        jsTest.log.info("Disabled connection (session) establishment rate limiter");
    };

    it("Starting up a replica set with priority port enabled on all nodes", () => {
        const rs = new ReplSetTest({
            nodes: 3,
            usePriorityPorts: true,
            nodeOptions: {
                setParameter: {
                    ...this.kParamsConnectionEstablishmentRateLimiter,
                    featureFlagRateLimitIngressConnectionEstablishment: true,
                },
                config: "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
            },
        });
        rs.startSet();
        rs.initiate();

        // Wait for replica set to be fully initialized
        rs.awaitReplication();

        rs.nodes.forEach((conn) => {
            jsTest.log.info("Testing replica set node: " + conn.host);
            this.testConnectionEstablishmentRateLimiter(conn, rs);
        });

        rs.stopSet();
    });

    it("Starting up a sharded cluster with priority port enabled on all nodes", () => {
        const opts = {
            setParameter: {
                ...this.kParamsConnectionEstablishmentRateLimiter,
                featureFlagRateLimitIngressConnectionEstablishment: true,
            },
            config: "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
        };
        const st = new ShardingTest({
            shards: 1,
            mongos: 2,
            useHostname: false,
            usePriorityPorts: true,
            keyFile: this.keyFile,
            other: {mongosOptions: opts, rsOptions: opts, configOptions: opts},
        });

        // Wait for the cluster to be fully initialized
        st.configRS.awaitSecondaryNodes();
        st.rs0.awaitSecondaryNodes();

        st.configRS.nodes.forEach((conn) => {
            jsTest.log.info("Testing config server node: " + conn.host);
            this.testConnectionEstablishmentRateLimiter(conn, st.configRS);
        });

        st.rs0.nodes.forEach((conn) => {
            jsTest.log.info("Testing shard server node: " + conn.host);
            this.testConnectionEstablishmentRateLimiter(conn, st.rs0);
        });

        st._mongos.forEach((conn) => {
            jsTest.log.info("Testing mongos node: " + conn.host);
            this.testConnectionEstablishmentRateLimiter(conn);
        });

        st.stop();
    });
});
