/**
 * Tests that a primary-driven index build is aborted, not resumed, when the step-up that would
 * resume it could not create a pending interceptor for it.
 *
 * Without one, writes accepted between the step-up and the build setting itself up produce no
 * index keys and no side writes, and no resume phase re-derives them. Resuming would commit an
 * index missing those keys, so the build has to be aborted instead.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = jsTestName();
const collName = "coll";

describe("primary-driven index build with no pending interceptor", function () {
    before(() => {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({testName: jsTestName()});
    });

    after(() => {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("is aborted on step-up instead of resumed", () => {
        PrimaryDrivenResumableIndexBuildTest.runWithoutPendingInterceptors(this.rst, {
            dbName,
            collName,
        });
    });
});
