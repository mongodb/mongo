/**
 * SERVER-126385: Verify a unique-index build retries cleanly through injected write
 * conflicts and does not surface a spurious DuplicateKey error when the conflicting
 * document is rolled back and re-applied by the retry loop.
 *
 * Regression-shaped: a bug in the retry path could double-count an in-flight insert and
 * fail the unique-constraint check on a key that is, in the end, unique.
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

// All-unique seed corpus. No two documents share `u`.
const numDocs = 150;
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i, u: i});
    }
    assert.commandWorked(bulk.execute());
}

// Yield often so the build re-enters the retry-eligible code path frequently.
assert.commandWorked(primary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 8}));

assert.commandWorked(
    primary.adminCommand({
        configureFailPoint: "WTWriteConflictException",
        mode: {activationProbability: 0.08},
    }),
);

let buildErr = null;
try {
    assert.commandWorked(coll.createIndex({u: 1}, {name: "u_1", unique: true}));
} catch (e) {
    buildErr = e;
} finally {
    assert.commandWorked(primary.adminCommand({configureFailPoint: "WTWriteConflictException", mode: "off"}));
}

assert.eq(null, buildErr, () => "unique index build failed under WCE retry: " + tojson(buildErr));
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "u_1"]);

// All seeded values should be present under the unique index exactly once.
assert.eq(numDocs, coll.find().hint({u: 1}).itcount());

// And the unique constraint must actually be enforced post-build.
assert.commandFailedWithCode(coll.insert({_id: numDocs, u: 0}), ErrorCodes.DuplicateKey);

rst.stopSet();
