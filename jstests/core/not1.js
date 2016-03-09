
t = db.not1;
t.drop();

t.insert({a: 1});
t.insert({a: 2});
t.insert({});

function test(name) {
    assert.eq(3, t.find().count(), name + "A");
    assert.eq(1, t.find({a: 1}).count(), name + "B");
    assert.eq(2, t.find({a: {$ne: 1}}).count(), name + "C");  // SERVER-198
    assert.eq(1, t.find({a: {$in: [1]}}).count(), name + "D");
    assert.eq(2, t.find({a: {$nin: [1]}}).count(), name + "E");  // SERVER-198
}

test("no index");
t.ensureIndex({a: 1});
test("with index");
