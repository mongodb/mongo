// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

// Tests creation of indexes using applyOps for collections with a non-simple default collation.
// Indexes created through applyOps should be built exactly according to their index spec, without
// inheriting the collection default collation, since this is how the oplog entries are replicated.
// TODO SERVER-31435: Move this test into core once applyOps with createIndexes replicates
// correctly.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    const coll = db.apply_ops_index_collation;
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));

    // An index created using an insert-style oplog entry with a non-simple collation does not
    // inherit the collection default collation.
    let res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "i",
            ns: db.system.indexes.getFullName(),
            o: {
                v: 2,
                key: {c: 1},
                name: "c_1_en",
                ns: coll.getFullName(),
                collation: {
                    locale: "en_US",
                    caseLevel: false,
                    caseFirst: "off",
                    strength: 3,
                    numericOrdering: false,
                    alternate: "non-ignorable",
                    maxVariable: "punct",
                    normalization: false,
                    backwards: false,
                    version: "57.1"
                }
            }
        }]
    }));
    let allIndexes = coll.getIndexes();
    let spec = GetIndexHelpers.findByName(allIndexes, "c_1_en");
    assert.neq(null, spec, "Index 'c_1_en' not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, tojson(spec));
    assert.eq("en_US", spec.collation.locale, tojson(spec));

    // An index created using an insert-style oplog entry with a simple collation does not inherit
    // the collection default collation.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "i",
            ns: db.system.indexes.getFullName(),
            o: {v: 2, key: {c: 1}, name: "c_1", ns: coll.getFullName()}
        }]
    }));
    allIndexes = coll.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "c_1");
    assert.neq(null, spec, "Index 'c_1' not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, tojson(spec));
    assert(!spec.hasOwnProperty("collation"), tojson(spec));

    // A v=1 index created using an insert-style oplog entry does not inherit the collection default
    // collation.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "i",
            ns: db.system.indexes.getFullName(),
            o: {v: 1, key: {d: 1}, name: "d_1", ns: coll.getFullName()}
        }]
    }));
    allIndexes = coll.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "d_1");
    assert.neq(null, spec, "Index 'd_1' not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, tojson(spec));
    assert(!spec.hasOwnProperty("collation"), tojson(spec));
})();
