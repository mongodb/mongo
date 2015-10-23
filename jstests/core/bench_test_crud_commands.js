// Tests the "writeCmd" and "readCmd" options to benchRun().
(function() {
    "use strict";

    var coll = db.bench_test_crud_commands;

    function executeBenchRun(benchOps) {
        var benchArgs = {ops: benchOps, parallel: 2, seconds: 1, host: db.getMongo().host};
        if (jsTest.options().auth) {
            benchArgs['db'] = 'admin';
            benchArgs['username'] = jsTest.options().adminUser;
            benchArgs['password'] = jsTest.options().adminPassword;
        }
        return benchRun(benchArgs);
    }

    function testInsert(writeCmd) {
        coll.drop();

        var docs = [];
        for (var i = 0; i < 100; i++) {
            docs.push({x: 1});
        }
        var res = executeBenchRun([{ns: coll.getFullName(),
                                    op: "insert",
                                    doc: docs,
                                    writeCmd: writeCmd}]);

        assert.gt(coll.count(), 0);
        assert.eq(coll.findOne({}, {_id:0}), docs[0]);
        assert.gt(res.insert, 0, tojson(res));
    }

    function testFind(readCmd) {
        coll.drop();
        for (var i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({}));
        }

        var res = executeBenchRun([{ns: coll.getFullName(),
                                    op: "find",
                                    query: {},
                                    batchSize: NumberInt(10),
                                    readCmd: readCmd}]);
        assert.gt(res.query, 0, tojson(res));
    }

    function testFindOne(readCmd) {
        coll.drop();
        for (var i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({}));
        }

        var res = executeBenchRun([{ns: coll.getFullName(),
                                    op: "findOne",
                                    query: {},
                                    readCmd: readCmd}]);
        assert.gt(res.findOne, 0, tojson(res));
    }

    testInsert(false);
    testInsert(true);
    testFind(false);
    testFind(true);
    testFindOne(false);
    testFindOne(true);
})();
