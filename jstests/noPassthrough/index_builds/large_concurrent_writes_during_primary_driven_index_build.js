/**
 * Tests that when a batched insert that exceeds the 16MB oplog entry limit is run concurrently with
 * a primary-driven index build is split into multiple chained applyOps entries in the oplog.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = jsTestName();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
const indexName = "x_1";
const indexSpec = {x: 1};

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primary.getDB(dbName), "ContainerWrites")) {
    jsTestLog("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

assert.commandWorked(coll.insert({x: 0}));

// Prevent the index build from completing.
IndexBuildTest.pauseIndexBuilds(primary);

// Start the index build and wait for it to start.
const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexSpec, {name: indexName});
IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

// Construct a ~16 MB string and perform a large write while the index build is in progress.
const hugeString = "a".repeat(1024 * 1024 * 16 - 43);
assert.commandWorked(coll.insertOne({x: 0, big: hugeString}));

// Resume and allow the index build to complete.
IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

// Inspect the oplog.rs for applyOps generated from the concurrent write during index build.
const oplog = primary.getDB("local").getCollection("oplog.rs");
const nss = coll.getFullName();
const containerNss = "admin.$container";
const applyOps = oplog
    .find({
        op: "c",
        "o.applyOps": {$exists: true},
        "o.applyOps.ns": {$in: [nss, containerNss]},
    })
    .sort({ts: 1})
    .toArray();

function redactBigInApplyOps(entry) {
    if (entry.o && Array.isArray(entry.o.applyOps)) {
        entry.o = Object.assign({}, entry.o);
        entry.o.applyOps = entry.o.applyOps.map((inner) => {
            inner = Object.assign({}, inner);
            if (inner.o && inner.o.hasOwnProperty("big")) {
                inner.o = Object.assign({}, inner.o);
                inner.o.big = "<redacted big>";
            }
            return inner;
        });
    }

    return entry;
}

function redactBigInApplyOpsArray(entries) {
    return entries.map(redactBigInApplyOps);
}

// The default oplog entry size is 16MB so expect 3 applyOps entries. Since the document inserted is
// large, the container insert to the side write table will be split into a second chained applyOp.
// There is also a third applyOps from draining the side write table.
assert.eq(
    applyOps.length,
    3,
    "expected large batch to be split into 3 applyOps for " +
        nss +
        ", got: " +
        tojson(redactBigInApplyOpsArray(applyOps)),
);

rst.stopSet();
