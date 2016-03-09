
t = db.array_match1;
t.drop();

t.insert({_id: 1, a: [5, 5]});
t.insert({_id: 2, a: [6, 6]});
t.insert({_id: 3, a: [5, 5]});

function test(f, m) {
    var q = {};

    q[f] = [5, 5];
    assert.eq(2, t.find(q).itcount(), m + "1");

    q[f] = [6, 6];
    assert.eq(1, t.find(q).itcount(), m + "2");
}

test("a", "A");
t.ensureIndex({a: 1});
test("a", "B");

t.drop();

t.insert({_id: 1, a: {b: [5, 5]}});
t.insert({_id: 2, a: {b: [6, 6]}});
t.insert({_id: 3, a: {b: [5, 5]}});

test("a.b", "C");
t.ensureIndex({a: 1});
test("a.b", "D");
