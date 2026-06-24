/**
 * Steps:
 *   1. Create a collection, an index, and insert a few documents.
 *   2. Capture the cluster time at which the collection still exists ("the snapshot").
 *   3. Drop the collection and recreate it. The recreated collection has a brand new catalog id
 *      that does NOT exist at the snapshot timestamp.
 *   4. Run whole-instance offline validation (mongod --validate) at the captured snapshot time.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

describe("Offline validation of a dropped collection against an older snapshot", () => {
    const dbName = jsTestName();
    const collName = "coll";
    let dbpath;
    let snapshotTime;
    let dbpathIdx = 0;

    beforeEach(() => {
        dbpath = MongoRunner.dataPath + jsTestName() + dbpathIdx++;
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet({dbpath: dbpath});
        rst.initiate();

        const primary = rst.getPrimary();
        const db = primary.getDB(dbName);

        assertDropCollection(db, collName);
        assert.commandWorked(db.createCollection(collName));
        assert.commandWorked(db[collName].createIndex({a: 1}));
        const insertRes = assert.commandWorked(
            db.runCommand({insert: collName, documents: [{a: 1}, {a: 2}, {a: 3}]}),
        );

        // The snapshot: the cluster time at which the collection still exists with its original
        // catalog id, data and indexes.
        snapshotTime = insertRes.operationTime;
        jsTest.log.info("Captured snapshot time", {snapshotTime});

        rst.awaitLastOpCommitted();
        assert.commandWorked(db.adminCommand({fsync: 1}));

        // After the snapshot, drop and recreate the collection. The recreated collection has a brand
        // new catalog id that does not exist at the snapshot timestamp, while still being present in
        // the latest durable catalog.
        assertDropCollection(db, collName);
        assert.commandWorked(db.createCollection(collName));
        assert.commandWorked(db[collName].createIndex({a: 1}));
        rst.awaitLastOpCommitted();
        assert.commandWorked(db.adminCommand({fsync: 1}));

        // Shut the replica set down but keep the data files so offline validation can run against
        // them.
        rst.stopSet(null /* signal */, true /* forRestart */);

        clearRawMongoProgramOutput();
    });

    for (const validationCliFlag of ["--validate", "--validateParallel"]) {
        it(`exits cleanly instead of crashing with an uncaught exception with \`${validationCliFlag}\``, () => {
            const exitCode = runMongoProgram(
                "mongod",
                validationCliFlag,
                "--dbpath",
                dbpath,
                "--port",
                allocatePort(),
                "--setParameter",
                `collectionValidateOptions={options: {atClusterTime: ${tojson(snapshotTime)}}}`,
            );

            const output = rawMongoProgramOutput(".*");
            jsTest.log.info("Offline validation exit code", {exitCode});

            // The uncaught Location10065 surfaces as log id 20557 ("DBException in initAndListen,
            // terminating"). Match the structured-log key to avoid false positives from WiredTiger
            // messages that coincidentally contain "20557" as a microsecond timestamp value.
            assert(
                !output.includes('"id":20557'),
                "Offline validation terminated with an uncaught exception in initAndListen",
                {exitCode},
            );
            assert.eq(
                MongoRunner.EXIT_FAIL,
                exitCode,
                "Offline validation of a dropped collection against an older snapshot should exit cleanly",
            );
        });
    }
});
