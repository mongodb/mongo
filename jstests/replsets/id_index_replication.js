/**
 * Tests that the exact _id index spec is replicated when v>=2, and a v=1 index is implicitly
 * created on the secondary when the index spec is not included in the oplog.
 */
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    var rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    rst.initiate(replSetConfig);

    var primaryDB = rst.getPrimary().getDB("test");
    var oplogColl = rst.getPrimary().getDB("local").oplog.rs;
    var secondaryDB = rst.getSecondary().getDB("test");

    function testOplogEntryIdIndexSpec(collectionName, idIndexSpec) {
        var oplogEntry = oplogColl.findOne({op: "c", "o.create": collectionName});
        assert.neq(null, oplogEntry);
        if (idIndexSpec === null) {
            assert(!oplogEntry.o.hasOwnProperty("idIndex"), tojson(oplogEntry));
        } else {
            assert.eq(0, bsonWoCompare(idIndexSpec, oplogEntry.o.idIndex), tojson(oplogEntry));
        }
    }

    assert.commandWorked(primaryDB.createCollection("without_version"));
    var allIndexes = primaryDB.without_version.getIndexes();
    var spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, "Expected primary to build a v=2 _id index: " + tojson(spec));
    testOplogEntryIdIndexSpec("without_version", spec);

    assert.commandWorked(
        primaryDB.createCollection("version_v2", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
    allIndexes = primaryDB.version_v2.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, "Expected primary to build a v=2 _id index: " + tojson(spec));
    testOplogEntryIdIndexSpec("version_v2", spec);

    assert.commandWorked(
        primaryDB.createCollection("version_v1", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
    allIndexes = primaryDB.version_v1.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected primary to build a v=1 _id index: " + tojson(spec));
    testOplogEntryIdIndexSpec("version_v1", null);

    assert.commandWorked(primaryDB.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    var res = primaryDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion, tojson(res));

    assert.commandWorked(primaryDB.createCollection("without_version_FCV32"));
    allIndexes = primaryDB.without_version_FCV32.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected primary to build a v=1 _id index: " + tojson(spec));
    testOplogEntryIdIndexSpec("without_version_FCV32", null);

    rst.awaitReplication();

    // Verify that the secondary built _id indexes with the same version as on the primary.

    allIndexes = secondaryDB.without_version.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(
        2,
        spec.v,
        "Expected secondary to build a v=2 _id index when explicitly requested: " + tojson(spec));

    allIndexes = secondaryDB.version_v2.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(
        2,
        spec.v,
        "Expected secondary to build a v=2 _id index when explicitly requested: " + tojson(spec));

    allIndexes = secondaryDB.version_v1.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected secondary to implicitly build a v=1 _id index: " + tojson(spec));

    allIndexes = secondaryDB.without_version_FCV32.getIndexes();
    spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected secondary to implicitly build a v=1 _id index: " + tojson(spec));

    rst.stopSet();
})();
