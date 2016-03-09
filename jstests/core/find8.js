// SERVER-1932 Test unindexed matching of a range that is only valid in a multikey context.

t = db.jstests_find8;
t.drop();

t.save({a: [1, 10]});
assert.eq(1, t.count({a: {$gt: 2, $lt: 5}}));

// Check that we can do a query with 'invalid' range.
assert.eq(1, t.count({a: {$gt: 5, $lt: 2}}));

t.save({a: [-1, 12]});

// Check that we can do a query with 'invalid' range and sort.
assert.eq(2, t.find({a: {$gt: 5, $lt: 2}}).sort({a: 1}).itcount());
assert.eq(2, t.find({a: {$gt: 5, $lt: 2}}).sort({$natural: -1}).itcount());

// SERVER-2864
if (0) {
    t.find({a: {$gt: 5, $lt: 2}}).itcount();
    // Check that we can record a plan for an 'invalid' range.
    assert(t.find({a: {$gt: 5, $lt: 2}}).explain(true).oldPlan);
}
