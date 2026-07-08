/**
 * Self-test for jstests/hooks/run_check_multikey_consistency.js.
 *
 * Builds healthy replica-set and sharded-cluster fixtures with regular multikey, regular
 * non-multikey, and wildcard multikey indexes, then invokes the hook in a separate shell the same
 * way resmoke does: assign global db to the target connection and import the hook module. The hook
 * must pass on both the direct replica-set path and the sharded-cluster Thread-worker path.
 *
 * Also verifies that the hook reports a regular-index divergence. The test creates a two-node
 * replica set, keeps a regular {a: 1} index non-multikey by inserting only scalar values, then
 * stops the secondary and rewrites _mdb_catalog.wt to flip the multikey flag and multikeyPaths for
 * the test index. The hook must fail reporting the divergence.
 *
 * @tags: [
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_persistence,
 *   requires_replication,
 *   requires_sharding,
 *   requires_snapshot_read,
 *   requires_wiredtiger,
 *   uses_transactions,
 * ]
 */

import {rewriteCatalogTable} from "jstests/disk/libs/wt_file_helper.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kHookConfig = Object.freeze({
    maxCollectionsPerIteration: null,
    maxWildcardPathsPerIndex: 10,
});

const kWriteConcern = Object.freeze({w: "majority"});
const kFeatureFlagParameter = Object.freeze({featureFlagReplicateMultikeynessInTransactions: true});

function runMultikeyConsistencyHook(conn, topologyName) {
    const awaitShell = startParallelShell(
        funWithArgs(
            async (host, hookConfig) => {
                const conn = new Mongo(host);
                globalThis.db = conn.getDB("admin");
                globalThis.TestData = globalThis.TestData || {};
                TestData.multikeyHook = hookConfig;
                await import("jstests/hooks/run_check_multikey_consistency.js");
            },
            conn.host,
            kHookConfig,
        ),
        undefined,
        true,
    );

    const exitCode = awaitShell({checkExitSuccess: false});
    assert.eq(0, exitCode, `${topologyName} multikey consistency hook exited non-zero`);
}

function createIndexedCollections(testDB, collNamePrefix) {
    const regularMultikey = testDB[`${collNamePrefix}_regular_multikey`];
    assert.commandWorked(
        regularMultikey.insert(
            [
                {_id: -1, multikeyField: [1, 2]},
                {_id: 1, multikeyField: [3, 4]},
            ],
            {writeConcern: kWriteConcern},
        ),
    );
    assert.commandWorked(
        regularMultikey.createIndex({multikeyField: 1}, {name: "multikeyField_1"}),
    );

    const regularScalar = testDB[`${collNamePrefix}_regular_scalar`];
    assert.commandWorked(
        regularScalar.insert(
            [
                {_id: -1, scalarField: 1},
                {_id: 1, scalarField: 2},
            ],
            {writeConcern: kWriteConcern},
        ),
    );
    assert.commandWorked(regularScalar.createIndex({scalarField: 1}, {name: "scalarField_1"}));

    const wildcardMultikey = testDB[`${collNamePrefix}_wildcard_multikey`];
    assert.commandWorked(
        wildcardMultikey.insert(
            [
                {_id: -1, nested: {path: [1, 2], scalar: 1}},
                {_id: 1, nested: {path: [1, 3], scalar: 2}},
            ],
            {writeConcern: kWriteConcern},
        ),
    );
    assert.commandWorked(wildcardMultikey.createIndex({"$**": 1}, {name: "wildcard_all_paths"}));
}

function shardIndexedCollections(st, testDB, collNamePrefix) {
    for (const suffix of ["regular_multikey", "regular_scalar", "wildcard_multikey"]) {
        st.shardColl(
            testDB[`${collNamePrefix}_${suffix}`],
            {_id: 1},
            {_id: 0},
            {_id: 0},
            testDB.getName(),
        );
    }
}

describe("multikey consistency hook self-test", () => {
    it("passes against a healthy replica set through the direct replica-set path", () => {
        const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: kFeatureFlagParameter}});
        rst.startSet();
        rst.initiate();

        try {
            const primary = rst.getPrimary();
            const testDB = primary.getDB(`${jsTestName()}_replset`);
            assert.commandWorked(testDB.dropDatabase());

            createIndexedCollections(testDB, "rs");
            rst.awaitReplication();

            runMultikeyConsistencyHook(primary, "replica set");
        } finally {
            rst.stopSet();
        }
    });

    it("handles collections with numeric names", () => {
        // Collection names that look like integers (e.g. "123") must be preserved as strings
        // when the hook enumerates and accesses them. Bracket notation db["123"] causes
        // SpiderMonkey to convert the key to an integer, which then reaches the server as
        // a BSON numeric type in the listIndexes command, producing
        // "Collection name has invalid type double".
        const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: kFeatureFlagParameter}});
        rst.startSet();
        rst.initiate();

        try {
            const primary = rst.getPrimary();
            const testDB = primary.getDB(`${jsTestName()}_numeric`);
            assert.commandWorked(testDB.dropDatabase());

            // Create collections with purely numeric names.
            for (const numCollName of ["0", "123", "0.3"]) {
                const coll = testDB.getCollection(numCollName);
                assert.commandWorked(
                    coll.insert({_id: 0, a: [1, 2]}, {writeConcern: kWriteConcern}),
                );
                assert.commandWorked(coll.createIndex({a: 1}, {name: "a_1"}));
            }
            rst.awaitReplication();

            // The hook must not throw "Collection name has invalid type double".
            runMultikeyConsistencyHook(primary, "replica set with numeric-named collections");
        } finally {
            rst.stopSet();
        }
    });

    it("passes against a healthy sharded cluster through the Thread-worker path", () => {
        const st = new ShardingTest({
            mongos: 1,
            mongosOptions: {setParameter: kFeatureFlagParameter},
            shards: 2,
            rs: {nodes: 2},
            rsOptions: {setParameter: kFeatureFlagParameter},
            config: 2,
            configOptions: {setParameter: kFeatureFlagParameter},
        });

        try {
            const testDB = st.s.getDB(`${jsTestName()}_sharded`);
            assert.commandWorked(testDB.dropDatabase());

            shardIndexedCollections(st, testDB, "sharded");
            createIndexedCollections(testDB, "sharded");

            runMultikeyConsistencyHook(st.s, "sharded cluster");
        } finally {
            st.stop();
        }
    });

    it("detects an offline-injected regular-index multikey catalog divergence", () => {
        const dbName = `${jsTestName()}_divergence`;
        const collName = "coll";
        const indexName = "a_1";
        const ns = `${dbName}.${collName}`;

        // The feature flag must be enabled so the hook actually performs its cross-member
        // comparison; with it disabled the hook intentionally skips (see the legacy-path case).
        const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: kFeatureFlagParameter}});
        rst.startSet();
        rst.initiate();

        try {
            const primary = rst.getPrimary();
            let secondary = rst.getSecondary();
            const coll = primary.getDB(dbName).getCollection(collName);

            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, a: 0},
                    {_id: 1, a: 1},
                ]),
            );
            assert.commandWorked(coll.createIndex({a: 1}, {name: indexName}));
            rst.awaitReplication();

            jsTestLog("Stopping secondary and injecting catalog-only multikey divergence");
            rst.stop(secondary, undefined, undefined, {forRestart: true});

            let modified = false;
            rewriteCatalogTable(secondary, (entry) => {
                if (!entry.md || entry.md.ns !== ns) {
                    return;
                }

                const indexMetadata = entry.md.indexes.find(
                    (idx) => idx.spec && idx.spec.name === indexName,
                );
                assert.neq(undefined, indexMetadata, "expected test index in catalog entry", {
                    entry,
                });
                assert.eq(false, indexMetadata.multikey, "test index should start non-multikey", {
                    indexMetadata,
                });

                indexMetadata.multikey = true;
                indexMetadata.multikeyPaths = {a: BinData(0, "AQ==")};
                modified = true;
            });
            assert(modified, "expected to modify secondary catalog entry", {ns, indexName});

            jsTestLog("Restarting secondary after catalog surgery");
            secondary = rst.start(secondary, {}, true /* restart */);
            rst.awaitSecondaryNodes();
            rst.awaitReplication();

            const hookTestData = {
                multikeyHook: kHookConfig,
            };
            clearRawMongoProgramOutput();
            const hookExitCode = runMongoProgram(
                "mongo",
                "--host",
                rst.getURL(),
                "--eval",
                `TestData = ${tojson(hookTestData)};`,
                "jstests/hooks/run_check_multikey_consistency.js",
            );
            const hookOutput = rawMongoProgramOutput("multikey divergence");

            assert.neq(0, hookExitCode, "hook unexpectedly succeeded", {hookExitCode, hookOutput});
            assert(
                hookOutput.includes("multikey divergence (regular index)"),
                "hook failure did not report regular-index multikey divergence",
                {hookExitCode, hookOutput},
            );
        } finally {
            rst.stopSet(undefined, false, {skipCheckDBHashes: true, skipValidation: true});
        }
    });

    it("tolerates a transient invalid view left in system.views", () => {
        const dbName = `${jsTestName()}_invalid_view`;

        const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: kFeatureFlagParameter}});
        rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const testDB = primary.getDB(dbName);
        const viewName = "dbNameWithEmbedded\0Character.collectionName";

        try {
            assert.commandWorked(testDB.dropDatabase());

            // Ensure this DB would otherwise have collections to check; the invalid view below
            // should still break listCollections.
            createIndexedCollections(testDB, "invalidview");

            // Create an invalid view.
            assert.commandWorked(testDB.createCollection("system.views"));
            assert.commandWorked(
                primary.adminCommand({
                    applyOps: [
                        {
                            op: "i",
                            ns: testDB.getName() + ".system.views",
                            o: {_id: viewName, viewOn: "someColl", pipeline: []},
                        },
                    ],
                }),
            );
            rst.awaitReplication();

            // listCollections must actually be broken by the invalid view; otherwise the test would
            // pass even without the hook's tolerance.
            assert.throwsWithCode(
                () => testDB.getCollectionInfos({type: "collection"}),
                ErrorCodes.InvalidViewDefinition,
            );

            runMultikeyConsistencyHook(primary, "invalid view");
        } finally {
            // Remove the bogus view so fixture teardown (dbhash/validation) is clean.
            assert.commandWorked(
                primary.adminCommand({
                    applyOps: [
                        {op: "d", ns: testDB.getName() + ".system.views", o: {_id: viewName}},
                    ],
                }),
            );
            rst.awaitReplication();
            rst.stopSet();
        }
    });
});
