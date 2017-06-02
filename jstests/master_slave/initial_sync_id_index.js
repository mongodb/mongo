// Tests that the _id index spec is copied exactly during initial sync.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    const rt = new ReplTest();
    const master = rt.start(true);
    const masterDB = master.getDB("test");

    // Create a collection with a v=2 _id index.
    assert.commandWorked(
        masterDB.createCollection("collV2", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
    let spec = GetIndexHelpers.findByName(masterDB.collV2.getIndexes(), "_id_");
    assert.neq(spec, null);
    assert.eq(spec.v, 2);

    // Create a collection with a v=1 _id index.
    assert.commandWorked(
        masterDB.createCollection("collV1", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
    spec = GetIndexHelpers.findByName(masterDB.collV1.getIndexes(), "_id_");
    assert.neq(spec, null);
    assert.eq(spec.v, 1);

    // Initial sync a slave.
    const slave = rt.start(false);
    const slaveDB = slave.getDB("test");

    // Wait for the slave to sync the collections.
    assert.soon(function() {
        var res = slaveDB.runCommand({listCollections: 1, filter: {name: "collV2"}});
        return res.cursor.firstBatch.length === 1;
    }, "Collection with v:2 _id index failed to sync on slave");
    assert.soon(function() {
        var res = slaveDB.runCommand({listCollections: 1, filter: {name: "collV1"}});
        return res.cursor.firstBatch.length === 1;
    }, "Collection with v:1 _id index failed to sync on slave");

    // Check _id index versions on slave.
    spec = GetIndexHelpers.findByName(slaveDB.collV2.getIndexes(), "_id_");
    assert.neq(spec, null);
    assert.eq(spec.v, 2);
    spec = GetIndexHelpers.findByName(slaveDB.collV1.getIndexes(), "_id_");
    assert.neq(spec, null);
    assert.eq(spec.v, 1);

    rt.stop();
})();