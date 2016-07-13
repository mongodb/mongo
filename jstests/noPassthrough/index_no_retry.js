// Check index rebuild is disabled with --noIndexBuildRetry when MongoDB is killed.
//
// This test requires persistence beacuase it assumes data/indices will survive a restart.
// This test requires journaling because the information that an index build was started
// must be made durable when the process aborts.
// @tags: [requires_persistence, requires_journaling]
(function() {
    'use strict';
    var baseName = 'index_no_retry';
    var dbpath = MongoRunner.dataPath + baseName;

    var conn = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, conn, 'failed to start mongod');

    var test = conn.getDB("test");

    var name = 'jstests_slownightly_' + baseName;
    var t = test.getCollection(name);
    t.drop();

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; ++i) {
        bulk.insert({a: i});
    }

    // Make sure the documents are journaled
    assert.writeOK(bulk.execute({j: true}));

    assert.eq(100, t.count(), 'unexpected number of documents after bulk insert.');

    function abortDuringIndexBuild(options) {
        var createIdx = startParallelShell(function() {
            var coll = db.getSiblingDB('test').getCollection('jstests_slownightly_index_no_retry');

            // Fail point will handle journal flushing and killing the mongod
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: 'crashAfterStartingIndexBuild', mode: 'alwaysOn'}));
            coll.createIndex({a: 1}, {background: true});
        }, conn.port);

        var exitCode = createIdx({checkExitSuccess: false});
        assert.neq(0, exitCode, "expected shell to exit abnormally due to mongod being terminated");
    }

    abortDuringIndexBuild();

    assert.eq(waitProgram(conn.pid),
              MongoRunner.EXIT_TEST,
              "mongod should have crashed due to the 'crashAfterStartingIndexBuild' " +
                  "failpoint being set.");

    conn = MongoRunner.runMongod({dbpath: dbpath, noIndexBuildRetry: '', restart: true});
    test = conn.getDB("test");
    t = test.getCollection(name);

    assert.throws(function() {
        t.find({}, {_id: 0, a: 1}).hint({a: 1}).next();
    }, null, 'index {a: 1} was rebuilt in spite of --noIndexBuildRetry');

    var indexes = t.getIndexes();
    assert.eq(1, indexes.length, 'unfinished indexes in listIndexes result: ' + tojson(indexes));

    MongoRunner.stopMongod(conn.port);
}());
