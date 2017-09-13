// SERVER-6468 nested and dotted projections should be treated the same
c = db.c;
c.drop();

c.save({a: 'foo', b: {c: 'bar', z: 'not there'}});

function test(projection) {
    res = c.aggregate({$project: projection});
    assert.eq(res.toArray()[0], {b: {c: 'bar'}});
}

// These should all mean the same thing
test({_id: 0, 'b.c': 1});
test({_id: 0, 'b.c': '$b.c'});
test({_id: 0, b: {c: 1}});
test({_id: 0, b: {c: '$b.c'}});
