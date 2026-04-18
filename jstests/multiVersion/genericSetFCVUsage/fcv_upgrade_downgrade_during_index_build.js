/**
 * Tests that fcv change is forbidden during index build
 *
 * @tags: [
 * ]
 */

import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

describe("fcv change forbidden during index build", function () {
    before(() => {
        this.startIndexBuild = () => {
            this.awaitCreateIdxJob = IndexBuildTest.startIndexBuild(this.rs.getPrimary(), this.coll.getFullName(), {
                a: 1,
            });
            this.fp.wait();
        };
    });

    beforeEach(() => {
        this.rs = new ReplSetTest({nodes: 1});
        this.rs.startSet();
        this.rs.initiate();

        this.testDB = this.rs.getPrimary().getDB("test");
        this.coll = this.testDB.getCollection("test");
        assert.commandWorked(this.coll.insertOne({a: 1}));

        this.fp = configureFailPoint(this.testDB, "hangIndexBuildBeforeCommit");
    });

    afterEach(() => {
        this.fp.off();
        this.awaitCreateIdxJob();

        this.rs.stopSet();
    });

    it("upgrade", () => {
        assert.commandWorked(
            this.rs.getPrimary().getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        this.startIndexBuild(this.rs.getPrimary());

        assert.commandFailedWithCode(
            this.rs.getPrimary().getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            ErrorCodes.BackgroundOperationInProgressForNamespace,
        );
    });

    it("downgrade", () => {
        assert.commandWorked(
            this.rs.getPrimary().getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        this.startIndexBuild(this.rs.getPrimary());

        assert.commandFailedWithCode(
            this.rs.getPrimary().getDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            ErrorCodes.BackgroundOperationInProgressForNamespace,
        );
    });
});
