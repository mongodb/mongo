/**
 * Tests resuming a primary-driven index build in the drain phase when writes made after the
 * collection scan are what first make the indexes multikey.
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
    PdibPhase,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("resumable primary-driven index build made multikey by drained side writes", function () {
    before(function () {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp();
    });

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("commits the indexes as multikey", function () {
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.DRAIN,
            ...MULTIKEY_SIDE_WRITE_OPTIONS,
            expectMultikey: true,
        });
    });
});
