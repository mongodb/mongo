/**
 * Group atomicity for a vectored insert into a clustered collection during a primary-driven index
 * build (PDIB). A clustered collection has no replicated record id, so verify a concurrent insert's
 * per-document writes still group correctly: when the insert spans multiple applyOps entries (forced
 * via a small maxNumberOfBatchedOperationsInSingleOplogEntry), each document's collection write must
 * stay with its index writes in one entry. Entries apply independently on secondaries, so a torn
 * group would leave an entry's collection-op and index-write counts out of proportion.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    assertGroupAtomicity,
    assertIndexMatchesOnSecondary,
    getGroupedApplyOps,
} from "jstests/noPassthrough/libs/index_builds/pdib_group_atomicity.js";

// max=3 ops/entry: with 5 records and one index write each, the insert spans several entries.
const kMaxOpsInEntry = 3;
const dbName = "test";
const collName = jsTestName();
const docs = [0, 1, 2, 3, 4].map((i) => ({_id: i, x: 100 + i}));

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {maxNumberOfBatchedOperationsInSingleOplogEntry: kMaxOpsInEntry},
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(dbName);
// TODO(SERVER-109578): Remove these checks when the feature flags are removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}
if (!FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites")) {
    jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(
    db.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}),
);
const coll = db.getCollection(collName);
const nss = coll.getFullName();
assert.commandWorked(coll.insert({_id: -1, x: -1}));

// Pause and build a secondary index so the vectored insert below writes one index key per document.
const indexName = "x_1";
IndexBuildTest.pauseIndexBuilds(primary);
const awaitIndex = IndexBuildTest.startIndexBuild(primary, nss, {x: 1}, {name: indexName});
IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

assert.commandWorked(coll.insertMany(docs, {ordered: true}));

IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

const applyOps = getGroupedApplyOps(primary, {nss});
assert.gte(applyOps.length, 2, "expected the vectored insert to span multiple applyOps entries", {
    applyOps,
});

const {primaryOps, containerOps} = assertGroupAtomicity(applyOps, nss, {sideWritesPerRecord: 1});
assert.eq(primaryOps, docs.length, "expected one collection op per doc", {applyOps});
assert.eq(containerOps, docs.length, "expected one index write per doc", {applyOps});

assertIndexMatchesOnSecondary(rst, dbName, collName, indexName, {x: {$gte: 0}});

rst.stopSet();
