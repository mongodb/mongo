/*
 * Verifies that a replica set fixture is running with the FCV expected by its suite and variant.
 */

import {assertCurrentFCVMatchesSuite} from "jstests/libs/feature_compatibility_version.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replica-set FCV suite self-test", function () {
    it("runs with the expected FCV", function () {
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        try {
            assertCurrentFCVMatchesSuite(rst.getPrimary());
        } finally {
            rst.stopSet();
        }
    });
});
