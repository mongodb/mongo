/**
 * Inject a TTL index whose expireAfterSeconds is persisted as NaN (a shape that the server
 * accepted prior to SERVER-68477) and verify:
 *   - server startup logs the expected warning,
 *   - validate() reports the collection as valid with a warning,
 *   - collMod with the no-op spec coerces the NaN value back to a valid integer.
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
    failPoints: [
        StaleCatalogFailPoints.skipIndexFieldNames,
        StaleCatalogFailPoints.skipTtlExpireValidation,
    ],
    setup: (harness, primary) => {
        const coll = primary.getDB(kDbName).getCollection(kCollName);
        assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));
        // With the failpoints lifted, this createIndex call would have its NaN replaced by
        // int32::max; under the failpoints the catalog row keeps the NaN exactly as a pre-
        // SERVER-68477 mongod would have persisted it.
        assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
    },
    verify: (harness, primary) => {
        const secondary = harness.rst.getSecondary();

        // Startup warning fired on the restarted secondary (the harness restarts both nodes,
        // so the previously-secondary node is the one carrying the freshly-replayed catalog).
        checkLog.containsJson(secondary, 6852200, {
            ns: `${kDbName}.${kCollName}`,
            spec: (spec) => isNaN(spec.expireAfterSeconds),
        });

        const db = primary.getDB(kDbName);
        const validateRes = assert.commandWorked(db.runCommand({validate: kCollName}));
        assert(validateRes.valid, validateRes);
        assert.gte(validateRes.warnings.length, 1, validateRes);

        // collMod with no spec changes still walks the index list and is expected to fix up
        // the NaN value in place.
        assert.commandWorked(db.runCommand({collMod: kCollName}));

        // Subsequent writes against the collection must still succeed - the whole point is
        // that catalog repair does not break the data plane.
        assert.commandWorked(
            db.getCollection(kCollName).insert({_id: 1, t: ISODate()}),
        );
    },
});
