/**
 * Tests that writes accepted on a new primary before a resumed primary-driven index build has set
 * itself up are still reflected in the committed index.
 *
 * No resume phase re-derives keys for records it has already processed, so the only way a write
 * taken in that window reaches the index is through the pending interceptor created for the build
 * before the node began accepting writes. Each phase is covered because each leaves a different
 * amount of the collection already processed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = jsTestName();
const collName = "coll";

// Written in the window. The values are outside the range the seeded documents use.
const insertedDoc = {_id: "inserted", a: -1, b: -1, c: -1};
const updatedId = 7;
const updatedValue = -2;
const deletedId = 8;

// Runs the window scenario for one resume phase and asserts every node's index agrees with the
// collection.
function runScenario(rst, phase, position) {
    let deletedDoc;
    const result = PrimaryDrivenResumableIndexBuildTest.runWithWritesBeforeResumeSetup(rst, {
        dbName,
        collName,
        phase,
        position,
        writesInWindow: (db, coll) => {
            deletedDoc = db.getCollection(coll).findOne({_id: deletedId});
            assert(deletedDoc, `expected a seeded document with _id ${deletedId}`);
            assert.commandWorked(
                db.runCommand({
                    insert: coll,
                    documents: [insertedDoc],
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                db.runCommand({
                    update: coll,
                    updates: [{q: {_id: updatedId}, u: {$set: {a: updatedValue}}}],
                    writeConcern: {w: "majority"},
                }),
            );
            assert.commandWorked(
                db.runCommand({
                    delete: coll,
                    deletes: [{q: {_id: deletedId}, limit: 1}],
                    writeConcern: {w: "majority"},
                }),
            );
        },
    });
    if (!result) {
        return;
    }

    // Every node's index must agree with its collection. Read the nodes off the replica set
    // rather than the helper: failover modes that restart the old primary replace its connection.
    //
    // Stale keys are probed with covered queries: fetching through a key that points at a deleted
    // record throws DataCorruptionDetected.
    for (const node of rst.nodes) {
        const coll = node.getDB(dbName).getCollection(collName);
        const label = node.port === result.newPrimary.port ? "new primary" : node.host;

        assert.eq(
            1,
            coll.find({a: insertedDoc.a}).hint({a: 1}).itcount(),
            `${label}: the inserted document must be reachable through the index`,
        );
        assert.eq(
            1,
            coll.find({a: updatedValue}).hint({a: 1}).itcount(),
            `${label}: the updated value must be reachable through the index`,
        );
        assert.eq(
            0,
            coll.find({a: deletedDoc.a}, {_id: 0, a: 1}).hint({a: 1}).itcount(),
            `${label}: the deleted document's key must not survive`,
        );

        const validateRes = assert.commandWorked(coll.validate({full: true}));
        assert.eq(true, validateRes.valid, `${label}: validate must pass`, {validateRes});
        assert.eq(0, validateRes.missingIndexEntries.length, `${label}: no missing entries`, {
            validateRes,
        });
        assert.eq(0, validateRes.extraIndexEntries.length, `${label}: no extra entries`, {
            validateRes,
        });
    }
}

describe("resumed primary-driven index build", function () {
    before(() => {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({testName: jsTestName()});
    });

    after(() => {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("includes writes taken before a drain-phase resume set itself up", () => {
        runScenario(this.rst, PdibPhase.DRAIN, PdibPosition.BEGINNING);
    });

    it("includes writes taken before a bulk-load-phase resume set itself up", () => {
        runScenario(this.rst, PdibPhase.LOAD, PdibPosition.BEGINNING);
    });

    // END so the records the window writes touch are already scanned; a resumed scan restarts from
    // the last spilled record and never revisits them.
    it("includes writes taken before a collection-scan-phase resume set itself up", () => {
        runScenario(this.rst, PdibPhase.SCAN, PdibPosition.END);
    });
});
