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

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {IndexBuildTest, ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const collName = "t";
const dbName = "test";

describe("index build failures", function () {
    before(() => {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(dbName);
    });

    beforeEach(() => {
        this.coll = this.db.getCollection(collName);
        assert.commandWorked(this.coll.insertMany(Array.from({length: 10}, () => ({a: "foo"}))));
    });

    it("failedDueToDuplicateKeyError", () => {
        // Check that creating an index over a collection with duplicate keys only increments
        // failedDueToDuplicateKeyError.
        assert.eq(0, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(0, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);

        assert.commandFailedWithCode(this.coll.createIndex({a: 1}, {unique: 1}), ErrorCodes.DuplicateKey);

        IndexBuildTest.assertIndexes(this.coll, 1, ["_id_"]);
        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(0, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);

        this.coll.drop();

        // Check that inserting a duplicate key into a unique index only increments
        // insertFailedDueToDuplicateKeyError.
        assert.commandWorked(this.coll.createIndex({a: 1}, {unique: 1}));
        assert.commandWorked(this.coll.insert({a: "foo"}));
        assert.commandFailedWithCode(this.coll.insert({a: "foo"}), ErrorCodes.DuplicateKey);

        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(1, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);
    });

    it("failedDueToManualCancellation", () => {
        assert.eq(0, this.db.serverStatus().indexBuilds.failedDueToManualCancellation);

        const fp = configureFailPoint(this.db, "hangAfterInitializingIndexBuild");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(this.conn, this.coll.getFullName(), {a: 1});

        fp.wait();
        assert.commandWorked(this.coll.dropIndex({a: 1}));
        awaitCreateIndex({checkExitStatus: false});

        // Wait for index build to be fully aborted before reading metrics.
        IndexBuildTest.waitForIndexBuildToStop(this.db, this.coll.getName(), "a_1");

        IndexBuildTest.assertIndexes(this.coll, 1, ["_id_"]);
        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToManualCancellation);
    });

    afterEach(() => {
        this.coll.drop();
    });

    after(() => {
        MongoRunner.stopMongod(this.conn);
    });
});

describe("lastCommittedMillis", function () {
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

    it("during steady state", () => {
        assert.eq(0, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);

        // Hang index build before completion to extend duration.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(this.primary, this.coll.getFullName(), {a: 1});

        // Initiate the failpoint and then sleep for 1000ms to ensure duration >= 1000ms.
        fp.wait();
        sleep(1000);
        fp.off();

        // Wait for the parallel shell to exit.
        awaitCreateIndex();

        assert.lt(1000, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);
    });

    it("on resume", () => {
        assert.eq(0, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);

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
            IndexBuildTest.assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true})["a_1"].buildUUID,
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

        assert.lt(0, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);
    });

    it("on restart", function () {
        assert.eq(0, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);

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
            IndexBuildTest.assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true})["a_1"].buildUUID,
        );

        // Kill process to require index build to start over. The `forRestart` flag is required to
        // preserve state between stop and start.
        this.rst.stop(0, 9 /* signal */, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

        // Wait for the parallel shell to exit.
        awaitCreateIndex({checkExitStatus: false});

        this.rst.start(0, {} /* options */, true /* restart */);

        // Connect to new primary.
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        // Ensure index build is completed before reading metrics.
        ResumableIndexBuildTest.assertCompleted(this.primary, this.coll, [buildUUID], ["a_1"]);

        assert.lt(0, this.db.serverStatus().indexBuilds.phases.lastCommittedMillis);
    });

    afterEach(() => {
        this.rst.stopSet();
    });
});
