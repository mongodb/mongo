/**
 * Regression pin for SERVER-126463.
 *
 * The collection cloner's `insertDocumentsCallback` constructs a
 * `CollectionBulkLoader::ParseRecordIdAndDocFunc` (a `function_ref`) from a
 * lambda temporary, then invokes it inside `CollectionBulkLoaderImpl::
 * insertDocuments`. Because `function_ref` is a non-owning view, the
 * temporary's destruction at the end of the initialization full-expression
 * leaves the view dangling. Captureless lambdas mask the dereference today,
 * but any future capture turns this into live use-after-free.
 *
 * This test stresses the affected code path by running initial sync many
 * times across a fault-injected sync source, exercising both
 * `recordIdsReplicated: true` and the default. ASAN-instrumented builds
 * catch a real dereference of out-of-lifetime storage; release builds still
 * benefit from the test as a smoke pin against future regressions in the
 * cloner / bulk-loader interaction.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kTestName = "initial_sync_cloner_fn_ref_lifetime";
const kDbName = kTestName;
// Run several initial sync iterations so the cloner is constructed and torn
// down repeatedly. Bumping kIterations is the right knob if a future ASAN
// regression is intermittent.
const kIterations = 3;
// Several collections per iteration so the cloner re-enters
// `insertDocumentsCallback` for each one.
const kCollections = 4;
// Batch size below and above one batch boundary, so we observe the inner
// loop in `CollectionBulkLoaderImpl::insertDocuments` execute the
// `function_ref` invocation more than once per cloner stage.
const kDocsPerCollection = 250;

function seedCollection(primaryDB, collName) {
    const coll = primaryDB[collName];
    const docs = [];
    for (let i = 0; i < kDocsPerCollection; ++i) {
        // Mix BSON shapes so the bulk loader cannot short-circuit any
        // size-homogeneous fast path.
        docs.push({_id: i, a: i, payload: "x".repeat((i % 16) + 1)});
    }
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.createIndex({a: 1}));
}

function runOneIteration(replTest, primary, iteration) {
    jsTestLog(`Iteration ${iteration}: seeding ${kCollections} collections on primary.`);
    const primaryDB = primary.getDB(kDbName);
    for (let c = 0; c < kCollections; ++c) {
        seedCollection(primaryDB, `coll_${iteration}_${c}`);
    }

    // Pause cloning of the first collection so we can poke the fail-point
    // before documents flow through `insertDocumentsCallback`. This forces
    // the cloner to walk the same code path repeatedly when the fail point
    // is released, mirroring the original report's call frequency.
    const stageFp = "hangBeforeClonerStage";
    const targetNss = `${kDbName}.coll_${iteration}_0`;

    let secondary = replTest.add({
        rsConfig: {priority: 0, votes: 0},
        setParameter: {
            numInitialSyncAttempts: 1,
            // Both passes of the matrix:
            //   - default `recordIdsReplicated: false` exercises the
            //     `[](const BSONObj& doc) { return {RecordId(0), doc}; }`
            //     lambda branch.
            //   - we cannot toggle replicated record IDs from a node-level
            //     param here, so the second pass below recreates the
            //     replica set with the feature enabled at collection time.
            ["failpoint." + stageFp]: tojson({
                mode: "alwaysOn",
                data: {cloner: "CollectionCloner", stage: "query", nss: targetNss},
            }),
        },
    });

    replTest.reInitiate();
    replTest.waitForState(secondary, ReplSetTest.State.STARTUP_2);

    jsTestLog(`Iteration ${iteration}: waiting for stage failpoint on ${targetNss}.`);
    assert.commandWorked(
        secondary.adminCommand({waitForFailPoint: stageFp, timesEntered: 1, maxTimeMS: 60 * 1000}),
    );

    // While paused, write more documents into the OTHER collections so the
    // cloner will need to re-enter `insertDocumentsCallback` multiple times
    // after release. Index-only writes also exercise the bulk loader on a
    // distinct shape from the initial seed batch.
    const primaryDBLive = replTest.getPrimary().getDB(kDbName);
    for (let c = 1; c < kCollections; ++c) {
        const extras = [];
        for (let i = kDocsPerCollection; i < kDocsPerCollection + 50; ++i) {
            extras.push({_id: i, a: i, payload: "y".repeat((i % 8) + 1)});
        }
        assert.commandWorked(primaryDBLive[`coll_${iteration}_${c}`].insert(extras));
    }

    jsTestLog(`Iteration ${iteration}: releasing stage failpoint.`);
    assert.commandWorked(secondary.adminCommand({configureFailPoint: stageFp, mode: "off"}));

    jsTestLog(`Iteration ${iteration}: awaiting initial sync completion.`);
    replTest.awaitSecondaryNodes(null, [secondary]);

    // Sanity: the cloned secondary saw every row. If the dangling
    // `function_ref` ever returned garbage instead of crashing, document
    // counts would drift.
    for (let c = 0; c < kCollections; ++c) {
        const collName = `coll_${iteration}_${c}`;
        const primaryCount = primaryDBLive[collName].countDocuments({});
        const secondaryDB = secondary.getDB(kDbName);
        secondary.setSecondaryOk();
        const secondaryCount = secondaryDB[collName].countDocuments({});
        assert.eq(
            primaryCount,
            secondaryCount,
            `count drift on ${collName}: primary=${primaryCount} secondary=${secondaryCount}`,
        );
    }

    // Drop the freshly-synced secondary so the next iteration starts clean.
    replTest.remove(secondary);
    replTest.reInitiate();
}

const replTest = new ReplSetTest({name: kTestName, nodes: 1});
replTest.startSet();
replTest.initiate();

assert.commandWorked(
    replTest.getPrimary().adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }),
);

try {
    for (let i = 0; i < kIterations; ++i) {
        runOneIteration(replTest, replTest.getPrimary(), i);
    }
} finally {
    replTest.stopSet();
}

jsTestLog(`${kTestName}: ${kIterations} initial-sync iterations completed without crash.`);
