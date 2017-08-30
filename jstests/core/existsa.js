/**
 * Tests that sparse indexes are disallowed for $exists:false queries.
 */
(function() {
    "use strict";

    const coll = db.jstests_existsa;
    coll.drop();

    assert.writeOK(coll.insert({}));
    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: {x: 1}, b: 1}));

    let indexKeySpec = {};
    let indexKeyField = '';

    /** Configure testing of an index { <indexKeyField>:1 }. */
    function setIndex(_indexKeyField) {
        indexKeyField = _indexKeyField;
        indexKeySpec = {};
        indexKeySpec[indexKeyField] = 1;
        coll.ensureIndex(indexKeySpec, {sparse: true});
    }
    setIndex('a');

    /** @return count when hinting the index to use. */
    function hintedCount(query) {
        return coll.find(query).hint(indexKeySpec).itcount();
    }

    /** The query field does not exist and the sparse index is not used without a hint. */
    function assertMissing(query, expectedMissing = 1, expectedIndexedMissing = 0) {
        assert.eq(expectedMissing, coll.count(query));
        // We also shouldn't get a different count depending on whether
        // an index is used or not.
        assert.eq(expectedIndexedMissing, hintedCount(query));
    }

    /** The query field exists and the sparse index is used without a hint. */
    function assertExists(query, expectedExists = 2) {
        assert.eq(expectedExists, coll.count(query));
        // An $exists:true predicate generates no index filters. Add another predicate on the index
        // key to trigger use of the index.
        let andClause = {};
        andClause[indexKeyField] = {$ne: null};
        Object.extend(query, {$and: [andClause]});
        assert.eq(expectedExists, coll.count(query));
        assert.eq(expectedExists, hintedCount(query));
    }

    /** The query field exists and the sparse index is not used without a hint. */
    function assertExistsUnindexed(query, expectedExists = 2) {
        assert.eq(expectedExists, coll.count(query));
        // Even with another predicate on the index key, the sparse index is disallowed.
        let andClause = {};
        andClause[indexKeyField] = {$ne: null};
        Object.extend(query, {$and: [andClause]});
        assert.eq(expectedExists, coll.count(query));
        assert.eq(expectedExists, hintedCount(query));
    }

    // $exists:false queries match the proper number of documents and disallow the sparse index.
    assertMissing({a: {$exists: false}});
    assertMissing({a: {$not: {$exists: true}}});
    assertMissing({$and: [{a: {$exists: false}}]});
    assertMissing({$or: [{a: {$exists: false}}]});
    assertMissing({$nor: [{a: {$exists: true}}]});
    assertMissing({'a.x': {$exists: false}}, 2, 1);

    // Currently a sparse index is disallowed even if the $exists:false query is on a different
    // field.
    assertMissing({b: {$exists: false}}, 2, 1);
    assertMissing({b: {$exists: false}, a: {$ne: 6}}, 2, 1);
    assertMissing({b: {$not: {$exists: true}}}, 2, 1);

    // Top level $exists:true queries match the proper number of documents
    // and use the sparse index on { a : 1 }.
    assertExists({a: {$exists: true}});

    // Nested $exists queries match the proper number of documents and disallow the sparse index.
    assertExistsUnindexed({$nor: [{a: {$exists: false}}]});
    assertExistsUnindexed({$nor: [{'a.x': {$exists: false}}]}, 1);
    assertExistsUnindexed({a: {$not: {$exists: false}}});

    // Nested $exists queries disallow the sparse index in some cases where it is not strictly
    // necessary to do so.  (Descriptive tests.)
    assertExistsUnindexed({$nor: [{b: {$exists: false}}]}, 1);  // Unindexed field.
    assertExists({$or: [{a: {$exists: true}}]});                // $exists:true not $exists:false.

    // Behavior is similar with $elemMatch.
    coll.drop();
    assert.writeOK(coll.insert({a: [{}]}));
    assert.writeOK(coll.insert({a: [{b: 1}]}));
    assert.writeOK(coll.insert({a: [{b: [1]}]}));
    setIndex('a.b');

    assertMissing({a: {$elemMatch: {b: {$exists: false}}}});

    // A $elemMatch predicate is treated as nested, and the index should be used for $exists:true.
    assertExists({a: {$elemMatch: {b: {$exists: true}}}});

    // A $not within $elemMatch should not attempt to use a sparse index for $exists:false.
    assertExistsUnindexed({'a.b': {$elemMatch: {$not: {$exists: false}}}}, 1);
    assertExistsUnindexed({'a.b': {$elemMatch: {$gt: 0, $not: {$exists: false}}}}, 1);

    // A non sparse index will not be disallowed.
    coll.drop();
    assert.writeOK(coll.insert({}));
    coll.ensureIndex({a: 1});
    assert.eq(1, coll.find({a: {$exists: false}}).itcount());
})();
