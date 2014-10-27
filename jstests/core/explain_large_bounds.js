// Make sure explain succeeds even when the index bounds are really big.

var t = db.jstests_explain_large_bounds;
t.drop();

t.ensureIndex({a: 1});

var inClause = [];
for (var i = 0; i < 1000000; i++) {
    inClause.push(i);
}

assert.commandWorked(t.find({a: {$in: inClause}}).explain());
