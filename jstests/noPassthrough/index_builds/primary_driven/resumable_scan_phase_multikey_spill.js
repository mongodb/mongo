/**
 * Tests resuming a primary-driven index build in the scan phase when a sorter spill lands in the
 * middle of a single document's index keys.
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
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("resumable primary-driven index build spilling mid-document", function () {
    before(function () {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp();
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
            ...MULTIKEY_SPILLING_OPTIONS,
        });
    });
});
