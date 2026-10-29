/**
 * Tests that a primary-driven index build resumes on step-up when the record its collection scan
 * would resume from has been deleted.
 *
 * The scan resumes after the lowest record every index has spilled. If that record was deleted (by
 * capped roll-off or a user delete), the scan must start at the next surviving record.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    PdibFailoverMode,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = jsTestName();
const kDocs = 40;

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({testName: jsTestName()});
if (!PrimaryDrivenResumableIndexBuildTest._preflight(rst, dbName, jsTestName())) {
    PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
    quit();
}

// A 1 MB sorter budget makes every index spill before the pause. The harness reapplies these if a
// failover restarts a node.
PrimaryDrivenResumableIndexBuildTest._setIndexBuildSettings(rst, {
    maxIndexBuildMemoryUsageMegabytes: 1,
    logComponentVerbosity: {index: {verbosity: 1}},
});

function insertDocs(coll, makeDoc) {
    for (let i = 0; i < kDocs; i++) {
        assert.commandWorked(coll.insert(makeDoc(i)));
    }
}

// Returns 'node''s logs with 'id' for 'buildUUID'.
function buildLogs(node, id, buildUUID) {
    return (checkLog.getFilteredLogMessages(node, id, {}) || []).filter(
        (entry) => entry.attr.buildUUID.uuid.$uuid === buildUUID,
    );
}

// Returns each index's last spilled record id, in index order.
function lastSpilledRecordIds(node, buildUUID, numIndexes) {
    const kWroteIndexStateInfoLogId = 12558701;
    const lastSpilled = new Array(numIndexes).fill(null);
    for (const entry of buildLogs(node, kWroteIndexStateInfoLogId, buildUUID)) {
        const recordId = entry.attr.details.lastSpilledRecordId;
        if (recordId !== undefined) {
            lastSpilled[entry.attr.index] = Number(recordId);
        }
    }
    assert(
        lastSpilled.every((recordId) => recordId !== null),
        "an index did not spill",
        {
            lastSpilled,
        },
    );
    return lastSpilled;
}

function deleteRecord(coll, recordId) {
    const doc = coll
        .find({}, {_id: 1})
        .showRecordId()
        .toArray()
        .find((d) => Number(d.$recordId) === recordId);
    assert(doc, "no document at record id", {recordId});
    assert.commandWorked(coll.deleteOne({_id: doc._id}));
}

// Builds 'keyPatterns', deletes the resume record via 'deleteResumeRecords', fails over, and checks
// the resumed build completes successfully from the next surviving record.
function buildAcrossFailover(collName, keyPatterns, deleteResumeRecords) {
    const primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    const indexNames = keyPatterns.map((keyPattern) => Object.keys(keyPattern)[0] + "_1");

    const kPauseAtId = 30;
    const scanFp = configureFailPoint(
        primary,
        "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
        {
            fieldsToMatch: {_id: kPauseAtId},
        },
    );
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        keyPatterns,
        {},
        [ErrorCodes.InterruptedDueToReplStateChange],
    );
    scanFp.wait();
    const indexMap = IndexBuildTest.assertIndexesSoon(
        coll,
        1 + indexNames.length,
        ["_id_"],
        indexNames,
        {
            includeBuildUUIDs: true,
        },
    );
    const buildUUID = extractUUIDFromObject(indexMap[indexNames[0]].buildUUID);

    const lastSpilled = lastSpilledRecordIds(primary, buildUUID, indexNames.length);
    deleteResumeRecords(coll, lastSpilled);
    rst.awaitReplication();

    const newPrimary = PrimaryDrivenResumableIndexBuildTest._failover(
        rst,
        primary,
        rst.getSecondary(),
        PdibFailoverMode.NO_RESTART,
    );
    if (!PrimaryDrivenResumableIndexBuildTest.failoverStopsOldPrimary()) {
        scanFp.off();
    }
    awaitIndexBuild({checkExitSuccess: false});

    const oplog = newPrimary.getDB("local").getCollection("oplog.rs");
    assert.soon(
        () =>
            oplog.findOne({
                op: "c",
                "o.indexBuildUUID": UUID(buildUUID),
                "o.commitIndexBuild": {$exists: true},
            }),
        "no commitIndexBuild for build " + buildUUID,
    );

    // validate also catches keys left behind for deleted records.
    const newColl = newPrimary.getDB(dbName).getCollection(collName);
    const res = assert.commandWorked(newColl.validate());
    assert(res.valid, "validate failed", {res});

    const liveRecordIds = newColl
        .find({}, {_id: 1})
        .showRecordId()
        .toArray()
        .map((doc) => Number(doc.$recordId));
    const countAfter = (recordId) => liveRecordIds.filter((rid) => rid > recordId).length;
    const kScanDoneLogId = 13224800;
    const scanDone = buildLogs(newPrimary, kScanDoneLogId, buildUUID);
    assert.eq(1, scanDone.length, "expected one resumed scan", {scanDone});
    assert.eq(
        countAfter(Math.min(...lastSpilled)),
        scanDone[0].attr.numDocsScanned,
        "resumed scan did not start at the next surviving record",
        {lastSpilled},
    );
    assert.eq(
        lastSpilled.reduce((total, recordId) => total + countAfter(recordId), 0),
        scanDone[0].attr.numDocsIndexed,
        "an index did not skip the records it already spilled",
        {lastSpilled},
    );
    assert(newColl.drop());
}

describe("resuming a primary-driven index build whose resume record was deleted", function () {
    const largePadding = "x".repeat(64 * 1024);

    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
    });

    it("completes after capped roll-off deletes the resume record", function () {
        const db = rst.getPrimary().getDB(dbName);
        assert.commandWorked(
            db.createCollection("capped", {capped: true, size: 64 * 1024 * 1024, max: kDocs}),
        );
        insertDocs(db.capped, (i) => ({_id: i, a: `${i}-${largePadding}`}));

        // Rolling off every original document deletes the resume record.
        buildAcrossFailover("capped", [{a: 1}], (coll) => {
            for (let i = kDocs; i < 2 * kDocs; i++) {
                assert.commandWorked(coll.insert({_id: i, a: `${i}-${largePadding}`}));
            }
        });
    });

    it("completes after a user delete removes the resume record", function () {
        const db = rst.getPrimary().getDB(dbName);
        insertDocs(db.regular, (i) => ({_id: i, a: `${i}-${largePadding}`}));

        // Delete exactly the resume record, leaving the records after it to resume onto.
        buildAcrossFailover("regular", [{a: 1}], (coll, [lastSpilled]) =>
            deleteRecord(coll, lastSpilled),
        );
    });

    it("completes when two indexes resume from different spill points", function () {
        // 'b' keys are 4x larger than 'a' keys, so b_1 spills more often and its last spill is
        // later. The scan resumes from a_1's earlier spill point, so b_1 must skip the records it
        // already spilled between the two points.
        const smallPadding = "x".repeat(16 * 1024);
        const db = rst.getPrimary().getDB(dbName);
        insertDocs(db.two_indexes, (i) => ({
            _id: i,
            a: `${i}-${smallPadding}`,
            b: `${i}-${largePadding}`,
        }));

        buildAcrossFailover(
            "two_indexes",
            [{a: 1}, {b: 1}],
            (coll, [aLastSpilled, bLastSpilled]) => {
                assert.gt(
                    bLastSpilled,
                    aLastSpilled + 1,
                    "spill points must leave records between them",
                );
                // Delete only the resume record, keeping the records between the two spill points.
                deleteRecord(coll, aLastSpilled);
            },
        );
    });
});
