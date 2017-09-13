// Test covered index projection with $or clause, specifically in getMore
// SERVER-4980

t = db.jstests_coveredIndex4;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

orClause = [];
for (i = 0; i < 200; ++i) {
    if (i % 2 == 0) {
        t.save({a: i});
        orClause.push({a: i});
    } else {
        t.save({b: i});
        orClause.push({b: i});
    }
}

c = t.find({$or: orClause}, {_id: 0, a: 1});

// No odd values of a were saved, so we should not see any in the results.
while (c.hasNext()) {
    o = c.next();
    if (o.a) {
        assert.eq(0, o.a % 2, 'unexpected result: ' + tojson(o));
    }
}

c = t.find({$or: orClause}, {_id: 0, b: 1});

// No even values of b were saved, so we should not see any in the results.
while (c.hasNext()) {
    o = c.next();
    if (o.b) {
        assert.eq(1, o.b % 2, 'unexpected result: ' + tojson(o));
    }
}
