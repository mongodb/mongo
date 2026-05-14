/**
 * Inject a non-text index that carries a `weights` field (a shape that the server accepted
 * prior to SERVER-54712) and verify that collMod can scrub the now-invalid option without
 * data loss or replication divergence.
 *
 * Exercises the harness at jstests/sharding/stale_catalog_injection_harness.js.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {
    runStaleCatalogInjectionScenario,
    StaleCatalogFailPoints,
} from "jstests/sharding/stale_catalog_injection_harness.js";

const kDbName = "test";
const kCollName = jsTestName();

runStaleCatalogInjectionScenario({
    topology: "replset",
    nodes: 2,
    failPoints: [StaleCatalogFailPoints.skipIndexFieldNames],
    // The legacy options survive a clean shutdown only if the catalog row already reflects
    // them, so we explicitly restart to confirm the catalog reloads cleanly.
    restartAfterSetup: true,
    setup: (harness, primary) => {
        const coll = primary.getDB(kDbName).getCollection(kCollName);
        assert.commandWorked(coll.insert({_id: 0, a: 1}));
        // Non-text index with a `weights` field - illegal on master, legal pre-SERVER-54712.
        assert.commandWorked(
            coll.createIndex({a: 1}, {weights: {a: 5}, sparse: true}),
        );
        // And one extra unknown field to make sure scrub handles >1 stale option in a row.
        assert.commandWorked(coll.createIndex({b: 1}, {legacy_unused_option: true}));
    },
    verify: (harness, primary) => {
        const db = primary.getDB(kDbName);
        const secondaryDb = harness.rst.getSecondary().getDB(kDbName);

        for (const conn of [db, secondaryDb]) {
            const validateRes = assert.commandWorked(conn.runCommand({validate: kCollName}));
            assert(validateRes.valid, validateRes);
            // One warning per index with a stale option.
            assert.eq(validateRes.errors.length, 0, validateRes);
            assert.gte(validateRes.warnings.length, 2, validateRes);
        }

        // collMod scrubs the invalid options on the primary and replicates the scrub.
        assert.commandWorked(db.runCommand({collMod: kCollName}));

        checkLog.containsJson(primary, 23878, {fieldName: "weights"});
        checkLog.containsJson(primary, 23878, {fieldName: "legacy_unused_option"});
        checkLog.containsJson(harness.rst.getSecondary(), 23878, {fieldName: "weights"});

        for (const conn of [db, secondaryDb]) {
            const validateRes = assert.commandWorked(conn.runCommand({validate: kCollName}));
            assert(validateRes.valid, validateRes);
            assert.eq(validateRes.warnings.length, 0, validateRes);
        }
    },
});
