// Tests the "writeCmd" and "readCmd" options to benchRun().
(function() {
    "use strict";

    var coll = db.bench_test_crud_commands;
    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));

    function makeDocument(docSize) {
        var doc = {"fieldName": ""};
        var longString = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        while (Object.bsonsize(doc) < docSize) {
            if (Object.bsonsize(doc) < docSize - longString.length) {
                doc.fieldName += longString;
            } else {
                doc.fieldName += "x";
            }
        }
        return doc;
    }

    function executeBenchRun(benchOps) {
        var benchArgs = {ops: benchOps, parallel: 2, seconds: 1, host: db.getMongo().host};
        if (jsTest.options().auth) {
            benchArgs['db'] = 'admin';
            benchArgs['username'] = jsTest.options().adminUser;
            benchArgs['password'] = jsTest.options().adminPassword;
        }
        return benchRun(benchArgs);
    }

    function testInsert(docs, writeCmd, wc) {
        coll.drop();

        var res = executeBenchRun([{
            ns: coll.getFullName(),
            op: "insert",
            doc: docs,
            writeCmd: writeCmd,
            writeConcern: wc
        }]);

        assert.gt(coll.count(), 0);
        assert.eq(coll.findOne({}, {_id: 0}), docs[0]);
    }

    function testFind(readCmd) {
        coll.drop();
        for (var i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({}));
        }

        var res = executeBenchRun([{
            ns: coll.getFullName(),
            op: "find",
            query: {},
            batchSize: NumberInt(10),
            readCmd: readCmd
        }]);
        assert.gt(res.query, 0, tojson(res));
    }

    function testFindOne(readCmd) {
        coll.drop();
        for (var i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({}));
        }

        var res =
            executeBenchRun([{ns: coll.getFullName(), op: "findOne", query: {}, readCmd: readCmd}]);
        assert.gt(res.findOne, 0, tojson(res));
    }

    function testWriteConcern(writeCmd) {
        var bigDoc = makeDocument(260 * 1024);
        var docs = [];
        for (var i = 0; i < 100; i++) {
            docs.push({x: 1});
        }

        testInsert([bigDoc], writeCmd, {});
        testInsert(docs, writeCmd, {});
        testInsert(docs, writeCmd, {"w": "majority"});
        testInsert(docs, writeCmd, {"w": 1, "j": false});
        testInsert(docs, writeCmd, {"j": true});
    }

    testWriteConcern(false);
    testWriteConcern(true);

    testFind(false);
    testFind(true);
    testFindOne(false);
    testFindOne(true);
})();
