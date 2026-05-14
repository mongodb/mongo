/**
 * Inject a TTL index whose expireAfterSeconds is persisted as a float (a shape that the
 * server accepted prior to SERVER-77828) and verify that the server starts cleanly, treats
 * the value as a valid TTL expiry on the data path, and that collMod normalises it.
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
        // 3.14 instead of 3 - exactly the kind of float-typed expireAfterSeconds the
        // pre-SERVER-77828 server would persist.
        assert.commandWorked(
            coll.createIndex({t: 1}, {expireAfterSeconds: 3.14}),
        );
    },
    verify: (harness, primary) => {
        const db = primary.getDB(kDbName);
        const secondary = harness.rst.getSecondary();

        // listIndexes should reflect the persisted float - the server is tolerant on read.
        const indexes = db.getCollection(kCollName).getIndexes();
        const ttl = indexes.find((idx) => idx.name !== "_id_");
        assert(ttl !== undefined, indexes);
        assert.eq(typeof ttl.expireAfterSeconds, "number", ttl);

        // No startup error - the server keeps running. The collection accepts writes.
        assert.commandWorked(
            db.getCollection(kCollName).insert({_id: 1, t: ISODate()}),
        );

        // Validate succeeds on both nodes - float expireAfterSeconds is a soft repair target.
        for (const conn of [db, secondary.getDB(kDbName)]) {
            const res = assert.commandWorked(conn.runCommand({validate: kCollName}));
            assert(res.valid, res);
        }

        // collMod with an explicit integer normalises the value.
        assert.commandWorked(db.runCommand({
            collMod: kCollName,
            index: {name: ttl.name, expireAfterSeconds: NumberLong(4)},
        }));
        const normalised = db.getCollection(kCollName).getIndexes().find(
            (idx) => idx.name === ttl.name,
        );
        assert.eq(normalised.expireAfterSeconds, 4, normalised);
    },
});
