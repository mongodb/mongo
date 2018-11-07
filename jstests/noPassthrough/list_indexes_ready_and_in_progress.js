/**
 * Tests that the listIndexes command's default is to only show ready indexes; and that
 * the 'includeIndexBuilds' flag can be set to include indexes that are still building
 * along with the ready indexes.
 */
(function() {
    "use strict";

    load("jstests/noPassthrough/libs/index_build.js");

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.dropDatabase());

    /**
     * Runs listIndexes command on collection.
     * If 'options' is provided, these will be sent along with the command request.
     * Asserts that all the indexes on this collection fit within the first batch of results.
     */
    function assertIndexes(coll, numIndexes, readyIndexes, notReadyIndexes, options) {
        notReadyIndexes = notReadyIndexes || [];
        options = options || {};

        let res = coll.runCommand("listIndexes", options);
        assert.eq(numIndexes, res.cursor.firstBatch.length);

        // First batch contains all the indexes in the collection.
        assert.eq(0, res.cursor.id);

        // A map of index specs keyed by index name.
        const indexMap = res.cursor.firstBatch.reduce(
            (m, spec) => {
                m[spec.name] = spec;
                return m;
            },
            {});

        // Check ready indexes.
        for (let name of readyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'ready index ' + name + ' missing from listIndexes result: ' + tojson(res));
            const spec = indexMap[name];
            assert(!spec.hasOwnProperty('buildUUID'),
                   'unexpected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
        }

        // Check indexes that are not ready.
        for (let name of notReadyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'not-ready index ' + name + ' missing from listIndexes result: ' + tojson(res));
            const spec = indexMap[name];
            assert(spec.hasOwnProperty('buildUUID'),
                   'expected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
        }
    }

    let coll = testDB.list_indexes_ready_and_in_progress;
    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName()));
    assertIndexes(coll, 1, ["_id_"]);
    assert.commandWorked(coll.createIndex({a: 1}));
    assertIndexes(coll, 2, ["_id_", "a_1"]);

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));
    const createIdx = startParallelShell(
        "let coll = db.getSiblingDB('test').list_indexes_ready_and_in_progress;" +
            "assert.commandWorked(coll.createIndex({ b: 1 }, { background: true }));",
        conn.port);
    assert.soon(function() {
        return getIndexBuildOpId(testDB) != -1;
    }, "Index build operation not found after starting via parallelShell");

    // Verify there is no third index.
    assertIndexes(coll, 2, ["_id_", "a_1"]);

    // The listIndexes command supports returning all indexes, including ones that are not ready.
    assertIndexes(coll, 3, ["_id_", "a_1"], ["b_1"], {includeIndexBuilds: true});

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
    // Wait for the index build to stop.
    assert.soon(function() {
        return getIndexBuildOpId(testDB) == -1;
    });
    const exitCode = createIdx();
    assert.eq(0, exitCode, 'expected shell to exit cleanly');

    assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
    MongoRunner.stopMongod(conn);
}());
