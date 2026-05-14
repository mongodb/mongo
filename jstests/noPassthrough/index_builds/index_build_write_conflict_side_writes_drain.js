/**
 * SERVER-126385: Exercise the storage-engine-agnostic write conflict failpoint while the
 * side-writes table is being drained, and verify the drain phase retries cleanly and the
 * final index agrees with the collection.
 *
 * Pattern: pause the build after the initial collection scan, drive concurrent CRUD which
 * lands in the side-writes table, then probabilistically inject write conflicts and resume
 * the build so the drain path is what exercises the retry loop.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(jsTestName());
const coll = primaryDB.getCollection("coll");

const initialDocs = 100;
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < initialDocs; i++) {
        bulk.insert({_id: i, a: i});
    }
    assert.commandWorked(bulk.execute());
}

// Pause new index builds before the side-writes drain begins.
IndexBuildTest.pauseIndexBuilds(primary);

const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, coll.getName(), "a_1");
assert.gte(opId, 0, "expected an index build opId after the collection-scan phase");

// Drive side-writes: inserts, updates, deletes that the drain phase will have to apply.
const sideWriteOps = 60;
for (let i = 0; i < sideWriteOps; i++) {
    assert.commandWorked(coll.insert({_id: initialDocs + i, a: initialDocs + i}));
    if (i % 3 === 0) {
        assert.commandWorked(coll.update({_id: i}, {$set: {a: i + 10_000}}));
    }
    if (i % 5 === 0) {
        assert.commandWorked(coll.remove({_id: i + 1}));
    }
}

// Now activate the WCE failpoint, then unpause so the *drain* hits the retry loop.
assert.commandWorked(
    primary.adminCommand({
        configureFailPoint: "WTWriteConflictException",
        mode: {activationProbability: 0.1},
    }),
);

try {
    IndexBuildTest.resumeIndexBuilds(primary);
    awaitBuild();
} finally {
    assert.commandWorked(primary.adminCommand({configureFailPoint: "WTWriteConflictException", mode: "off"}));
}

IndexBuildTest.waitForIndexBuildToStop(primaryDB, coll.getName(), "a_1");
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

// Collection-vs-index sanity: every doc must be indexed under the {a:1} entry.
const indexedCount = coll.find({a: {$exists: true}}).hint({a: 1}).itcount();
assert.eq(coll.count(), indexedCount, "drain phase under WCE retries dropped or duplicated keys");

rst.stopSet();
