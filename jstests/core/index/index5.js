// index5.js - test reverse direction index
//
// @tags: [requires_fastcount]

function validate() {
    assert.eq(2, t.find().count());
    let f = t.find().sort({a: 1});
    assert.eq(2, t.count());
    assert.eq(1, f[0].a);
    assert.eq(2, f[1].a);
    let r = t.find().sort({a: -1});
    assert.eq(2, r.count());
    assert.eq(2, r[0].a);
    assert.eq(1, r[1].a);
}

let t = db.index5;
t.drop();

t.save({a: 1});
t.save({a: 2});

validate();

t.createIndex({a: -1});
validate();
