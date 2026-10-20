// Verifies that the TLS handshake latency histogram is populated end to end.

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {CA_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

describe("Ingress TLS handshake latency histogram", function () {
    let mongod;

    // preferTLS lets us open both TLS and non-TLS connections to the same mongod.
    before(function () {
        mongod = MongoRunner.runMongod({
            tlsMode: "preferTLS",
            tlsCertificateKeyFile: SERVER_CERT,
            tlsCAFile: CA_CERT,
        });
        assert.neq(null, mongod, "mongod failed to start");
    });

    after(function () {
        if (mongod) {
            MongoRunner.stopMongod(mongod);
        }
    });

    function tlsHandshakeHistogram(conn) {
        const res = assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
        return res.metrics.network.ingressTLSHandshakeMicros;
    }

    it("does not record a sample for non-TLS connections", function () {
        const baseline = tlsHandshakeHistogram(mongod);
        assert(baseline, "TLS handshake latency histogram missing from serverStatus");

        // The ssl suite configures the shell with --tls globally; tls=false in the
        // URI overrides it to make a plaintext connection.
        const conn = new Mongo("mongodb://" + mongod.host + "/?tls=false");
        assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));

        const histogram = tlsHandshakeHistogram(conn);
        assert.eq(
            Number(baseline.totalCount),
            histogram.totalCount,
            "expected no new TLS handshake latency samples for the non-TLS connection",
        );
    });

    it("records exactly one sample for each new TLS connection", function () {
        const baseline = tlsHandshakeHistogram(mongod);
        assert(baseline, "TLS handshake latency histogram missing from serverStatus");

        const conn = new Mongo(mongod.host);
        assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));

        const histogram = tlsHandshakeHistogram(conn);
        assert.eq(
            Number(baseline.totalCount) + 1,
            histogram.totalCount,
            "expected exactly one new TLS handshake latency sample for the new TLS connection",
        );
    });
});
