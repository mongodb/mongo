/**
 * Oversized-group fallback during a primary-driven index build (PDIB), run both plain and in a
 * retryable-writes session. When one document's group (its collection write plus side-table writes)
 * cannot fit one applyOps entry, the batched insert fails with TransactionTooLarge and the insert
 * executor re-drives each document on its own. A single-document insert during PDIB then replicates
 * as one atomic chain spanning multiple oplog entries -- so the oversized document is never torn and
 * the insert still succeeds. The retryable run additionally retains its retryable-write metadata on
 * the chain.
 *
 * Forced via a shrunken maxSizeOfBatchedOperationsInSingleOplogEntryBytes plus one large document.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    assertIndexMatchesOnSecondary,
    getGroupedApplyOps,
} from "jstests/noPassthrough/libs/index_builds/pdib_group_atomicity.js";

const kSizeCapBytes = 1 * 1024 * 1024; // 1MB cap.
const dbName = "test";
const collName = jsTestName();
const indexName = "x_1";
const big = "a".repeat(1536 * 1024); // 1.5MB: this doc's group alone exceeds the cap.
const docs = [
    {_id: 0, x: 100},
    {_id: 1, x: 101},
    {_id: 2, x: big}, // oversized
    {_id: 3, x: 103},
    {_id: 4, x: 104},
];

function runScenario({retryable}) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            setParameter: {maxSizeOfBatchedOperationsInSingleOplogEntryBytes: kSizeCapBytes},
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
    assert.commandWorked(coll.insert({_id: -1, x: -1}));

    // Pause and start an index build on `x` so the insert fires side-table ops counting against the
    // per-applyOps size budget.
    IndexBuildTest.pauseIndexBuilds(primary);
    const awaitIndex = IndexBuildTest.startIndexBuild(
        primary,
        coll.getFullName(),
        {x: 1},
        {name: indexName},
    );
    IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

    let session;
    let insertColl = coll;
    if (retryable) {
        session = primary.startSession({retryWrites: true});
        insertColl = session.getDatabase(dbName).getCollection(collName);
    }
    // The oversized group must not surface TransactionTooLarge to the client: the per-document
    // fallback handles it.
    assert.commandWorked(insertColl.insertMany(docs, {ordered: true}));

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();

    assert.eq(
        coll.countDocuments({}),
        docs.length + 1,
        "expected all docs (incl. initial) present",
    );

    // The oversized document replicated (proving the fallback replicated it, not a dropped/torn
    // write).
    rst.awaitReplication();
    const secondaryColl = rst.getSecondary().getDB(dbName).getCollection(collName);
    assert.eq(
        secondaryColl.countDocuments({_id: 2}),
        1,
        "expected the oversized document to replicate to the secondary",
    );

    // Correlate the fallback to the oversized doc specifically: find the entry holding _id:2's
    // collection insert and assert its group replicated as a multi-entry atomic chain, not a single
    // self-contained entry. This is stronger than checking that any atomic chain exists, since the
    // small docs re-drive as single-entry groups.
    const applyOps = getGroupedApplyOps(primary, {
        nss: coll.getFullName(),
        lsid: retryable ? session.getSessionId().id : undefined,
    });
    const bigDocEntry = applyOps.find((e) => e.o.applyOps.some((op) => op.o && op.o._id === 2));
    assert(bigDocEntry, "expected the oversized doc's collection op in the applyOps stream", {
        applyOps,
    });
    assert(
        bigDocEntry.o.partialTxn === true || bigDocEntry.o.count > 1,
        "expected the oversized doc to replicate as a multi-entry atomic chain",
        {bigDocEntry},
    );

    if (retryable) {
        // The retryable fallback chain also retains retryable-write metadata; the non-retryable
        // chain would not.
        assert(
            bigDocEntry.lsid && bigDocEntry.txnNumber !== undefined,
            "expected the retryable fallback chain to retain retryable-write metadata",
            {bigDocEntry},
        );
        session.endSession();
    }

    // The build drained correctly on the secondary: an indexed query returns the same result there.
    assertIndexMatchesOnSecondary(rst, dbName, collName, "x_1", {x: 100});

    rst.stopSet();
}

runScenario({retryable: false});
runScenario({retryable: true});
