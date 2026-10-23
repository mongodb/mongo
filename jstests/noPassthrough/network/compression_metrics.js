// serverStatus 'network.compression' ingress/egress counters; legacy fields sum both.
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let mongod = null;
let replSet = null;

// The legacy per-compressor fields are the sum over both connection roles.
function assertLegacyFieldsSumBothRoles(stats) {
    for (const side of ["compressor", "decompressor"]) {
        assert.eq(
            stats[side].bytesIn,
            stats.ingress[side].bytesIn + stats.egress[side].bytesIn,
            "legacy bytesIn does not sum ingress and egress",
            {stats},
        );
        assert.eq(
            stats[side].bytesOut,
            stats.ingress[side].bytesOut + stats.egress[side].bytesOut,
            "legacy bytesOut does not sum ingress and egress",
            {stats},
        );
    }
}

describe("serverStatus network.compression", function () {
    afterEach(function () {
        if (replSet) {
            replSet.stopSet();
            replSet = null;
        }
        if (mongod) {
            MongoRunner.stopMongod(mongod);
            mongod = null;
        }
    });

    it("reports per-compressor counters split by ingress and egress", function () {
        mongod = MongoRunner.runMongod({networkMessageCompressors: "snappy"});

        // The server decompresses these requests and compresses the responses, both ingress.
        const awaitShell = startParallelShell(
            function () {
                const coll = db.getSiblingDB("compression_metrics").coll;
                for (let i = 0; i < 5; ++i) {
                    assert.commandWorked(coll.insert({a: "x".repeat(500)}));
                }
            },
            mongod.port,
            false,
            "--networkMessageCompressors",
            "snappy",
        );
        awaitShell();

        // mongod has no outbound client connections, so egress counts are zero.
        const res = assert.commandWorked(mongod.getDB("admin").runCommand({serverStatus: 1}));
        assert(res.network.compression, "compression section missing", {network: res.network});
        assert(res.network.compression.snappy, "snappy section missing", {
            compression: res.network.compression,
        });

        const snappy = res.network.compression.snappy;
        assert.gte(snappy.ingress.decompressor.bytesIn, 1, "no decompressed ingress requests", {
            snappy,
        });
        assert.gte(snappy.ingress.compressor.bytesOut, 1, "no compressed ingress responses", {
            snappy,
        });
        assert.eq(snappy.egress.compressor.bytesIn, 0, "egress compressor bytesIn is nonzero", {
            snappy,
        });
        assert.eq(snappy.egress.compressor.bytesOut, 0, "egress compressor bytesOut is nonzero", {
            snappy,
        });
        assert.eq(snappy.egress.decompressor.bytesIn, 0, "egress decompressor bytesIn is nonzero", {
            snappy,
        });
        assert.eq(
            snappy.egress.decompressor.bytesOut,
            0,
            "egress decompressor bytesOut is nonzero",
            {snappy},
        );

        assertLegacyFieldsSumBothRoles(snappy);
    });

    it("reports egress counters on outbound connections", function () {
        // Replica set nodes exchange heartbeats on outbound connections, which negotiate
        // the configured compressor.
        replSet = new ReplSetTest({nodes: 2, nodeOptions: {networkMessageCompressors: "snappy"}});
        replSet.startSet();
        replSet.initiate();
        replSet.awaitSecondaryNodes();

        const primary = replSet.getPrimary();
        const serverStatusSnappy = () =>
            assert.commandWorked(primary.getDB("admin").runCommand({serverStatus: 1})).network
                .compression.snappy;

        // Heartbeat requests are compressed and the responses come back compressed.
        assert.soon(() => {
            const egress = serverStatusSnappy().egress;
            return egress.compressor.bytesOut > 0 && egress.decompressor.bytesIn > 0;
        }, "no compressed egress traffic");

        const egress = serverStatusSnappy().egress;
        assert.gte(egress.compressor.bytesIn, 1, "no compressed egress request bytesIn", {egress});
        assert.gte(egress.compressor.bytesOut, 1, "no compressed egress request bytesOut", {
            egress,
        });
        assert.gte(egress.decompressor.bytesIn, 1, "no compressed egress response bytesIn", {
            egress,
        });
        assert.gte(egress.decompressor.bytesOut, 1, "no compressed egress response bytesOut", {
            egress,
        });
    });
});
