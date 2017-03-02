// Check index rebuild when BongoDB is killed.
//
// This test requires persistence because it assumes data/indices will survive a restart.
// This test requires journaling because the information that an index build was started
// must be made durable when the process aborts.
// @tags: [requires_persistence, requires_journaling]
(function() {
    'use strict';
    var baseName = 'index_retry';
    var dbpath = BongoRunner.dataPath + baseName;

    var conn = BongoRunner.runBongod({dbpath: dbpath});
    assert.neq(null, conn, 'failed to start bongod');

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

    function abortDuringIndexBuild() {
        var createIdx = startParallelShell(function() {
            var coll = db.getSiblingDB('test').getCollection('jstests_slownightly_index_retry');

            // Fail point will handle journal flushing and killing the bongod
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: 'crashAfterStartingIndexBuild', mode: 'alwaysOn'}));
            coll.createIndex({a: 1}, {background: true});
        }, conn.port);

        var exitCode = createIdx({checkExitSuccess: false});
        assert.neq(0, exitCode, "expected shell to exit abnormally due to bongod being terminated");
    }

    abortDuringIndexBuild();

    assert.eq(waitProgram(conn.pid),
              BongoRunner.EXIT_TEST,
              "bongod should have crashed due to the 'crashAfterStartingIndexBuild' " +
                  "failpoint being set.");

    conn = BongoRunner.runBongod({dbpath: dbpath, restart: true});
    test = conn.getDB("test");
    t = test.getCollection(name);

    assert.eq(100,
              t.find({}, {_id: 0, a: 1}).hint({a: 1}).itcount(),
              'index {a: 1} was expected to be rebuilt on startup');
    var indexes = t.getIndexes();
    assert.eq(2,
              indexes.length,
              'unexpected number of indexes in listIndexes result: ' + tojson(indexes));

    BongoRunner.stopBongod(conn.port);
}());
