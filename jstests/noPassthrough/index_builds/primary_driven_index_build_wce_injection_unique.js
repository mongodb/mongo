/**
 * Drives a primary-driven UNIQUE index build with WCEs injected throughout.
 * Unique builds exercise the duplicate-key detection path twice (during the
 * load and again at commit); both must remain correct under spurious WCE
 * retries — a real duplicate must still surface as DuplicateKey, and a
 * conflict-free build must still succeed.
 *
 * See SERVER-126326 — PDIB WC-injection test coverage.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0, votes: 0}},
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");

if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(
    primaryDB.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}),
);

// ----------------------------------------------------------------------------
// Case 1: conflict-free unique build under WCE injection must succeed.
// ----------------------------------------------------------------------------
const uniqueCollName = "pdibWceUnique";
const uniqueColl = primaryDB.getCollection(uniqueCollName);
assert.commandWorked(primaryDB.runCommand({create: uniqueCollName}));

const numUnique = 3000;
let bulk = uniqueColl.initializeUnorderedBulkOp();
for (let i = 0; i < numUnique; i++) {
    bulk.insert({_id: i, u: i});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

let wcFp = configureFailPoint(
    primary,
    "WTWriteConflictException",
    {},
    {activationProbability: 0.02},
);
try {
    jsTestLog("Building unique index over distinct keys under WCE injection");
    assert.commandWorked(
        primaryDB.runCommand({
            createIndexes: uniqueCollName,
            indexes: [{key: {u: 1}, name: "u_1", unique: true}],
        }),
    );
} finally {
    wcFp.off();
}
IndexBuildTest.assertIndexes(uniqueColl, 2, ["_id_", "u_1"]);
assert.eq(numUnique, uniqueColl.find().hint({u: 1}).itcount());

// ----------------------------------------------------------------------------
// Case 2: a true duplicate must still surface DuplicateKey even with WCEs
// firing in the background. WCE retries must not mask the real conflict.
// ----------------------------------------------------------------------------
const dupCollName = "pdibWceUniqueDup";
const dupColl = primaryDB.getCollection(dupCollName);
assert.commandWorked(primaryDB.runCommand({create: dupCollName}));

bulk = dupColl.initializeUnorderedBulkOp();
for (let i = 0; i < 1000; i++) {
    bulk.insert({_id: i, d: i});
}
// Inject one real duplicate on key `d`.
bulk.insert({_id: 1000, d: 42});
assert.commandWorked(bulk.execute());
rst.awaitReplication();

wcFp = configureFailPoint(
    primary,
    "WTWriteConflictException",
    {},
    {activationProbability: 0.02},
);
try {
    jsTestLog("Building unique index over duplicate keys under WCE injection");
    const res = primaryDB.runCommand({
        createIndexes: dupCollName,
        indexes: [{key: {d: 1}, name: "d_1", unique: true}],
    });
    assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
} finally {
    wcFp.off();
}

// The failed unique-index attempt must not leave an `d_1` index behind.
IndexBuildTest.assertIndexes(dupColl, 1, ["_id_"]);

rst.awaitReplication();
rst.stopSet();
