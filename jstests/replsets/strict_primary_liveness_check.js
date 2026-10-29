/**
 * Tests the 'enableStrictPrimaryLivenessCheck' server parameter.
 *
 * A primary whose network stack still replies to inbound heartbeats, but which is unable to
 * initiate any work of its own, is simulated with the 'pauseSendingOutgoingHeartbeats' failpoint.
 * With the strict check disabled, the secondaries keep postponing their election timeouts forever
 * because the heartbeat *responses* they get back look healthy. With the strict check enabled, a
 * secondary additionally requires a recent heartbeat *request* from the primary, so it times out
 * and steps up.
 *
 * The 'pauseSendingOutgoingHeartbeats' failpoint and the server parameter only exist on the latest
 * binaries.
 *
 * @tags: [
 *     multiversion_incompatible,
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const electionTimeoutMillis = 5 * 1000;
// Doubles as how long we observe the set before concluding that no election happened, and as the
// upper bound on a step-up (the election timeout, plus the dry run and the real vote round).
const electionObservationMillis = 5 * electionTimeoutMillis;

describe("strict primary liveness check", function () {
    before(function () {
        this.rst = new ReplSetTest({
            name: jsTestName(),
            nodes: 3,
            settings: {electionTimeoutMillis: electionTimeoutMillis, heartbeatIntervalMillis: 500},
        });
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.secondaries = this.rst.getSecondaries();

        this.setStrictCheck = (enabled) =>
            this.rst.nodes.forEach((node) =>
                assert.commandWorked(
                    node.adminCommand({setParameter: 1, enableStrictPrimaryLivenessCheck: enabled}),
                ),
            );

        // Wedge the primary: it stops sending heartbeats to anyone, but still replies to the
        // heartbeats it receives. Also stop the secondaries from fetching oplog entries, since a
        // live oplog stream from the primary is an independent (and legitimate) reason to postpone
        // the election timeout, and a truly wedged primary would not be serving getMores either.
        this.stopProducerFailPoints = this.secondaries.map((node) =>
            configureFailPoint(node, "stopReplProducer"),
        );
        this.pauseHeartbeats = configureFailPoint(this.primary, "pauseSendingOutgoingHeartbeats");
    });

    after(function () {
        // Let the set heal so that the usual shutdown consistency checks can run.
        this.pauseHeartbeats.off();
        this.stopProducerFailPoints.forEach((fp) => fp.off());
        this.setStrictCheck(true);
        this.rst.awaitNodesAgreeOnPrimary();
        this.rst.awaitReplication();
        this.rst.stopSet();
    });

    it("does not elect a new primary when the check is disabled", function () {
        this.setStrictCheck(false);
        sleep(electionObservationMillis);
        this.secondaries.forEach((node) => {
            assert(
                !node.adminCommand({hello: 1}).isWritablePrimary,
                "secondary unexpectedly stepped up with the strict check disabled",
                {host: node.host},
            );
        });
        assert(
            this.primary.adminCommand({hello: 1}).isWritablePrimary,
            "primary unexpectedly stepped down with the strict check disabled",
            {host: this.primary.host},
        );
    });

    it("elects a new primary when the check is enabled", function () {
        this.setStrictCheck(true);
        assert.soon(
            () => this.secondaries.some((node) => node.adminCommand({hello: 1}).isWritablePrimary),
            "no secondary stepped up while the primary was wedged and the strict check was enabled",
            electionObservationMillis,
        );

        // The wedged primary learns about the new term from the heartbeats it answers, so it also
        // steps down.
        assert.soon(
            () => !this.primary.adminCommand({hello: 1}).isWritablePrimary,
            "the wedged primary did not step down after a secondary stepped up",
            electionObservationMillis,
        );
    });
});
