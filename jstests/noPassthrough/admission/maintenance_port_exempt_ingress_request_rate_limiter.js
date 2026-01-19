/**
 * Test that connections on the maintenance port through TCP or Unix socket (only on non-Windows
 * platforms) are exempt from ingress admission rate limiting
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_fcv_83,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertContainsExpectedErrorLabels} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

describe("Tests for maintenance port exemption from ingress request rate limiter within JS test replica set and sharded cluster helpers", function () {
    this.maxBurstRequests = 5;
    // An abnormally slow refresh rate to assure that a refresh doesn't trigger during the
    // test
    this.kSlowestRefreshRateSecs = 5e-6;
    this.maxBurstCapacitySecs = this.maxBurstRequests / this.kSlowestRefreshRateSecs;
    this.exemptAppName = "ApplicationExemptFromRateLimiting";
    this.kParamsIngressRequestRateLimiter = {
        // This value will be overwritten by the fail point
        ingressRequestAdmissionRatePerSec: 1,
        ingressRequestAdmissionBurstCapacitySecs: this.maxBurstCapacitySecs,
        // Keep ingress request rate limiter disabled during setup
        ingressRequestRateLimiterEnabled: false,
        logComponentVerbosity: tojson({command: 2}),
        "failpoint.ingressRequestRateLimiterFractionalRateOverride": tojson({
            mode: "alwaysOn",
            data: {rate: this.kSlowestRefreshRateSecs},
        }),
        // Exempt test application and replication components from token consumption
        // to prevent token usage when retrieving server status and issuing replication-related
        // heartbeats commands
        ingressRequestRateLimiterApplicationExemptions: {
            appNames: [this.exemptAppName, "OplogFetcher", "NetworkInterfaceTL-Repl"],
        },
    };
    this.extraRequests = 3;
    this.maintenancePortRequests = 3;

    this.keyFile = "jstests/libs/key1";

    this.createAdminUser = (conn) => {
        const directConnection = conn.getDB("admin");
        directConnection.createUser({user: "admin", pwd: "x", roles: ["root"]});
        assert(directConnection.auth("admin", "x"), "Authentication admin user failed when creating admin user");
        directConnection.logout();
    };

    this.createRegularUser = (conn) => {
        assert(conn.getDB("admin").auth("admin", "x"), "Authentication admin user failed when creating regular user");
        conn.getDB("admin").createUser({user: "user", pwd: "y", roles: ["clusterAdmin"]});
        conn.getDB("admin").logout();
    };

    this.testIngressRequestRateLimiter = (conn, rs) => {
        // Creating connections to main and maintenance port, as well as maintenance unix
        // socket if not on Windows system
        const connToMainPort = new Mongo(conn.host);
        assert(connToMainPort.getDB("admin").auth("user", "y"), "Authentication on main port for regular user failed");

        const connToMaintenancePort = rs
            ? rs.getNewConnectionToMaintenancePort(conn)
            : new Mongo(conn.host.split(":")[0] + ":" + conn.maintenancePort);
        assert(
            connToMaintenancePort.getDB("admin").auth("user", "y"),
            "Authentication on maintenance port for regular user failed",
        );

        /**  TODO(SERVER-115111): Enable unix domain socket connection testing.
        let connToMaintenanceSocket = null;
        if (!_isWindows()) {
            connToMaintenanceSocket = rs
                ? rs.getNewConnectionToMaintenanceSocket(conn)
                : new Mongo("/tmp/mongodb-" + conn.maintenancePort + ".sock");
            assert(
                connToMaintenanceSocket.getDB("admin").auth("user", "y"),
                "Authentication on maintenance socket for regular user failed",
            );
        }
        */

        const exemptConn = new Mongo(`mongodb://${conn.host}/?appName=${this.exemptAppName}`);
        assert(exemptConn.getDB("admin").auth("admin", "x"), "Authentication admin user failed on exempt connection");

        // Enable ingress request rate limiter (increasing the exemptedAdmissions metric counter by 1)
        assert.commandWorked(
            exemptConn.getDB("admin").adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: true}),
        );
        jsTest.log.info("Enabled ingress request rate limiter");

        // Check that the first 'maxBurstRequests' requests on the main port are successful, while the following 'extraRequests' are rate limited
        for (let i = 0; i < this.maxBurstRequests + this.extraRequests; i++) {
            const result = connToMainPort.getDB("admin").runCommand({ping: 1});
            if (result.ok === 0) {
                assert.commandFailedWithCode(result, ErrorCodes.IngressRequestRateLimitExceeded);
                assertContainsExpectedErrorLabels(result);
                jsTest.log.info(
                    "Request number " +
                        (this.maxBurstRequests + 1) +
                        "/" +
                        this.maxBurstRequests +
                        " to main port rate limited as expected",
                );
            } else {
                jsTest.log.info("Request number " + (i + 1) + "/" + this.maxBurstRequests + " to main port successful");
            }
        }

        // Store the amount of exempted admissions before making requests to maintenance port
        const initialStatus = exemptConn.adminCommand({serverStatus: 1});
        assert(initialStatus, "Failed to get initial server status");
        const initialExemptedAdmissions = initialStatus.network.ingressRequestRateLimiter.exemptedAdmissions;

        // Test that when hitting the ingress request rate limiting, connections to the
        // maintenance port through TCP and unix socket are exempted from rate limiting
        for (let i = 0; i < this.maintenancePortRequests; i++) {
            assert.commandWorked(connToMaintenancePort.getDB("admin").runCommand({ping: 1}));
            jsTest.log.info(
                "Request number " + (i + 1) + "/" + this.maintenancePortRequests + " to maintenance port successful",
            );
        }

        /**  TODO(SERVER-115111): Enable unix domain socket connection testing.
        // Test that connections to maintenance unix socket are successful
        if (!_isWindows()) {
            for (let i = 0; i < this.maintenancePortRequests; i++) {
                assert.commandWorked(connToMaintenanceSocket.getDB("admin").runCommand({ping: 1}));
                jsTest.log.info(
                    "Request number " +
                        (i + 1) +
                        "/" +
                        this.maintenancePortRequests +
                        " to maintenance unix socket successful",
                );
            }
        }
        */

        // Check that the exemptedAdmissions metric is consistent
        const finalStatus = exemptConn.adminCommand({serverStatus: 1});
        assert(finalStatus, "Failed to get final server status");
        const finalExemptedAdmissions = finalStatus.network.ingressRequestRateLimiter.exemptedAdmissions;
        jsTest.log.info("Final ingress request rate limiter stats: " + finalExemptedAdmissions);
        assert(finalExemptedAdmissions - initialExemptedAdmissions >= this.maintenancePortRequests);

        // Closing all the connections
        connToMainPort.close();
        connToMaintenancePort.close();
        /**  TODO(SERVER-115111): Enable unix domain socket connection testing.
        if (!_isWindows()) {
            connToMaintenanceSocket.close();
        }
        */

        // Disable ingress rate limiter
        assert.commandWorked(
            exemptConn.getDB("admin").adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: false}),
        );
        jsTest.log.info("Disabled ingress request rate limiter");

        exemptConn.getDB("admin").logout();
        exemptConn.close();
    };

    it("Starting up a replica set with maintenance port enabled on all nodes", () => {
        const rs = new ReplSetTest({
            nodes: 3,
            useMaintenancePorts: true,
            keyFile: this.keyFile,
            nodeOptions: {
                auth: "",
                setParameter: {...this.kParamsIngressRequestRateLimiter},
            },
        });
        rs.startSet();
        rs.initiate();

        jsTest.log.info("Replica set initiated");

        jsTest.log.info("Creating users for replica set");
        this.createAdminUser(rs.getPrimary());
        this.createRegularUser(rs.getPrimary());

        // Wait for user replication to complete
        rs.awaitReplication();

        jsTest.log.info("Testing replica set nodes");
        rs.nodes.forEach((conn) => {
            this.testIngressRequestRateLimiter(conn, rs);
        });

        rs.stopSet();
    });

    it("Starting up a sharded cluster with maintenance port enabled on all nodes", () => {
        const opts = {
            setParameter: {...this.kParamsIngressRequestRateLimiter},
        };
        const st = new ShardingTest({
            shards: 1,
            mongos: 2,
            useMaintenancePorts: true,
            other: {keyFile: this.keyFile, auth: "", mongosOptions: opts, rsOptions: opts, configOptions: opts},
        });

        jsTest.log.info("Creating users for config server");
        this.createAdminUser(st.configRS.getPrimary());
        this.createRegularUser(st.configRS.getPrimary());

        jsTest.log.info("Creating users for shard server");
        this.createAdminUser(st.rs0.getPrimary());
        this.createRegularUser(st.rs0.getPrimary());

        // Wait for the cluster to be fully initialized
        st.configRS.awaitSecondaryNodes();
        st.rs0.awaitSecondaryNodes();

        jsTest.log.info("Testing config server nodes");
        st.configRS.nodes.forEach((conn) => {
            this.testIngressRequestRateLimiter(conn, st.configRS);
        });

        jsTest.log.info("Testing shard server nodes");
        st.rs0.nodes.forEach((conn) => {
            this.testIngressRequestRateLimiter(conn, st.rs0);
        });

        jsTest.log.info("Testing mongos nodes");
        st._mongos.forEach((conn) => {
            this.testIngressRequestRateLimiter(conn);
        });

        st.stop();
    });
});
