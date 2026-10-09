/*
 * Verifies that the fixture is running with the FCV expected by its suite and variant.
 *
 * @tags: [simulate_mongoq_incompatible]
 */

import {assertCurrentFCVMatchesSuite} from "jstests/libs/feature_compatibility_version.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FCV suite self-test", function () {
    it("runs with the expected FCV", function () {
        assertCurrentFCVMatchesSuite(db.getMongo());
    });
});
