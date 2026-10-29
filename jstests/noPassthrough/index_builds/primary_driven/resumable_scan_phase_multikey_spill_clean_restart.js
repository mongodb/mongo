/**
 * Tests resuming a primary-driven index build in the scan phase when a sorter spill lands in the
 * middle of a single document's index keys, with the old primary cleanly restarted across each
 * failover.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    MULTIKEY_SPILLING_OPTIONS,
    PdibFailoverMode,
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("resumable primary-driven index build spilling mid-document across a clean restart", function () {
    before(function () {
        // The cleanly-restarted primary is taken down before the secondary is stepped up, so a
        // third node is needed to form a majority for that election.
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
    });

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("indexes every key of a document split by a spill", function () {
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.SCAN,
            // Only the MIDDLE and END positions are exercised: at the BEGINNING of the scan the
            // build pauses before any key is inserted, so no spill has happened yet and there is
            // nothing to skip.
            positions: [PdibPosition.MIDDLE, PdibPosition.END],
            failoverMode: PdibFailoverMode.CLEAN_RESTART,
            ...MULTIKEY_SPILLING_OPTIONS,
        });
    });
});
