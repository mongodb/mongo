
t = db.regex_embed1;

t.drop();

t.insert({_id: 1, a: [{x: "abc"}, {x: "def"}]});
t.insert({_id: 2, a: [{x: "ab"}, {x: "de"}]});
t.insert({_id: 3, a: [{x: "ab"}, {x: "de"}, {x: "abc"}]});

function test(m) {
    assert.eq(3, t.find().itcount(), m + "1");
    assert.eq(2, t.find({"a.x": "abc"}).itcount(), m + "2");
    assert.eq(2, t.find({"a.x": /.*abc.*/}).itcount(), m + "3");

    assert.eq(1, t.find({"a.0.x": "abc"}).itcount(), m + "4");
    assert.eq(1, t.find({"a.0.x": /abc/}).itcount(), m + "5");
}

test("A");

t.ensureIndex({"a.x": 1});
test("B");
