/**
 * Tests resuming a primary-driven index build in the drain phase when writes made after the
 * collection scan are what first make the indexes multikey, with the old primary uncleanly restarted
 * across each failover.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    MULTIKEY_SIDE_WRITE_OPTIONS,
    PdibFailoverMode,
    PdibPhase,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("resumable primary-driven index build made multikey by drained writes across a unclean restart", function () {
    before(function () {
        // The uncleanly-restarted primary is taken down before the secondary is stepped up, so a
        // third node is needed to form a majority for that election.
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
    });

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("commits the indexes as multikey", function () {
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.DRAIN,
            failoverMode: PdibFailoverMode.UNCLEAN_RESTART,
            ...MULTIKEY_SIDE_WRITE_OPTIONS,
            expectMultikey: true,
        });
    });
});
