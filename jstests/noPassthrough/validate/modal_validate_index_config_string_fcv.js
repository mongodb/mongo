/**
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";

describe("offline validation of an index with a WiredTiger configString", () => {
    const dbpath = MongoRunner.dataPath + jsTestName();

    before(() => {
        // Set up a datadir containing a collection whose secondary index sets a custom WiredTiger
        // configString.
        const conn = MongoRunner.runMongod({dbpath: dbpath});
        const testDB = conn.getDB("test");
        assert.commandWorked(testDB.createCollection("coll"));
        assert.commandWorked(testDB["coll"].insert({a: 1}));
        assert.commandWorked(
            testDB["coll"].createIndex(
                {a: 1},
                {storageEngine: {wiredTiger: {configString: "block_compressor=snappy"}}},
            ),
        );
        MongoRunner.stopMongod(conn);
    });

    it("completes cleanly regardless of FCV state", () => {
        const exitCode = runMongoProgram(
            "mongod",
            "--validate",
            "--port",
            allocatePort(),
            "--dbpath",
            dbpath,
        );
        assert.eq(
            MongoRunner.EXIT_CLEAN,
            exitCode,
            "Offline validation of a collection with an index configString should complete cleanly",
        );
    });
});
