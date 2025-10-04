// @tags: [
//   requires_getmore,
// ]

// Test covered index projection with $or clause, specifically in getMore
// SERVER-4980

let t = db.jstests_coveredIndex4;
t.drop();

t.createIndex({a: 1});
t.createIndex({b: 1});

let orClause = [];
for (let i = 0; i < 200; ++i) {
    if (i % 2 == 0) {
        t.save({a: i});
        orClause.push({a: i});
    } else {
        t.save({b: i});
        orClause.push({b: i});
    }
}

let c = t.find({$or: orClause}, {_id: 0, a: 1});

// No odd values of a were saved, so we should not see any in the results.
while (c.hasNext()) {
    let o = c.next();
    if (o.a) {
        assert.eq(0, o.a % 2, "unexpected result: " + tojson(o));
    }
}

c = t.find({$or: orClause}, {_id: 0, b: 1});

// No even values of b were saved, so we should not see any in the results.
while (c.hasNext()) {
    let o = c.next();
    if (o.b) {
        assert.eq(1, o.b % 2, "unexpected result: " + tojson(o));
    }
}
