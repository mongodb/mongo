// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// Uses features that require featureCompatibilityVersion 3.6.
// @tags: [assumes_no_implicit_collection_creation_after_drop, requires_fcv36,
// requires_non_retryable_commands]

// Tests creation of indexes using applyOps for collections with a non-simple default collation.
// Indexes created through applyOps should be built exactly according to their index spec, without
// inheriting the collection default collation, since this is how the oplog entries are replicated.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");
    load('jstests/libs/uuid_util.js');

    const coll = db.apply_ops_index_collation;
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
    const uuid = getUUIDFromListCollections(db, coll.getName());

    // An index created using a createIndexes-style oplog entry with a non-simple collation does not
    // inherit the collection default collation.
    let res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "c",
            ns: coll.getFullName(),
            ui: uuid,
            o: {
                createIndexes: coll.getFullName(),
                v: 2,
                key: {a: 1},
                name: "a_1_en",
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
    let spec = GetIndexHelpers.findByName(allIndexes, "a_1_en");
    assert.neq(null, spec, "Index 'a_1_en' not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, tojson(spec));
    assert.eq("en_US", spec.collation.locale, tojson(spec));

    // An index created using a createIndexes-style oplog entry with a simple collation does not
    // inherit the collection default collation.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "c",
            ns: coll.getFullName(),
            ui: uuid,
            o: {createIndexes: coll.getFullName(), v: 2, key: {a: 1}, name: "a_1"}
        }]
    }));
    allIndexes = coll.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "a_1");
    assert.neq(null, spec, "Index 'a_1' not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, tojson(spec));
    assert(!spec.hasOwnProperty("collation"), tojson(spec));

    // A v=1 index created using a createIndexes-style oplog entry does not inherit the collection
    // default collation.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: "c",
            ns: coll.getFullName(),
            ui: uuid,
            o: {createIndexes: coll.getFullName(), v: 1, key: {b: 1}, name: "b_1"}
        }]
    }));
    allIndexes = coll.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "b_1");
    assert.neq(null, spec, "Index 'b_1' not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, tojson(spec));
    assert(!spec.hasOwnProperty("collation"), tojson(spec));

    // An index created using an insert-style oplog entry with a non-simple collation does not
    // inherit the collection default collation.
    res = assert.commandWorked(db.adminCommand({
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
    allIndexes = coll.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "c_1_en");
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
