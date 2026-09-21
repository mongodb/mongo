/**
 * Tests resuming a primary-driven index build in the drain phase when the only thing making the
 * indexes multikey is an update that leaves the document's key set untouched -- each scalar becomes
 * a single-element array holding the same value.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    MULTIKEY_UNCHANGED_KEYS_OPTIONS,
    PdibPhase,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("resumable primary-driven index build made multikey without new keys", function () {
    before(function () {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp();
    });

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("commits the indexes as multikey", function () {
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.DRAIN,
            ...MULTIKEY_UNCHANGED_KEYS_OPTIONS,
            expectMultikey: true,
        });
    });
});
