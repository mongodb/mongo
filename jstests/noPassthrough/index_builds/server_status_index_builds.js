/**
 * Tests that serverStatus contains an indexBuilds section with lastCommittedMillis.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    IndexBuildTest,
    ResumableIndexBuildTest
} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const collName = "t";
const dbName = "test";

function getLastCommittedMillis(conn) {
    const serverStatus = conn.serverStatus();

    assert(serverStatus.hasOwnProperty("indexBuilds"),
           "indexBuilds section missing: " + tojson(serverStatus));
    const indexBuilds = serverStatus.indexBuilds;

    assert(indexBuilds.hasOwnProperty("phases"), "phases missing: " + tojson(indexBuilds));
    const phases = indexBuilds.phases;

    assert(phases.hasOwnProperty("lastCommittedMillis"),
           "lastCommittedMillis missing: " + tojson(phases));
    return phases.lastCommittedMillis;
}

describe("indexBuilds serverStatus metrics", function() {
    before(() => {
        this.rst = new ReplSetTest({
            nodes: 1,
        });
    });

    beforeEach(() => {
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        assert.commandWorked(this.coll.insertMany(Array.from({length: 10}, () => ({a: "foo"}))));
    });

    afterEach(() => {
        this.rst.stopSet();
    });

    it("during steady state", () => {
        let duration = getLastCommittedMillis(this.db);
        assert.eq(
            duration, 0, `Expected lastCommittedMillis to be 0 on startup, but got ${duration}`);

        // Hang index build before completion to extend duration.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex =
            IndexBuildTest.startIndexBuild(this.primary, this.coll.getFullName(), {a: 1});

        // Initiate the failpoint and then sleep for 1000ms to ensure duration >= 1000ms.
        fp.wait();
        sleep(1000);
        fp.off();

        // Wait for the parallel shell to exit.
        awaitCreateIndex();

        duration = getLastCommittedMillis(this.db);
        assert.gt(
            duration,
            1000,
            `Expected lastCommittedMillis to be > 1000 after index build, but got ${duration}`);
    });

    it("on resume", () => {
        let duration = getLastCommittedMillis(this.db);
        assert.eq(
            duration, 0, `Expected lastCommittedMillis to be 0 on startup, but got ${duration}`);

        // Hang index build before completion to require resume.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.coll.getFullName(),
            {a: 1},
            {} /* options */,
            [ErrorCodes.InterruptedDueToReplStateChange],
        );

        // Ensure index build is in-progress before restarting, else it is not resumed.
        fp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true})["a_1"]
                .buildUUID,
        );

        // Restart to trigger resume.
        this.rst.restart(0);

        // Wait for the parallel shell to exit.
        awaitCreateIndex();

        // Connect to new primary.
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        // Ensure index build is completed before reading metrics.
        ResumableIndexBuildTest.assertCompleted(this.primary, this.coll, [buildUUID], ["a_1"]);

        duration = getLastCommittedMillis(this.db);
        assert.gt(duration,
                  0,
                  `Expected lastCommittedMillis to be > 0 after index build, but got ${duration}`);
    });

    it("on restart", function() {
        let duration = getLastCommittedMillis(this.db);
        assert.eq(
            duration, 0, `Expected lastCommittedMillis to be 0 on startup, but got ${duration}`);

        // Hang index build before completion to require restart.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.coll.getFullName(),
            {a: 1},
            {} /* options */,
            [ErrorCodes.InterruptedDueToReplStateChange],
        );

        // Ensure index build is in-progress before stopping and starting, else it is not restarted.
        fp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true})["a_1"]
                .buildUUID,
        );

        // Kill process to require index build to start over. The `forRestart` flag is required to
        // preserve state between stop and start.
        this.rst.stop(
            0, 9 /* signal */, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

        // Wait for the parallel shell to exit.
        awaitCreateIndex({checkExitStatus: false});

        this.rst.start(0, {} /* options */, true /* restart */);

        // Connect to new primary.
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        // Ensure index build is completed before reading metrics.
        ResumableIndexBuildTest.assertCompleted(this.primary, this.coll, [buildUUID], ["a_1"]);

        duration = getLastCommittedMillis(this.db);
        assert.gt(duration,
                  0,
                  `Expected lastCommittedMillis to be > 0 after index build, but got ${duration}`);
    });
});
