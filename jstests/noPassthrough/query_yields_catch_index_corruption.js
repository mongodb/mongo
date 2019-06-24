// Note: This test will fail burn_in for some build variants. We expect each execution to generate a
// core dump (because of an expected fassert failure). When the burn_in test runs the test in a
// tight loop, the core dumps can consume all available disk space and cause writes to fail.
// @tags: [requires_persistence, requires_journaling]
(function() {
    "use strict";

    const name = "query_yields_catch_index_corruption";
    const dbpath = MongoRunner.dataPath + name + "/";

    resetDbpath(dbpath);

    let mongod = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, mongod, "mongod failed to start.");

    let db = mongod.getDB("test");

    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "skipUnindexingDocumentWhenDeleted", mode: "alwaysOn"}));

    let coll = db.getCollection(name);
    coll.drop();

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.writeOK(coll.insert({a: 0}));

    // Corrupt the index by deleting the document but not unindexing it.
    assert.commandWorked(coll.remove({a: 0}));
    let validateRes = assert.commandWorked(coll.validate());
    assert.eq(false, validateRes.valid);

    assert.throws(() => coll.find({a: 0}).toArray());

    // fassert() calls std::abort(), which returns a different exit code for Windows vs. other
    // platforms.
    const exitCode = _isWindows() ? MongoRunner.EXIT_ABRUPT : MongoRunner.EXIT_ABORT;
    MongoRunner.stopMongod(mongod, null, {allowedExitCode: exitCode});

    // Test that the --repair flag rebuilds the corrupted index.
    mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, repair: ""});
    assert.eq(null, mongod, "Expect this to exit cleanly");

    // Restarting the server.
    mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    assert.neq(null, mongod, "mongod failed to start after repair");

    db = mongod.getDB("test");
    coll = db.getCollection(name);

    validateRes = assert.commandWorked(coll.validate());
    assert.eq(true, validateRes.valid, tojson(validateRes));

    MongoRunner.stopMongod(mongod);
})();