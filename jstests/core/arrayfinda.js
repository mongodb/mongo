// Assorted $elemMatch matching behavior checks.

t = db.jstests_arrayfinda;
t.drop();

// $elemMatch only matches elements within arrays (a descriptive, not a normative test).
t.save({a: [{b: 1}]});
t.save({a: {b: 1}});

function assertExpectedMatch(cursor) {
    assert.eq([{b: 1}], cursor.next().a);
    assert(!cursor.hasNext());
}

assertExpectedMatch(t.find({a: {$elemMatch: {b: {$gte: 1}}}}));
assertExpectedMatch(t.find({a: {$elemMatch: {b: 1}}}));

// $elemMatch is not used to perform key matching.  SERVER-6001
t.ensureIndex({a: 1});
assertExpectedMatch(t.find({a: {$elemMatch: {b: {$gte: 1}}}}).hint({a: 1}));
assertExpectedMatch(t.find({a: {$elemMatch: {b: 1}}}).hint({a: 1}));
