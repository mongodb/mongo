t = db.distinct_array1;
t.drop();

t.save({a: [1, 2, 3]});
t.save({a: [2, 3, 4]});
t.save({a: [3, 4, 5]});
t.save({a: 9});

// Without index.
res = t.distinct("a").sort();
assert.eq("1,2,3,4,5,9", res.toString(), "A1");

// Array element 0 without index.
res = t.distinct("a.0").sort();
assert.eq("1,2,3", res.toString(), "A2");

// Array element 1 without index.
res = t.distinct("a.1").sort();
assert.eq("2,3,4", res.toString(), "A3");

// With index.
t.ensureIndex({a: 1});
res = t.distinct("a").sort();
assert.eq("1,2,3,4,5,9", res.toString(), "A4");

// Array element 0 with index.
res = t.distinct("a.0").sort();
assert.eq("1,2,3", res.toString(), "A5");

// Array element 1 with index.
res = t.distinct("a.1").sort();
assert.eq("2,3,4", res.toString(), "A6");

// t.drop();

t.save({a: [{b: "a"}, {b: "d"}], c: 12});
t.save({a: [{b: "b"}, {b: "d"}], c: 12});
t.save({a: [{b: "c"}, {b: "e"}], c: 12});
t.save({a: [{b: "c"}, {b: "f"}], c: 12});
t.save({a: [], c: 12});
t.save({a: {b: "z"}, c: 12});

// Without index.
res = t.distinct("a.b").sort();
assert.eq("a,b,c,d,e,f,z", res.toString(), "B1");

// Array element 0 without index
res = t.distinct("a.0.b").sort();
assert.eq("a,b,c", res.toString(), "B2");

// Array element 1 without index
res = t.distinct("a.1.b").sort();
assert.eq("d,e,f", res.toString(), "B3");

// With index.
t.ensureIndex({"a.b": 1});
res = t.distinct("a.b");
res.sort();
assert.eq("a,b,c,d,e,f,z", res.toString(), "B4");

// _id as an document containing an array
t.save({_id: {a: [1, 2, 3]}});
t.save({_id: {a: [2, 3, 4]}});
t.save({_id: {a: [3, 4, 5]}});
t.save({_id: {a: 9}});

// Without index.
res = t.distinct("_id.a").sort();
assert.eq("1,2,3,4,5,9", res.toString(), "C1");

// Array element 0 without index.
res = t.distinct("_id.a.0").sort();
assert.eq("1,2,3", res.toString(), "C2");

// Array element 1 without index.
res = t.distinct("_id.a.1").sort();
assert.eq("2,3,4", res.toString(), "C3");

// With index.
t.ensureIndex({"_id.a": 1});
res = t.distinct("_id.a").sort();
assert.eq("1,2,3,4,5,9", res.toString(), "C4");

// Array element 0 with index.
res = t.distinct("_id.a.0").sort();
assert.eq("1,2,3", res.toString(), "C5");

// Array element 1 with index.
res = t.distinct("_id.a.1").sort();
assert.eq("2,3,4", res.toString(), "C6");
