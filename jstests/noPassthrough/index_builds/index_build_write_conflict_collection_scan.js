/**
 * SERVER-126385: Exercise the storage-engine-agnostic write conflict failpoint during the
 * collection-scan phase of a hybrid index build, and verify the build retries through the
 * injected conflicts and commits a queryable index.
 *
 * The failpoint name (WTWriteConflictException) is the runtime knob exported by the storage
 * layer. SERVER-126328 generalised the C++ test-harness wiring; for jstests we still drive it
 * via adminCommand. Keep `requires_wiredtiger` until the runtime failpoint is renamed to
 * something storage-agnostic.
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

const numDocs = 200;
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i, a: i, payload: "x".repeat(64)});
    }
    assert.commandWorked(bulk.execute());
}

// Force frequent yields so the collection-scan phase repeatedly re-acquires locks: each
// re-acquisition path is a candidate site for the injected write conflict to fire.
assert.commandWorked(primary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 4}));

// Probabilistic injection across the entire build, exercising the retry loop in
// IndexBuildsCoordinator without forcing a deterministic abort path.
assert.commandWorked(
    primary.adminCommand({
        configureFailPoint: "WTWriteConflictException",
        mode: {activationProbability: 0.05},
    }),
);

try {
    assert.commandWorked(coll.createIndex({a: 1}, {name: "a_1"}));
} finally {
    assert.commandWorked(primary.adminCommand({configureFailPoint: "WTWriteConflictException", mode: "off"}));
}

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);
assert.eq(numDocs, coll.find({a: {$gte: 0}}).hint({a: 1}).itcount());

rst.stopSet();
