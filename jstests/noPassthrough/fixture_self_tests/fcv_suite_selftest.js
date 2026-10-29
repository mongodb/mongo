/*
 * Verifies that a fixture created by a noPassthrough test is running with the expected FCV.
 */

import {assertCurrentFCVMatchesSuite} from "jstests/libs/feature_compatibility_version.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("noPassthrough FCV suite self-test", function () {
    it("runs a standalone mongod with the expected FCV", function () {
        const conn = MongoRunner.runMongod();
        try {
            assertCurrentFCVMatchesSuite(conn);
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    it("runs a replica set with the expected FCV", function () {
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        try {
            assertCurrentFCVMatchesSuite(rst.getPrimary());
        } finally {
            rst.stopSet();
        }
    });

    it("runs a sharded cluster with the expected FCV", function () {
        const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
        try {
            assertCurrentFCVMatchesSuite(st.s);
        } finally {
            st.stop();
        }
    });
});
