// Test that limit is applied by explain.

t = db.jstests_explain4;
t.drop();

t.ensureIndex({a: 1});

for (i = 0; i < 10; ++i) {
    t.save({a: i, b: 0});
}

explain = t.find({a: {$gte: 0}, b: 0}).sort({a: 1}).hint({a: 1}).limit(5).explain(true);

// Five results are expected, matching the limit spec.
assert.eq(5, explain.executionStats.nReturned);
