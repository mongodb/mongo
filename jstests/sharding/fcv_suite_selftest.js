/*
 * Verifies that all replica sets in a sharded fixture run with the FCV expected by the suite and
 * variant.
 */

import {assertCurrentFCVMatchesSuite} from "jstests/libs/feature_compatibility_version.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("sharded FCV suite self-test", function () {
    it("runs with the expected FCV", function () {
        const st = new ShardingTest({shards: 1, mongos: 1});
        try {
            assertCurrentFCVMatchesSuite(st.s);
        } finally {
            st.stop();
        }
    });
});
