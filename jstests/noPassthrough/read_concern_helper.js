// This tests readConcern handling for the find/findOne shell helpers.
// @tags: [requires_majority_read_concern]
(function() {
    "use strict";
    var testServer = MongoRunner.runMongod();
    if (!testServer.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        MongoRunner.stopMongod(testServer);
        return;
    }
    var coll = testServer.getDB("test").readMajority;

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
