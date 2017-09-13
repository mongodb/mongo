// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

t = db.update_addToSet1;
t.drop();

o = {
    _id: 1,
    a: [2, 1]
};
t.insert(o);

assert.eq(o, t.findOne(), "A1");

t.update({}, {$addToSet: {a: 3}});
o.a.push(3);
assert.eq(o, t.findOne(), "A2");

t.update({}, {$addToSet: {a: 3}});
assert.eq(o, t.findOne(), "A3");

// SERVER-628
t.update({}, {$addToSet: {a: {$each: [3, 5, 6]}}});
o.a.push(5);
o.a.push(6);
assert.eq(o, t.findOne(), "B1");

t.drop();
o = {
    _id: 1,
    a: [3, 5, 6]
};
t.insert(o);
t.update({}, {$addToSet: {a: {$each: [3, 5, 6]}}});
assert.eq(o, t.findOne(), "B2");

t.drop();
t.update({_id: 1}, {$addToSet: {a: {$each: [3, 5, 6]}}}, true);
assert.eq(o, t.findOne(), "B3");
t.update({_id: 1}, {$addToSet: {a: {$each: [3, 5, 6]}}}, true);
assert.eq(o, t.findOne(), "B4");

// SERVER-630
t.drop();
t.update({_id: 2}, {$addToSet: {a: 3}}, true);
assert.eq(1, t.count(), "C1");
assert.eq({_id: 2, a: [3]}, t.findOne(), "C2");

// SERVER-3245
o = {
    _id: 1,
    a: [1, 2]
};
t.drop();
t.update({_id: 1}, {$addToSet: {a: {$each: [1, 2]}}}, true);
assert.eq(o, t.findOne(), "D1");

t.drop();
t.update({_id: 1}, {$addToSet: {a: {$each: [1, 2, 1, 2]}}}, true);
assert.eq(o, t.findOne(), "D2");

t.drop();
t.insert({_id: 1});
t.update({_id: 1}, {$addToSet: {a: {$each: [1, 2, 2, 1]}}});
assert.eq(o, t.findOne(), "D3");

t.update({_id: 1}, {$addToSet: {a: {$each: [3, 2, 2, 3, 3]}}});
o.a.push(3);
assert.eq(o, t.findOne(), "D4");

// Test that dotted and '$' prefixed field names fail.
t.drop();
o = {
    _id: 1,
    a: [1, 2]
};
assert.writeOK(t.insert(o));

assert.writeOK(t.update({}, {$addToSet: {a: {'x.$.y': 'bad'}}}));
assert.writeOK(t.update({}, {$addToSet: {a: {b: {'x.$.y': 'bad'}}}}));

assert.writeError(t.update({}, {$addToSet: {a: {"$bad": "bad"}}}));
assert.writeError(t.update({}, {$addToSet: {a: {b: {"$bad": "bad"}}}}));

assert.writeOK(t.update({}, {$addToSet: {a: {_id: {"x.y": 2}}}}));

assert.writeOK(t.update({}, {$addToSet: {a: {$each: [{'x.$.y': 'bad'}]}}}));
assert.writeOK(t.update({}, {$addToSet: {a: {$each: [{b: {'x.$.y': 'bad'}}]}}}));

assert.writeError(t.update({}, {$addToSet: {a: {$each: [{'$bad': 'bad'}]}}}));
assert.writeError(t.update({}, {$addToSet: {a: {$each: [{b: {'$bad': 'bad'}}]}}}));

// Test that nested _id fields are allowed.
t.drop();
o = {
    _id: 1,
    a: [1, 2]
};
assert.writeOK(t.insert(o));

assert.writeOK(t.update({}, {$addToSet: {a: {_id: ["foo", "bar", "baz"]}}}));
assert.writeOK(t.update({}, {$addToSet: {a: {_id: /acme.*corp/}}}));

// Test that DBRefs are allowed.
t.drop();
o = {
    _id: 1,
    a: [1, 2]
};
assert.writeOK(t.insert(o));

foo = {
    "foo": "bar"
};
assert.writeOK(t.insert(foo));
let fooDoc = t.findOne(foo);
assert.eq(fooDoc.foo, foo.foo);

let fooDocRef = {reference: new DBRef(t.getName(), fooDoc._id, t.getDB().getName())};

assert.writeOK(t.update({_id: o._id}, {$addToSet: {a: fooDocRef}}));
assert.eq(t.findOne({_id: o._id}).a[2], fooDocRef);

assert.writeOK(t.update({_id: o._id}, {$addToSet: {a: {b: fooDocRef}}}));
assert.eq(t.findOne({_id: o._id}).a[3].b, fooDocRef);
