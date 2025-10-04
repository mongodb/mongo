/**
 * Tests that the exact _id index spec is replicated when v>=2, and a v=1 index is implicitly
 * created on the secondary when the index spec is not included in the oplog.
 */
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({nodes: 2});
rst.startSet();
let replSetConfig = rst.getReplSetConfig();
replSetConfig.members[1].priority = 0;
rst.initiate(replSetConfig);

let primaryDB = rst.getPrimary().getDB("test");
let oplogColl = rst.getPrimary().getDB("local").oplog.rs;
let secondaryDB = rst.getSecondary().getDB("test");

function testOplogEntryIdIndexSpec(collectionName, idIndexSpec) {
    let oplogEntry = oplogColl.findOne({op: "c", "o.create": collectionName});
    assert.neq(null, oplogEntry);
    if (idIndexSpec === null) {
        assert(!oplogEntry.o.hasOwnProperty("idIndex"), tojson(oplogEntry));
    } else {
        assert.eq(0, bsonUnorderedFieldsCompare(idIndexSpec, oplogEntry.o.idIndex), tojson(oplogEntry));
    }
}

assert.commandWorked(primaryDB.createCollection("without_version"));
let allIndexes = primaryDB.without_version.getIndexes();
let spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(2, spec.v, "Expected primary to build a v=2 _id index: " + tojson(spec));
testOplogEntryIdIndexSpec("without_version", spec);

assert.commandWorked(primaryDB.createCollection("version_v2", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
allIndexes = primaryDB.version_v2.getIndexes();
spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(2, spec.v, "Expected primary to build a v=2 _id index: " + tojson(spec));
testOplogEntryIdIndexSpec("version_v2", spec);

assert.commandWorked(primaryDB.createCollection("version_v1", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
allIndexes = primaryDB.version_v1.getIndexes();
spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(1, spec.v, "Expected primary to build a v=1 _id index: " + tojson(spec));
testOplogEntryIdIndexSpec("version_v1", null);

rst.awaitReplication();

// Verify that the secondary built _id indexes with the same version as on the primary.

allIndexes = secondaryDB.without_version.getIndexes();
spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(2, spec.v, "Expected secondary to build a v=2 _id index when explicitly requested: " + tojson(spec));

allIndexes = secondaryDB.version_v2.getIndexes();
spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(2, spec.v, "Expected secondary to build a v=2 _id index when explicitly requested: " + tojson(spec));

allIndexes = secondaryDB.version_v1.getIndexes();
spec = IndexCatalogHelpers.findByKeyPattern(allIndexes, {_id: 1});
assert.neq(null, spec, "_id index not found: " + tojson(allIndexes));
assert.eq(1, spec.v, "Expected secondary to implicitly build a v=1 _id index: " + tojson(spec));

rst.stopSet();
