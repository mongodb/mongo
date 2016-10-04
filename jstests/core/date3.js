// Check dates before Unix epoch - SERVER-405

t = db.date3;
t.drop();

d1 = new Date(-1000);
dz = new Date(0);
d2 = new Date(1000);

t.save({x: 3, d: dz});
t.save({x: 2, d: d2});
t.save({x: 1, d: d1});

function test() {
    var list = t.find({d: {$lt: dz}});
    assert.eq(1, list.size());
    assert.eq(1, list[0].x);
    assert.eq(d1, list[0].d);
    var list = t.find({d: {$gt: dz}});
    assert.eq(1, list.size());
    assert.eq(2, list[0].x);
    var list = t.find().sort({d: 1});
    assert.eq(3, list.size());
    assert.eq(1, list[0].x);
    assert.eq(3, list[1].x);
    assert.eq(2, list[2].x);
}

test();
t.ensureIndex({d: 1});
test();
