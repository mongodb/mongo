/**
 * Group atomicity for a vectored insert during a primary-driven index build (PDIB), run plain,
 * retryable, and with multiple indexes building at once. When the insert spans multiple applyOps
 * entries (forced via a small maxNumberOfBatchedOperationsInSingleOplogEntry), each document's
 * collection write must stay with all of its side-table writes in one entry; entries apply
 * independently on secondaries, so a torn group would leave an entry's collection-op and container-op
 * counts out of proportion. With N indexes a record has N side writes, exercising groups of N+1 ops.
 * The retryable run also checks the entries retain their retryable-write metadata.
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

// max=3 ops/entry: with 5 records and up to 2 side writes each, the batch must span several entries.
const kMaxOpsInEntry = 3;
const dbName = "test";
const collName = jsTestName();
const docs = [
    {_id: 0, x: 100, y: 200},
    {_id: 1, x: 101, y: 201},
    {_id: 2, x: 102, y: 202},
    {_id: 3, x: 103, y: 203},
    {_id: 4, x: 104, y: 204},
];

// A single-field ascending keyPattern's default index name, e.g. {x: 1} -> "x_1".
function indexNameFor(keyPattern) {
    const field = Object.keys(keyPattern)[0];
    return field + "_" + keyPattern[field];
}

function runScenario({retryable, indexKeys = [{x: 1}]}) {
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

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.insert({_id: -1, x: -1, y: -1}));

    // Pause and build the index(es) so the vectored insert below fires one side write per index.
    const indexNames = indexKeys.map(indexNameFor);
    IndexBuildTest.pauseIndexBuilds(primary);
    const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexKeys);
    IndexBuildTest.waitForIndexBuildToStart(db, collName, indexNames[0]);

    let session;
    let insertColl = coll;
    if (retryable) {
        session = primary.startSession({retryWrites: true});
        insertColl = session.getDatabase(dbName).getCollection(collName);
    }
    assert.commandWorked(insertColl.insertMany(docs, {ordered: true}));

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();

    const nss = coll.getFullName();
    const applyOps = getGroupedApplyOps(primary, {
        nss,
        lsid: retryable ? session.getSessionId().id : undefined,
    });
    assert.gte(
        applyOps.length,
        2,
        "expected the vectored insert to span multiple applyOps entries",
        {
            applyOps,
        },
    );

    const perEntry = retryable
        ? (entry, inners) => {
              assert(entry.lsid, "expected a retryable applyOps entry to carry an lsid", {entry});
              assert.neq(entry.txnNumber, undefined, "expected a txnNumber", {entry});
              assert(
                  entry.stmtId !== undefined || inners.some((op) => op.stmtId !== undefined),
                  "expected stmtId(s)",
                  {entry},
              );
          }
        : undefined;
    const {primaryOps, containerOps} = assertGroupAtomicity(applyOps, nss, {
        sideWritesPerRecord: indexKeys.length,
        perEntry,
    });
    assert.eq(primaryOps, docs.length, "expected one collection op per doc", {applyOps});
    assert.eq(
        containerOps,
        indexKeys.length * docs.length,
        "expected one side write per doc per index",
        {applyOps},
    );

    // The build drained correctly on the secondary: an indexed query returns the same docs there as
    // on the primary.
    for (let i = 0; i < indexKeys.length; i++) {
        const field = Object.keys(indexKeys[i])[0];
        assertIndexMatchesOnSecondary(rst, dbName, collName, indexNames[i], {[field]: {$gte: 0}});
    }

    if (session) {
        session.endSession();
    }

    rst.stopSet();
}

runScenario({retryable: false});
runScenario({retryable: true});
runScenario({retryable: false, indexKeys: [{x: 1}, {y: 1}]});
