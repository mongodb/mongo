/*
 * Starting in MongoDB v3.6, `--enableMajorityReadConcern` is always on. Previously the startup
 * option parsing would disallow the flag being set with a storage engine that does not support
 * the feature. This has now become a runtime check on every query that requests the majority read
 * concern.
 *
 * This test makes sure an MMAP replica set node will return an error when asked to respond to a
 * read concern majority request.
 *
 * This test requires mmapv1, but does not rely on the tag to ensure this.
 */
(function() {
    "use strict";

    {
        var testServer = MongoRunner.runMongod();
        if (testServer.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
            jsTest.log("skipping test since storage engine supports committed reads");
            MongoRunner.stopMongod(testServer);
            return;
        }
        MongoRunner.stopMongod(testServer);
    }

    let numNodes = 2;
    let rst = new ReplSetTest({name: "mmap_disallows_rc_majority", nodes: numNodes});
    rst.startSet();
    rst.initiate();

    let collName = "test.foo";
    assert.writeOK(
        rst.getPrimary().getCollection(collName).insert({}, {writeConcern: {w: numNodes}}));

    assert.throws(function() {
        rst.getPrimary().getCollection(collName).findOne({}, {}, {}, "majority");
    }, [], "Expected `findOne` to throw an exception on the failed {readConcern: majority}");
    assert.throws(function() {
        rst.getSecondary().getCollection(collName).findOne({}, {}, {}, "majority");
    }, [], "Expected `findOne` to throw an exception on the failed {readConcern: majority}");

    rst.stopSet();
})();
