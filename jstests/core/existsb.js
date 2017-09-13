// Tests for $exists against documents that store a null value
//
// A document with a missing value for an indexed field
// is indexed *as if* it had the value 'null' explicitly.
// Therefore:
//     { b : 1 }
//     { a : null, b : 1 }
// look identical based on a standard index on { a : 1 }.
//
// -- HOWEVER!! --
// A sparse index on { a : 1 } would include { a : null, b : 1 },
// but would not include { b : 1 }.  In this case, the two documents
// are treated equally.
//
// Also, super special edge case around sparse, compound indexes
// from Mathias:
//   If we have a sparse index on { a : 1, b : 1 }
//   And we insert docs {}, { a : 1 },
//                      { b : 1 }, and { a : 1, b : 1 }
//   everything but {} will have an index entry.
// Let's make sure we handle this properly!

t = db.jstests_existsb;
t.drop();

t.save({});
t.save({a: 1});
t.save({b: 1});
t.save({a: 1, b: null});
t.save({a: 1, b: 1});

/** run a series of checks, just on the number of docs found */
function checkExistsNull() {
    // Basic cases
    assert.eq(3, t.count({a: {$exists: true}}));
    assert.eq(2, t.count({a: {$exists: false}}));
    assert.eq(3, t.count({b: {$exists: true}}));
    assert.eq(2, t.count({b: {$exists: false}}));
    // With negations
    assert.eq(3, t.count({a: {$not: {$exists: false}}}));
    assert.eq(2, t.count({a: {$not: {$exists: true}}}));
    assert.eq(3, t.count({b: {$not: {$exists: false}}}));
    assert.eq(2, t.count({b: {$not: {$exists: true}}}));
    // Both fields
    assert.eq(2, t.count({a: 1, b: {$exists: true}}));
    assert.eq(1, t.count({a: 1, b: {$exists: false}}));
    assert.eq(1, t.count({a: {$exists: true}, b: 1}));
    assert.eq(1, t.count({a: {$exists: false}, b: 1}));
    // Both fields, both $exists
    assert.eq(2, t.count({a: {$exists: true}, b: {$exists: true}}));
    assert.eq(1, t.count({a: {$exists: true}, b: {$exists: false}}));
    assert.eq(1, t.count({a: {$exists: false}, b: {$exists: true}}));
    assert.eq(1, t.count({a: {$exists: false}, b: {$exists: false}}));
}

// with no index, make sure we get correct results
checkExistsNull();

// try with a standard index
t.ensureIndex({a: 1});
checkExistsNull();

// try with a sparse index
t.dropIndexes();
t.ensureIndex({a: 1}, {sparse: true});
checkExistsNull();

// try with a compound index
t.dropIndexes();
t.ensureIndex({a: 1, b: 1});
checkExistsNull();

// try with sparse compound index
t.dropIndexes();
t.ensureIndex({a: 1, b: 1}, {sparse: true});
checkExistsNull();
