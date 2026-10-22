/**
 * Verifies that the hello and auth latency histograms are populated end to end.
 *
 * @tags: [
 *   grpc_incompatible,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

function getNetworkMetrics(conn) {
    const res = assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
    return res.metrics.network;
}

describe("Ingress handshake latency histograms", function () {
    let mongod;
    let admin;

    before(function () {
        mongod = MongoRunner.runMongod({});
        assert.neq(null, mongod, "mongod failed to start");
        admin = mongod.getDB("admin");
    });

    after(function () {
        if (mongod) {
            MongoRunner.stopMongod(mongod);
        }
    });

    it("records a hello sample and no auth samples for a connection that does not authenticate", function () {
        const baseline = getNetworkMetrics(mongod);

        // The driver runs a hello command when creating a new connection.
        const conn = new Mongo(mongod.host);

        const network = getNetworkMetrics(conn);
        assert(network.ingressHelloLatency, "hello latency histogram missing from serverStatus");
        assert.eq(
            Number(baseline.ingressHelloLatency.totalCount) + 1,
            network.ingressHelloLatency.totalCount,
            "expected exactly one new hello latency sample for the new connection",
        );

        assert(network.ingressAuthLatency, "auth latency histogram missing from serverStatus");
        assert.eq(
            0,
            network.ingressAuthLatency.totalCount,
            "connections that do not authenticate must not record auth latency samples",
        );
    });

    it("records exactly one auth sample for a connection that authenticates immediately", function () {
        admin.createUser({user: "authUser", pwd: "password", roles: [{role: "root", db: "admin"}]});

        const baseline = getNetworkMetrics(mongod);

        const conn = new Mongo(mongod.host);
        assert(conn.getDB("admin").auth("authUser", "password"), "authentication failed");

        const network = getNetworkMetrics(conn);
        assert.eq(
            Number(baseline.ingressAuthLatency.totalCount) + 1,
            network.ingressAuthLatency.totalCount,
            "expected exactly one new auth latency sample for the authenticated connection",
        );
        assert.eq(
            Number(baseline.ingressHelloLatency.totalCount) + 1,
            network.ingressHelloLatency.totalCount,
            "expected exactly one new hello latency sample for the new connection",
        );
    });

    it("records exactly one auth sample for a connection that authenticates after the handshake", function () {
        const baseline = getNetworkMetrics(mongod);

        const conn = new Mongo(mongod.host);
        // A non-auth command ends the handshake on this connection; authenticating afterwards must
        // still record an auth sample.
        assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));
        assert(conn.getDB("admin").auth("authUser", "password"), "authentication failed");

        const network = getNetworkMetrics(conn);
        assert.eq(
            Number(baseline.ingressAuthLatency.totalCount) + 1,
            network.ingressAuthLatency.totalCount,
            "expected exactly one new auth latency sample for the authenticated connection",
        );
    });
});
