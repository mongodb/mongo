/**
 * Tests that newer versions of mongod infer the absence of the "v" field in the oplog entry for the
 * index creation as a request to build a v=1 index.
 */
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    var replSetName = "index_version_missing";
    var nodes = [
        // The 3.2.1 version of mongod includes the index specification the user requested in the
        // corresponding oplog entry, and not the index specification the server actually built. We
        // therefore use this version of mongod to test the behavior of newer versions when the "v"
        // field isn't always present in the oplog entry for the index creation.
        {binVersion: "3.2.1"},
        {binVersion: "latest"},
    ];

    var rst = new ReplSetTest({name: replSetName, nodes: nodes});
    rst.startSet();

    // Rig the election so that the 3.2.1 node becomes the primary.
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;

    rst.initiate(replSetConfig);

    var primaryDB = rst.getPrimary().getDB("test");
    var secondaryDB = rst.getSecondary().getDB("test");

    assert.commandWorked(primaryDB.index_version_missing.createIndex({withoutAnyOptions: 1}));
    var allIndexes = primaryDB.index_version_missing.getIndexes();
    var spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withoutAnyOptions: 1});
    assert.neq(null,
               spec,
               "Index with key pattern {withoutAnyOptions: 1} not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected 3.2.1 primary to build a v=1 index: " + tojson(spec));

    assert.commandWorked(primaryDB.index_version_missing.createIndex({withV1: 1}, {v: 1}));
    allIndexes = primaryDB.index_version_missing.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withV1: 1});
    assert.neq(null, spec, "Index with key pattern {withV1: 1} not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected 3.2.1 primary to build a v=1 index: " + tojson(spec));

    rst.awaitReplication();

    // Verify that the 3.4 secondary builds a v=1 index when the index version is omitted from the
    // corresponding oplog entry.
    allIndexes = secondaryDB.index_version_missing.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withoutAnyOptions: 1});
    assert.neq(null,
               spec,
               "Index with key pattern {withoutAnyOptions: 1} not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected 3.4 secondary to implicitly build a v=1 index: " + tojson(spec));

    // Verify that the 3.4 secondary builds a v=1 index when it is explicitly requested by the user.
    allIndexes = secondaryDB.index_version_missing.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withV1: 1});
    assert.neq(null, spec, "Index with key pattern {withV1: 1} not found: " + tojson(allIndexes));
    assert.eq(
        1,
        spec.v,
        "Expected 3.4 secondary to build a v=1 index when explicitly requested: " + tojson(spec));

    // Verify that the 3.4 secondary builds a v=1 _id index.
    allIndexes = secondaryDB.index_version_missing.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "Index with key pattern {_id: 1} not found: " + tojson(allIndexes));
    assert.eq(
        1, spec.v, "Expected 3.4 secondary to implicitly build a v=1 _id index: " + tojson(spec));

    rst.stopSet();
})();
