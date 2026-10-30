/**
 * Concurrent writes must not race when a collection validator evaluates a $convert.
 * The parsed validator shares its ExpressionContext across operation threads.
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   uses_parallel_shell,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

describe("Concurrent conversion in collection validators", function () {
    let conn;
    let testDB;
    const shells = [];
    const dbName = jsTestName();
    const numClients = 16;
    const numCollections = 64;
    const iterations = 128;

    before(function () {
        conn = MongoRunner.runMongod();
        assert.neq(null, conn, "mongod failed to start");
        testDB = conn.getDB(dbName);
    });

    after(function () {
        try {
            try {
                if (testDB && shells.length) {
                    // Release waiting workers even when setup or the readiness assertion failed.
                    assert.commandWorked(
                        testDB.coordination.update(
                            {_id: "release"},
                            {$set: {go: true}},
                            {upsert: true},
                        ),
                    );
                }
            } finally {
                while (shells.length) {
                    shells.shift()({checkExitSuccess: false});
                }
            }
        } finally {
            if (conn) {
                MongoRunner.stopMongod(conn);
            }
        }
    });

    it("safely initializes the cache on concurrent first validation", function () {
        assert.commandWorked(testDB.coordination.insert({_id: "release", go: false}));

        // Populate before installing the validator to avoid initializing its cache during setup.
        // Each collection supplies a fresh cache; repeated writes to one validator do not.
        for (let collection = 0; collection < numCollections; ++collection) {
            const coll = testDB.getCollection("c" + collection);
            assert.commandWorked(
                coll.insert(Array.from({length: numClients}, (_, i) => ({_id: i, a: i}))),
            );
            assert.commandWorked(
                testDB.runCommand({
                    collMod: coll.getName(),
                    validator: {$expr: {$eq: ["$a", {$toInt: "$a"}]}},
                }),
            );
        }

        // The coordination collection has no validator, so waiting does not initialize the cache.
        for (let client = 0; client < numClients; ++client) {
            const jsCode = funWithArgs(
                function (dbName, iterations, client, numCollections) {
                    const clientDB = db.getSiblingDB(dbName);
                    assert.commandWorked(clientDB.coordination.insert({_id: client}));
                    assert.soon(
                        () => clientDB.coordination.findOne({_id: "release"}).go,
                        "Waiting for all validation workers to be ready",
                    );
                    for (let i = 0; i < iterations; ++i) {
                        const result = assert.commandWorked(
                            clientDB.runCommand({
                                update: "c" + (i % numCollections),
                                updates: [{q: {_id: client}, u: {$inc: {a: 1}}}],
                            }),
                        );
                        assert.eq(1, result.n);
                        assert.eq(1, result.nModified);
                    }
                },
                dbName,
                iterations,
                client,
                numCollections,
            );
            shells.push(startParallelShell(jsCode, conn.port, false));
        }
        assert.soon(
            () => testDB.coordination.countDocuments({_id: {$type: "number"}}) === numClients,
            "Waiting for all validation workers to start",
        );
        assert.commandWorked(testDB.coordination.update({_id: "release"}, {$set: {go: true}}));
        while (shells.length) {
            shells.shift()({checkExitSuccess: true});
        }

        for (let collection = 0; collection < numCollections; ++collection) {
            const coll = testDB.getCollection("c" + collection);
            assert.eq(numClients, coll.countDocuments({}));
            for (let client = 0; client < numClients; ++client) {
                assert.eq(client + iterations / numCollections, coll.findOne({_id: client}).a);
            }
            assert(coll.drop());
        }
        assert(testDB.coordination.drop());
    });
});
