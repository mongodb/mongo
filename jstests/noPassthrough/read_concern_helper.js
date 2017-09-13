// This tests readConcern handling for the find/findOne shell helpers It runs find commands that
// should fail without --enableMajorityReadConcern enabled and then reruns the find commands with
// that option enabled.
(function() {
    "use strict";
    var testServer = MongoRunner.runMongod();
    if (!testServer.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        MongoRunner.stopMongod(testServer);
        return;
    }
    var coll = testServer.getDB("test").readMajority;

    assert.writeOK(coll.insert({_id: "foo"}));
    assert.throws(function() {
        coll.find({_id: "foo"}).readConcern("majority").itcount();
    });
    assert.throws(function() {
        coll.findOne({_id: "foo"}, {}, {}, "majority");
    });
    assert.throws(function() {
        coll.count({_id: "foo"}, {readConcern: "majority"});
    });
    assert.throws(function() {
        coll.find({_id: "foo"}).readConcern("majority").count();
    });

    MongoRunner.stopMongod(testServer);
    testServer = MongoRunner.runMongod({
        restart: true,
        port: testServer.port,
        enableMajorityReadConcern: "",
    });
    coll = testServer.getDB("test").readMajority;

    assert.doesNotThrow(function() {
        coll.find({_id: "foo"}).readConcern("majority").itcount();
    });
    assert.doesNotThrow(function() {
        coll.findOne({_id: "foo"}, {}, {}, "majority");
    });
    assert.doesNotThrow(function() {
        coll.count({_id: "foo"}, {readConcern: "majority"});
    });
    assert.doesNotThrow(function() {
        coll.find({_id: "foo"}).readConcern("majority").count();
    });

    MongoRunner.stopMongod(testServer);
}());
