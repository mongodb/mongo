// Check index rebuild when MongoDB is killed.
//
// This test requires persistence beacuase it assumes data/indices will survive a restart.
// @tags: [requires_persistence]
(function() {
    'use strict';
    var baseName = 'index_retry';
    var dbpath = MongoRunner.dataPath + baseName;

    var conn = MongoRunner.runMongod({dbpath: dbpath, journal: ''});

    var test = conn.getDB("test");

    var name = 'jstests_slownightly_' + baseName;
    var t = test.getCollection(name);
    t.drop();

    // Insert a large number of documents, enough to ensure that an index build on these documents
    // can be interrupted before complete.
    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 5e5; ++i) {
        bulk.insert({a: i});
        if (i % 10000 == 0) {
            print("i: " + i);
        }
    }
    assert.writeOK(bulk.execute());

    function debug(x) {
        printjson(x);
    }

    /**
     * @return if there's a current running index build
     */
    function indexBuildInProgress() {
        var inprog = test.currentOp().inprog;
        debug(inprog);
        var indexBuildOpId = -1;
        inprog.forEach(function(op) {
            // Identify the index build as a createIndexes command.
            // It is assumed that no other clients are concurrently
            // accessing the 'test' database.
            if ((op.op == 'query' || op.op == 'command') && 'createIndexes' in op.query) {
                debug(op.opid);
                var idxSpec = op.query.indexes[0];
                // SERVER-4295 Make sure the index details are there
                // we can't assert these things, since there is a race in reporting
                // but we won't count if they aren't
                if ("a_1" == idxSpec.name && 1 == idxSpec.key.a && idxSpec.background &&
                    op.progress && (op.progress.done / op.progress.total) > 0.20) {
                    indexBuildOpId = op.opid;
                }
            }
        });
        return indexBuildOpId != -1;
    }

    function abortDuringIndexBuild(options) {
        var createIdx = startParallelShell(
            'db.' + name + '.createIndex({ a: 1 }, { background: true });', conn.port);

        // Wait for the index build to start.
        var times = 0;
        assert.soon(function() {
            return indexBuildInProgress() && times++ >= 2;
        });

        print("killing the mongod");
        MongoRunner.stopMongod(conn.port, /* signal */ 9);

        var exitCode = createIdx({checkExitSuccess: false});
        assert.neq(0, exitCode, "expected shell to exit abnormally due to mongod being terminated");
    }

    abortDuringIndexBuild();

    conn = MongoRunner.runMongod({dbpath: dbpath, journal: '', restart: true});
    test = conn.getDB("test");
    t = test.getCollection(name);

    assert.eq({a: 42},
              t.find({a: 42}, {_id: 0}).hint({a: 1}).next(),
              'index {a: 1} was rebuilt on startup');

    var indexes = t.getIndexes();
    assert.eq(2,
              indexes.length,
              'unexpected number of indexes in listIndexes result: ' + tojson(indexes));

    print("Index built");

    MongoRunner.stopMongod(conn.port);
    print("SUCCESS!");
}());
