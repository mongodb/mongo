// Sparse indexes are disallowed for $exists:false queries.  SERVER-3918

t = db.jstests_existsa;
t.drop();

t.save({});
t.save({a: 1});
t.save({a: {x: 1}, b: 1});

/** Configure testing of an index { <indexKeyField>:1 }. */
function setIndex(_indexKeyField) {
    indexKeyField = _indexKeyField;
    indexKeySpec = {};
    indexKeySpec[indexKeyField] = 1;
    t.ensureIndex(indexKeySpec, {sparse: true});
}
setIndex('a');

/** @return count when hinting the index to use. */
function hintedCount(query) {
    return t.find(query).hint(indexKeySpec).itcount();
}

/** The query field does not exist and the sparse index is not used without a hint. */
function assertMissing(query, expectedMissing, expectedIndexedMissing) {
    expectedMissing = expectedMissing || 1;
    expectedIndexedMissing = expectedIndexedMissing || 0;
    assert.eq(expectedMissing, t.count(query));
    // We also shouldn't get a different count depending on whether
    // an index is used or not.
    assert.eq(expectedIndexedMissing, hintedCount(query));
}

/** The query field exists and the sparse index is used without a hint. */
function assertExists(query, expectedExists) {
    expectedExists = expectedExists || 2;
    assert.eq(expectedExists, t.count(query));
    // An $exists:true predicate generates no index filters.  Add another predicate on the index key
    // to trigger use of the index.
    andClause = {};
    andClause[indexKeyField] = {$ne: null};
    Object.extend(query, {$and: [andClause]});
    assert.eq(expectedExists, t.count(query));
    assert.eq(expectedExists, hintedCount(query));
}

/** The query field exists and the sparse index is not used without a hint. */
function assertExistsUnindexed(query, expectedExists) {
    expectedExists = expectedExists || 2;
    assert.eq(expectedExists, t.count(query));
    // Even with another predicate on the index key, the sparse index is disallowed.
    andClause = {};
    andClause[indexKeyField] = {$ne: null};
    Object.extend(query, {$and: [andClause]});
    assert.eq(expectedExists, t.count(query));
    assert.eq(expectedExists, hintedCount(query));
}

// $exists:false queries match the proper number of documents and disallow the sparse index.
assertMissing({a: {$exists: false}});
assertMissing({a: {$not: {$exists: true}}});
assertMissing({$and: [{a: {$exists: false}}]});
assertMissing({$or: [{a: {$exists: false}}]});
assertMissing({$nor: [{a: {$exists: true}}]});
assertMissing({'a.x': {$exists: false}}, 2, 1);

// Currently a sparse index is disallowed even if the $exists:false query is on a different field.
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
t.drop();
t.save({a: [{}]});
t.save({a: [{b: 1}]});
t.save({a: [{b: 1}]});
setIndex('a.b');

assertMissing({a: {$elemMatch: {b: {$exists: false}}}});
// A $elemMatch predicate is treated as nested, and the index should be used for $exists:true.
assertExists({a: {$elemMatch: {b: {$exists: true}}}});

// A non sparse index will not be disallowed.
t.drop();
t.save({});
t.ensureIndex({a: 1});
assert.eq(1, t.find({a: {$exists: false}}).itcount());
