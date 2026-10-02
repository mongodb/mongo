/**
 * Tests that replication connections correctly set their connectionPurpose and that their metrics are
 * under network.repl.secondary.
 *
 * @tags: [requires_replication, requires_otel_build, multiversion_incompatible]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    awaitMetrics,
    getCounterByAttribute,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

const kPayloadSize = 1024;

describe("replication-specific network metrics", function () {
    let rs;
    let primary;
    let secondary;
    let metricsDir;

    before(function () {
        const {metricsDir: dir, otelParams} = otelFileExportParams(jsTestName());
        metricsDir = dir;

        // Setup replication test, two nodes, one default (primary), and one that can never become the primary
        rs = new ReplSetTest({
            nodes: [{setParameter: otelParams}, {rsConfig: {priority: 0}}],
        });
        rs.startSet();
        rs.initiate();
        // Block until set is fully operational
        rs.awaitSecondaryNodes();
        primary = rs.getPrimary();
        secondary = rs.getSecondary();
    });

    after(function () {
        if (rs) {
            rs.stopSet();
        }
    });

    describe("records bytes served as secondary over replication connections", function () {
        let initialLogicalBytes, finalLogicalBytes;
        let initialPhysicalBytes, finalPhysicalBytes;
        let initialEgressBytes, finalEgressBytes;

        before(function () {
            const initialSecondaryStatus = assert.commandWorked(
                secondary.adminCommand({serverStatus: 1}),
            );
            initialLogicalBytes = initialSecondaryStatus.network.repl.secondary.bytesIn;
            initialPhysicalBytes = initialSecondaryStatus.network.repl.secondary.physicalBytesIn;
            initialEgressBytes = initialSecondaryStatus.network.egress.bytesIn;
            // write concern of 2 ensures that secondary fetches the data
            assert.commandWorked(
                primary
                    .getDB("test")
                    .foo.insert(
                        {_id: 1, payload: "a".repeat(kPayloadSize)},
                        {writeConcern: {w: 2}},
                    ),
            );

            const finalSecondaryStatus = assert.commandWorked(
                secondary.adminCommand({serverStatus: 1}),
            );
            finalLogicalBytes = finalSecondaryStatus.network.repl.secondary.bytesIn;
            finalPhysicalBytes = finalSecondaryStatus.network.repl.secondary.physicalBytesIn;
            finalEgressBytes = finalSecondaryStatus.network.egress.bytesIn;
        });

        it("increments the logical byte counter", function () {
            // logical bytes should increase by at least the size of the document
            assert.gte(
                finalLogicalBytes - kPayloadSize,
                initialLogicalBytes,
                "Secondary should have incremented logical byte counter",
            );
        });

        it("increments the physical byte counter", function () {
            // ensure physical counter increased by any amount to account for possible compression
            assert.gt(
                finalPhysicalBytes,
                initialPhysicalBytes,
                "Secondary should have incremented physical byte counter",
            );
        });

        it("increments normal egress byte counter", function () {
            assert.gte(
                finalEgressBytes - kPayloadSize,
                initialEgressBytes,
                "Replication traffic should be counted in normal egress counters as well",
            );
        });

        after(function () {
            assert.commandWorked(primary.getDB("test").dropDatabase());
        });
    });

    describe("records bytes served as source over replication connections", function () {
        const kSourceLogicalOut = "mongodb.network.repl.source.bytes_out";
        const kSourcePhysicalOut = "mongodb.network.repl.source.physical_bytes_out";

        const getLogicalOut = () =>
            getCounterByAttribute(metricsDir, kSourceLogicalOut, "peer_id", 0);
        const getPhysicalOut = () =>
            getCounterByAttribute(metricsDir, kSourcePhysicalOut, "peer_id", 0);
        let initialLogicalBytes, finalLogicalBytes;
        let initialPhysicalBytes, finalPhysicalBytes;
        let initialIngressBytes, finalIngressBytes;

        before(function () {
            initialLogicalBytes = getLogicalOut();
            initialPhysicalBytes = getPhysicalOut();
            initialIngressBytes = assert.commandWorked(primary.adminCommand({serverStatus: 1}))
                .network.bytesOut;

            assert.commandWorked(
                primary
                    .getDB("test")
                    .foo.insert(
                        {_id: 1, payload: "a".repeat(kPayloadSize)},
                        {writeConcern: {w: 2}},
                    ),
            );

            assert.soon(
                () =>
                    getLogicalOut() - initialLogicalBytes >= kPayloadSize &&
                    getPhysicalOut() > initialPhysicalBytes,
                () =>
                    `logical ${getLogicalOut()} (initial ${initialLogicalBytes}), physical ${getPhysicalOut()} (initial ${initialPhysicalBytes})`,
            );

            finalLogicalBytes = getLogicalOut();
            finalPhysicalBytes = getPhysicalOut();
            finalIngressBytes = assert.commandWorked(primary.adminCommand({serverStatus: 1}))
                .network.bytesOut;
        });

        it("increments logical byte counter", function () {
            assert.gte(
                finalLogicalBytes - kPayloadSize,
                initialLogicalBytes,
                "Primary should have incremented logical byte counter",
            );
        });

        it("increments physical byte counter", function () {
            assert.gt(
                finalPhysicalBytes,
                initialPhysicalBytes,
                "Primary should have incremented physical byte counter",
            );
        });

        it("increments normal ingress byte counter", function () {
            assert.gte(
                finalIngressBytes - kPayloadSize,
                initialIngressBytes,
                "Replication traffic should be counted in normal ingress counters as well",
            );
        });
        after(function () {
            assert.commandWorked(primary.getDB("test").dropDatabase());
        });
    });
});
