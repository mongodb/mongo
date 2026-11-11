/**
 * Tests that a resumed unique index build commits when a duplicate key it recorded before the
 * resume is resolved before commit.
 *
 * The build records a duplicate in the first drain. The duplicate document is then deleted, and the
 * node restarts while the build is paused, so the build resumes and drains the delete before it
 * commits. At commit the constraint check finds the recorded duplicate resolved and the build
 * should succeed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    kRamLogMaxLines,
    IndexBuildTest,
    ResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const kLogDuplicateRecorded = 20676; // "Index build: recording duplicate key conflict on unique index"
const kLogResuming = 12500800; // "Index build: resuming index build from phase"

const dbName = "test";
const collName = jsTestName();
const indexName = "a_1";
const numDocs = 100;
// The seeded document {_id: 5, a: 5} already holds this key.
const dupDoc = {_id: "dup", a: 5};

function matchBuildUUID(buildUUID) {
    return (uuid) => uuid && uuid["uuid"]["$uuid"] === buildUUID;
}

describe("resumable unique index build with a transient duplicate", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                // Log when the build records a duplicate key.
                setParameter: {
                    logComponentVerbosity: tojson({index: 1}),
                    ramLogMaxLines: kRamLogMaxLines,
                },
            },
        });
        this.rst.startSet();
        this.rst.initiate();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("commits after a restart when the recorded duplicate was resolved", function () {
        const rst = this.rst;
        let primary = rst.getPrimary();
        let coll = primary.getDB(dbName).getCollection(collName);

        assert.commandWorked(
            coll.insert(Array.from({length: numDocs}, (_, i) => ({_id: i, a: i}))),
        );

        // Hold the build after the collection scan so the duplicate arrives as a side write.
        const fpAfterBulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst,
            function (collName, indexSpec, indexName) {
                // The restart interrupts the createIndexes command.
                try {
                    assert.commandFailedWithCode(
                        db
                            .getCollection(collName)
                            .createIndex(indexSpec, {name: indexName, unique: true}),
                        [
                            ErrorCodes.InterruptedDueToReplStateChange,
                            ErrorCodes.InterruptedAtShutdown,
                        ],
                    );
                } catch (e) {
                    if (!isNetworkError(e)) {
                        throw e;
                    }
                }
            },
            coll,
            {a: 1},
            indexName,
            () => {}, // No side writes before the collection scan.
        );
        const buildUUID = extractUUIDFromObject(
            IndexBuildTest.assertIndexesIdHelper(coll, 1, [], [indexName], {
                includeBuildUUIDs: true,
            })[indexName].buildUUID,
        );
        fpAfterBulk.wait();

        // The first drain applies the duplicate's key and records it in the duplicate key tracker.
        // Resumable two-phase builds drain at the majority-committed snapshot, so the first drain
        // sees this insert only once it is majority committed.
        assert.commandWorked(coll.insert(dupDoc));
        rst.awaitLastOpCommitted();

        const fpAfterFirstDrain = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
        fpAfterBulk.off();
        fpAfterFirstDrain.wait();
        checkLog.containsJson(primary, kLogDuplicateRecorded, {index: indexName});

        // Resolve the duplicate. Its delete stays in the side writes table until the build resumes.
        assert.commandWorked(coll.remove({_id: dupDoc._id}));

        // A clean shutdown interrupts the paused build. The fail point does not survive it.
        jsTest.log.info("Restarting the node with the index build paused after the first drain", {
            buildUUID,
        });
        rst.stop(primary, undefined, {}, {forRestart: true});
        awaitCreateIndex();
        rst.start(primary, {}, /*restart=*/ true);
        primary = rst.getPrimary();

        // Validate that we resumed and all duplicate key errors get resolved.
        checkLog.containsJson(primary, kLogResuming, {buildUUID: matchBuildUUID(buildUUID)});
        coll = primary.getDB(dbName).getCollection(collName);
        ResumableIndexBuildTest.assertCompleted(primary, coll, [buildUUID], [indexName]);

        ResumableIndexBuildTest.assertIndexesReadyAndValidate(
            rst,
            dbName,
            collName,
            [indexName],
            (coll) => {
                assert.eq(numDocs, coll.countDocuments({}));
                // The committed index enforces uniqueness.
                assert.commandFailedWithCode(coll.insert(dupDoc), ErrorCodes.DuplicateKey);
            },
        );
    });
});
