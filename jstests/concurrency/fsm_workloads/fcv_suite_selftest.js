/*
 * Verifies that the fixture is running with the FCV expected by its suite and variant.
 *
 * @tags: [incompatible_with_concurrency_simultaneous]
 */

import {assertCurrentFCVMatchesSuite} from "jstests/libs/feature_compatibility_version.js";

export const $config = {
    threadCount: 1,
    iterations: 1,
    startState: "init",
    states: {
        init: function (db, collName) {},
    },
    transitions: {
        init: {init: 1},
    },
    setup: function (db) {
        assertCurrentFCVMatchesSuite(db.getMongo());
    },
};
