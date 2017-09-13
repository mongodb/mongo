// index5.js - test reverse direction index

function validate() {
    assert.eq(2, t.find().count());
    f = t.find().sort({a: 1});
    assert.eq(2, t.count());
    assert.eq(1, f[0].a);
    assert.eq(2, f[1].a);
    r = t.find().sort({a: -1});
    assert.eq(2, r.count());
    assert.eq(2, r[0].a);
    assert.eq(1, r[1].a);
}

t = db.index5;
t.drop();

t.save({a: 1});
t.save({a: 2});

validate();

t.ensureIndex({a: -1});
validate();
