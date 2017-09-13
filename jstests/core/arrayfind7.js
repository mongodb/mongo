// Nested $elemMatch clauses.  SERVER-5741

t = db.jstests_arrayfind7;
t.drop();

t.save({a: [{b: [{c: 1, d: 2}]}]});

function checkElemMatchMatches() {
    assert.eq(1, t.count({a: {$elemMatch: {b: {$elemMatch: {c: 1, d: 2}}}}}));
}

// The document is matched using nested $elemMatch expressions, with and without an index.
checkElemMatchMatches();
t.ensureIndex({'a.b.c': 1});
checkElemMatchMatches();

function checkElemMatch(index, document, query) {
    // The document is matched without an index, and with single and multi key indexes.
    t.drop();
    t.save(document);
    assert.eq(1, t.count(query));
    t.ensureIndex(index);
    assert.eq(1, t.count(query));
    t.save({a: {b: {c: [10, 11]}}});  // Make the index multikey.
    assert.eq(1, t.count(query));
}

// Two constraints within a nested $elemMatch expression.
checkElemMatch({'a.b.c': 1},
               {a: [{b: [{c: 1}]}]},
               {a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1, $lte: 1}}}}}});

// Two constraints within a nested $elemMatch expression, one of which contains the other.
checkElemMatch({'a.b.c': 1},
               {a: [{b: [{c: 2}]}]},
               {a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1, $in: [2]}}}}}});

// Two nested $elemMatch expressions.
checkElemMatch({'a.d.e': 1, 'a.b.c': 1}, {a: [{b: [{c: 1}], d: [{e: 1}]}]}, {
    a: {$elemMatch: {d: {$elemMatch: {e: {$lte: 1}}}, b: {$elemMatch: {c: {$gte: 1}}}}}
});

// A non $elemMatch expression and a nested $elemMatch expression.
checkElemMatch({'a.x': 1, 'a.b.c': 1},
               {a: [{b: [{c: 1}], x: 1}]},
               {'a.x': 1, a: {$elemMatch: {b: {$elemMatch: {c: {$gte: 1}}}}}});

// $elemMatch is applied directly to a top level field.
checkElemMatch({'a.b.c': 1},
               {a: [{b: [{c: [1]}]}]},
               {a: {$elemMatch: {'b.c': {$elemMatch: {$gte: 1, $lte: 1}}}}});
