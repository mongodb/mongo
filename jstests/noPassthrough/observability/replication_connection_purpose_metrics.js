/**
 * Tests that replication connections correctly set their connectionPurpose and that their metrics are
 * under network.repl.secondary.
 *
 * @tags: [requires_replication, requires_otel_build, multiversion_incompatible]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kPayloadSize = 1024;

describe("replication-specific network metrics", function () {
    let rs;
    let primary;
    let secondary;

    before(function () {
        // Setup replication test, two nodes, one default (primary), and one that can never become the primary
        rs = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
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

    describe("records bytes sent over replication connections", function () {
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
});
