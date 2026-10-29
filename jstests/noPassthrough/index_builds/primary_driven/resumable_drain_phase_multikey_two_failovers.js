/**
 * Tests a primary-driven index build that fails over twice: the node that drains the writes making
 * the indexes multikey is not the node that commits the build.
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
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

describe("primary-driven index build made multikey, committed two failovers later", function () {
    before(function () {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
    });

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("commits the indexes as multikey", function () {
        PrimaryDrivenResumableIndexBuildTest.runAcrossTwoFailovers(this.rst, {
            ...MULTIKEY_SIDE_WRITE_OPTIONS,
            expectMultikey: true,
        });
    });
});
