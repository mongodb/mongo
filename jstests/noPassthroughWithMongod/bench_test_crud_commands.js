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
    var benchArgs = {ops: benchOps, parallel: 2, seconds: 5, host: db.getMongo().host};
    if (jsTest.options().auth) {
        benchArgs['db'] = 'admin';
        benchArgs['username'] = jsTest.options().authUser;
        benchArgs['password'] = jsTest.options().authPassword;
    }
    return benchRun(benchArgs);
}

function testInsert(docs, wc) {
    coll.drop();

    var res = executeBenchRun(
        [{ns: coll.getFullName(), op: "insert", doc: docs, writeCmd: true, writeConcern: wc}]);

    assert.gt(coll.count(), 0);
    assert.eq(coll.findOne({}, {_id: 0}), docs[0]);
}

function testFind() {
    coll.drop();
    for (var i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({}));
    }

    var res = executeBenchRun(
        [{ns: coll.getFullName(), op: "find", query: {}, batchSize: NumberInt(10), readCmd: true}]);
    assert.gt(res.query, 0, tojson(res));
}

function testFindOne() {
    coll.drop();
    for (var i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({}));
    }

    var res = executeBenchRun([{ns: coll.getFullName(), op: "findOne", query: {}, readCmd: true}]);
    assert.gt(res.findOne, 0, tojson(res));
}

function testWriteConcern() {
    var bigDoc = makeDocument(260 * 1024);
    var docs = [];
    for (var i = 0; i < 100; i++) {
        docs.push({x: 1});
    }

    testInsert([bigDoc], {});
    testInsert(docs, {});
    testInsert(docs, {"w": "majority"});
    testInsert(docs, {"w": 1, "j": false});

    if (jsTestOptions().storageEngine != "inMemory") {
        // Only test journaled writes if the server actually supports them.
        testInsert(docs, {"j": true});
    }
}

testWriteConcern();

testFind();
testFindOne();
})();
