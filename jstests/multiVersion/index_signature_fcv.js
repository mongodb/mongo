/**
 * Verifies that the following FCV constraints are observed when building indexes:
 *
 *   - Multiple indexes which differ only by partialFilterExpression can be built in FCV 4.7+.
 *   - Multiple indexes which differ only by unique or sparse can be built in FCV 4.9+.
 *   - Multiple indexes which differ only by wildcardProjection can be built in FCV 5.0+.
 *   - The planner can continue to use these indexes after downgrading to FCV 4.4.
 *   - These indexes can be dropped in FCV 4.4.
 *   - Indexes which differ only by partialFilterExpression cannot be created in FCV 4.4.
 *   - Indexes which differ only by either unique, sparse, or wildcardProjection cannot be created
 *     in FCV 4.4.
 *   - We do not fassert if the set is downgraded to binary 4.4 with "duplicate" indexes present.
 *
 * TODO SERVER-47766: this test is specific to the 4.4 - 4.7+ upgrade process, and can be removed
 * when 5.0 becomes last-lts.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");               // For isIxscan and hasRejectedPlans.
load("jstests/multiVersion/libs/multi_rs.js");      // For upgradeSet.
load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest

// Runs a createIndex command with the given key pattern and options. Then verifies that no index
// was built, since an index with the same signature already existed.
function assertIndexAlreadyExists(keyPattern, indexOptions) {
    const numIndexesBefore = coll.getIndexes().length;
    const cmdRes = assert.commandWorked(coll.createIndex(keyPattern, indexOptions));

    // In a sharded cluster, the results from all shards are returned in cmdRes.raw.
    assert(cmdRes.numIndexesBefore != null || Object.values(cmdRes.raw), tojson(cmdRes));
    const numIndexesAfter =
        (cmdRes.numIndexesAfter != null ? cmdRes.numIndexesAfter
                                        : Object.values(cmdRes.raw)[0].numIndexesAfter);

    assert.eq(numIndexesAfter, numIndexesBefore, cmdRes);
}

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {binVersion: "latest"},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB(jsTestName());
let coll = testDB.test;
coll.insert({a: 100});

// Verifies that the given query is indexed, and that 'numAlternativePlans' were generated.
function assertIndexedQuery(query, numAlternativePlans) {
    const explainOut = coll.explain().find(query).finish();
    assert(isIxscan(testDB, explainOut), explainOut);
    assert.eq(getRejectedPlans(explainOut).length, numAlternativePlans, explainOut);
}

// Creates a base index without any index options.
assert.commandWorked(coll.createIndex({a: 1}, {name: "base_index"}));

// Verifies that multiple indexes differing from base_index only by 'partialFilterExpression' option
// can be created in FCV 4.7+.
testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index1", partialFilterExpression: {a: {$gte: 0}}}));
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}));
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index3", partialFilterExpression: {a: {$gte: 100}}}));

// Verifies that the planner considers all relevant partial indexes when answering a query in
// FCV 4.7+.
assertIndexedQuery({a: 1}, 1);
assertIndexedQuery({a: 11}, 2);
assertIndexedQuery({a: 101}, 3);

// Verifies that an index differing from base_index only by 'unique' option can be created in
// FCV 4.9+.
assert.commandWorked(coll.createIndex({a: 1}, {name: "unique_index", unique: true}));

// Verifies that the planner considers all relevant indexes when answering a query in FCV 4.9+.
assertIndexedQuery({a: 1}, 2);
assertIndexedQuery({a: 11}, 3);
assertIndexedQuery({a: 101}, 4);

// Verifies that an index differing from base_index only by 'sparse' option can be created in
// FCV 4.9+.
assert.commandWorked(coll.createIndex({a: 1}, {name: "sparse_index", sparse: true}));

// Verifies that the planner considers all relevant indexes when answering a query in FCV 4.9+.
assertIndexedQuery({a: 1}, 3);
assertIndexedQuery({a: 11}, 4);
assertIndexedQuery({a: 101}, 5);

// Creates a base wildcard index without any index options.
assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wc_all"}));

// Verifies that an index differing from wc_all only by 'wildcardProjection' option can be created
// in FCV 5.0+.
assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wc_a", wildcardProjection: {a: 1}}));

// Verifies that the planner considers all relevant indexes when answering a query in FCV 5.0+.
assertIndexedQuery({a: 1}, 5);
assertIndexedQuery({a: 11}, 6);
assertIndexedQuery({a: 101}, 7);

// Verifies that an index build restarted during startup recovery in FCV 4.7+ does not revert to
// FCV 4.4 behavior.
jsTestLog("Starting index build on primary and pausing before completion");
IndexBuildTest.pauseIndexBuilds(primary);
IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {name: "index4", partialFilterExpression: {a: {$lt: 0}}});

jsTestLog("Waiting for secondary to start the index build");
let secondary = rst.getSecondary();
let secondaryDB = secondary.getDB(jsTestName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
rst.restart(secondary.nodeId);

jsTestLog("Waiting for all nodes to finish building the index");
IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.waitForIndexBuildToStop(testDB, coll.getName(), "index4");
rst.awaitReplication();

// Resets connection in case leadership has changed.
primary = rst.getPrimary();
testDB = primary.getDB(jsTestName());
coll = testDB.test;

// base_index & unique_index & sparse_index & wc_all & wc_a can be used.
assertIndexedQuery({a: -1}, 5);

jsTestLog("Downgrade to the last LTS");

// Downgrades to the last LTS FCV
testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});

// Verifies that attempting to build an index with the same name and identical value for
// 'partialFilterExpression' option as an existing index results in a no-op in FCV 4.4, and the
// command reports successful execution.
var cmdRes = assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index1", partialFilterExpression: {a: {$gte: 0}}}));
assert.eq(cmdRes.numIndexesBefore, cmdRes.numIndexesAfter);

// Verifies that attempting to build an index with the same name and identical value for 'unique'
// option as an existing index results in a no-op in FCV 4.4, and the command reports successful
// execution.
cmdRes = assert.commandWorked(coll.createIndex({a: 1}, {name: "unique_index", unique: true}));
assert.eq(cmdRes.numIndexesBefore, cmdRes.numIndexesAfter);

// Verifies that attempting to build an index with the same name and identical value for 'sparse'
// option as an existing index results in a no-op in FCV 4.4, and the command reports successful
// execution.
cmdRes = assert.commandWorked(coll.createIndex({a: 1}, {name: "sparse_index", sparse: true}));
assert.eq(cmdRes.numIndexesBefore, cmdRes.numIndexesAfter);

// Verifies that attempting to build an index with the same name and identical value for
// 'wildcardProjection' option as an existing index results in a no-op in FCV 4.4, and the command
// reports successful execution.
cmdRes =
    assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wc_a", wildcardProjection: {a: 1}}));
assert.eq(cmdRes.numIndexesBefore, cmdRes.numIndexesAfter);

// Verifies that these indexes are retained and can be used by the planner when we downgrade to
// FCV 4.4.
assertIndexedQuery({a: 1}, 5);
assertIndexedQuery({a: 11}, 6);
assertIndexedQuery({a: 101}, 7);

// Verifies that indexes distinguished only by 'partialFilterExpression' can be dropped by name in
// FCV 4.4.
assert.commandWorked(coll.dropIndex("index2"));
assertIndexedQuery({a: 1}, 5);
assertIndexedQuery({a: 11}, 5);
assertIndexedQuery({a: 101}, 6);

// Verifies that indexes distinguished only by 'unique' option can be dropped by name in FCV 4.4.
assert.commandWorked(coll.dropIndex("unique_index"));
assertIndexedQuery({a: 1}, 4);
assertIndexedQuery({a: 11}, 4);
assertIndexedQuery({a: 101}, 5);

// Verifies that indexes distinguished only by 'sparse' option can be dropped by name in FCV 4.4.
assert.commandWorked(coll.dropIndex("sparse_index"));
assertIndexedQuery({a: 1}, 3);
assertIndexedQuery({a: 11}, 3);
assertIndexedQuery({a: 101}, 4);

// Verifies that indexes distinguished only by 'wildcardProjection' option can be dropped by name in
// FCV 4.4.
assert.commandWorked(coll.dropIndex("wc_a"));
assertIndexedQuery({a: 1}, 2);
assertIndexedQuery({a: 11}, 2);
assertIndexedQuery({a: 101}, 3);

// Verifies that an index distinguished only by 'partialFilterExpression' option cannot be created
// in FCV 4.4.
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}),
    ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'unique' option cannot be created in FCV 4.4.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {name: "unique_index", unique: true}),
                             ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'sparse' option cannot be created in FCV 4.4.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {name: "sparse_index", sparse: true}),
                             ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'wildcardProjection' option cannot be created in
// FCV 4.4.
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a", wildcardProjection: {a: 1}}),
    ErrorCodes.IndexOptionsConflict);

// We need to recreate the unique & sparse & wildcardProjection indexes that we just dropped before
// downgrading to the LTS binary. To do so, we need to temporarily upgrade the FCV to 'latest'
// again.
testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});

assert.commandWorked(coll.createIndex({a: 1}, {name: "unique_index", unique: true}));
assert.commandWorked(coll.createIndex({a: 1}, {name: "sparse_index", sparse: true}));
assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wc_a", wildcardProjection: {a: 1}}));

// Need to downgrade to the LTS FCV before downgrade to the LTS binary.
testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});

// Verifies that downgrading to binary 4.4 with overlapping partialFilterExpression, unique, sparse,
// and wildcardProjection indexes present does not fassert.
rst.upgradeSet({binVersion: "last-lts"});
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.test;

// Verifies that the indexes still exist and can be used to answer queries on the binary 4.4
// replset.
assertIndexedQuery({a: 1}, 5);
assertIndexedQuery({a: 11}, 5);
assertIndexedQuery({a: 101}, 6);

// Verifies that indexes which are distinguished only by 4.7+ signature fields can be dropped by
// name on binary 4.4.
assert.commandWorked(coll.dropIndex("unique_index"));
assert.commandWorked(coll.dropIndex("sparse_index"));
assert.commandWorked(coll.dropIndex("wc_a"));

// Verifies that an index which differs only by 'partialFilterExpression' option cannot be created
// on binary 4.4.
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}),
    ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'unique' option cannot be created in binary 4.4.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {name: "unique_index", unique: true}),
                             ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'sparse' option cannot be created in binary 4.4.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {name: "sparse_index", sparse: true}),
                             ErrorCodes.IndexOptionsConflict);

// Verifies that an index distinguished only by 'wildcardProjection' option cannot be created in
// binary 4.4.
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a", wildcardProjection: {a: 1}}),
    ErrorCodes.IndexOptionsConflict);

// Prepares the next test case that verifies that non-normalized wildcard path projection in a
// previous version will be normalized and compared to a new wildcard index path projection.
assert.commandWorked(coll.dropIndex("wc_all"));

// Path projections are not normalized before FCV 4.7.
assert.commandWorked(coll.createIndex(
    {"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {_id: 0, "a.b": 1, "a.c": 1}}));

// Upgrades to the latest binary. The FCV stays as the last FCV even after binary upgrade.
rst.upgradeSet({binVersion: "latest"});
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.test;

// Before upgrading to the latest FCV, verifies that any attempt to create an wildcard index
// with a wildcard projection fails with the IndexOptionsConflict error since we already
// have the 'wc_a_sub_b_c' wildcard index and wildcardProjection is not part of the index signature
// in the last FCV.
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {name: "wc_all"}),
                             ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1},
                     {name: "wc_noid_a_sub_b_c", wildcardProjection: {_id: 0, a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1},
                     {name: "wc_id_a_sub_b_c", wildcardProjection: {_id: 1, a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {a: {c: 1, b: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c_1", wildcardProjection: {a: {c: 1, b: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {"a.c": 1, "a.b": 1}}),
    ErrorCodes.IndexOptionsConflict);
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1},
                     {name: "wc_a_sub_b_c_1", wildcardProjection: {"a.c": 1, "a.b": 1}}),
    ErrorCodes.IndexOptionsConflict);

// Upgrades to the lastest FCV
testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});

// Verifies that indexes with path projections which is identical after normalization can not be
// created.
assertIndexAlreadyExists({"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {a: {b: 1, c: 1}}});
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c_1", wildcardProjection: {a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists({"$**": 1},
                         {name: "wc_a_sub_b_c", wildcardProjection: {_id: 0, a: {b: 1, c: 1}}});
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1},
                     {name: "wc_noid_a_sub_b_c", wildcardProjection: {_id: 0, a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists({"$**": 1}, {name: "wc_a_sub_b_c", wildcardProjection: {a: {c: 1, b: 1}}});
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1}, {name: "wc_a_sub_b_c_1", wildcardProjection: {a: {c: 1, b: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists({"$**": 1},
                         {name: "wc_a_sub_b_c", wildcardProjection: {"a.c": 1, "a.b": 1}});
assert.commandFailedWithCode(
    coll.createIndex({"$**": 1},
                     {name: "wc_a_sub_b_c_1", wildcardProjection: {"a.c": 1, "a.b": 1}}),
    ErrorCodes.IndexOptionsConflict);

// Only the 'wc_a_sub_b_c' index can be used. So, there's no alternative plan. Verifies this before
// creating an wildcard index with explicit inclusion of _id path projection.
assertIndexedQuery({"a.b": 10}, 0);

// Verifies that an index with a path projection which is different only in _id path can be created.
assert.commandWorked(coll.createIndex(
    {"$**": 1}, {name: "wc_id_a_sub_b_c", wildcardProjection: {_id: 1, a: {b: 1, c: 1}}}));

// The 'wc_a_sub_b_c' and 'wc_id_a_sub_b_c' indexes can be used. So, there's one alternative plan.
assertIndexedQuery({"a.b": 10}, 1);

rst.stopSet();
})();
