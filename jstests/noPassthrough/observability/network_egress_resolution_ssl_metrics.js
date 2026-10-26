/**
 * Tests that network.egress.resolutionOperations advances when a node dials a peer, by forcing a
 * secondary to reconnect to the primary.
 *
 * @tags: [requires_replication]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("egress resolution and SSL operation counters", function () {
    let rs;
    let secondary;
    let initialResolution, finalResolution;

    before(function () {
        rs = new ReplSetTest({nodes: 2});
        rs.startSet();
        rs.initiate();
        rs.awaitSecondaryNodes();
        secondary = rs.getSecondary();

        const primary = rs.getPrimary();
        initialResolution = secondary.adminCommand({serverStatus: 1}).metrics.network.egress
            .resolutionOperations;

        // Dropping the connection forces a reconnect, which re-triggers host resolution.
        assert.commandWorked(
            secondary.adminCommand({dropConnections: 1, hostAndPort: [primary.host]}),
        );
        assert.commandWorked(primary.getDB("test").foo.insert({_id: 1}, {writeConcern: {w: 2}}));

        assert.soon(
            () => {
                finalResolution = secondary.adminCommand({serverStatus: 1}).metrics.network.egress
                    .resolutionOperations;
                return finalResolution > initialResolution;
            },
            () =>
                `resolutionOperations did not increase: initial ${initialResolution}, final ${finalResolution}`,
        );
    });

    after(function () {
        if (rs) {
            rs.stopSet();
        }
    });

    it("increments resolutionOperations when the secondary reconnects to the primary", function () {
        assert.gt(
            finalResolution,
            initialResolution,
            "Expected network.egress.resolutionOperations to increase after forcing a reconnect",
        );
    });
});
