/**
 * Group atomicity for timeseries bucket *updates* during a primary-driven index build (PDIB). A
 * timeseries insert batch that appends measurements to existing buckets produces bucket update ops,
 * which fire side-table (container) writes via the index interceptor's update() path. The batch
 * replicates as a single batched write, and each bucket update's collection write must stay with its
 * side writes in one applyOps entry.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
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

const kMaxOpsInEntry = 3;
const dbName = "test";
const collName = jsTestName();
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

const timeField = "t";
const metaField = "m";
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
);
const coll = db.getCollection(collName);
// A timeseries bucket update logs at the collection namespace (with isTimeseries set), not the
// system.buckets namespace.
const nss = coll.getFullName();

// Seed one bucket per meta value so the second batch below appends to (updates) existing buckets.
const metas = [0, 1, 2, 3, 4];
const baseTime = ISODate("2026-01-01T00:00:00Z");
assert.commandWorked(
    coll.insertMany(metas.map((m) => ({[timeField]: baseTime, [metaField]: m, v: m}))),
);

// Build an index on a measurement field so bucket updates change index keys and fire side writes.
const indexName = "v_1";
IndexBuildTest.pauseIndexBuilds(primary);
const awaitIndex = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    {v: 1},
    {name: indexName},
);
IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

// Append one measurement to each existing bucket: >1 bucket update op in one batched write, each
// producing container side writes. Capture the oplog timestamp first so the check below skips the
// seed insertMany (also a batched write, but it predates the index build so it has no side writes).
const oplog = primary.getDB("local").getCollection("oplog.rs");
const tsBeforeAppend = oplog.find().sort({$natural: -1}).limit(1).next().ts;
assert.commandWorked(
    coll.insertMany(
        metas.map((m) => ({
            [timeField]: new Date(baseTime.getTime() + 1000),
            [metaField]: m,
            v: m + 10,
        })),
    ),
);

IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

const applyOps = getGroupedApplyOps(primary, {nss, afterTs: tsBeforeAppend});
assert.gte(
    applyOps.length,
    2,
    "expected the bucket-update batch to span multiple applyOps entries",
    {
        applyOps,
    },
);
// Each bucket update rewrites the indexed max-key: one side write to remove the old key and one to
// add the new key, so two side writes per bucket update.
const {primaryOps, containerOps} = assertGroupAtomicity(applyOps, nss, {sideWritesPerRecord: 2});
assert.eq(primaryOps, metas.length, "expected one bucket update per seeded bucket", {applyOps});
assert.eq(containerOps, 2 * metas.length, "expected two side writes per bucket update", {applyOps});

// The build drained correctly on the secondary: the measurement index returns the same result there.
assertIndexMatchesOnSecondary(rst, dbName, collName, indexName, {v: {$gte: 0}});

rst.stopSet();
