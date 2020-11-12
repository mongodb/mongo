t = db.distinct_array1;
t.drop();

t.save({a: [1, 2, 3]});
t.save({a: [2, 3, 4]});
t.save({a: [3, 4, 5]});
t.save({a: 9});

// Without index.
res = t.distinct("a").sort();
assert.eq("1,2,3,4,5,9", res.toString());

// Array element 0 without index.
res = t.distinct("a.0").sort();
assert.eq("1,2,3", res.toString());

// Array element 1 without index.
res = t.distinct("a.1").sort();
assert.eq("2,3,4", res.toString());

// With index.
t.ensureIndex({a: 1});
res = t.distinct("a").sort();
assert.eq("1,2,3,4,5,9", res.toString());

// Array element 0 with index.
res = t.distinct("a.0").sort();
assert.eq("1,2,3", res.toString());

// Array element 1 with index.
res = t.distinct("a.1").sort();
assert.eq("2,3,4", res.toString());

t.save({a: [{b: "a"}, {b: "d"}], c: 12});
t.save({a: [{b: "b"}, {b: "d"}], c: 12});
t.save({a: [{b: "c"}, {b: "e"}], c: 12});
t.save({a: [{b: "c"}, {b: "f"}], c: 12});
t.save({a: [], c: 12});
t.save({a: {b: "z"}, c: 12});

// Without index.
res = t.distinct("a.b").sort();
assert.eq("a,b,c,d,e,f,z", res.toString());

// Array element 0 without index
res = t.distinct("a.0.b").sort();
assert.eq("a,b,c", res.toString());

// Array element 1 without index
res = t.distinct("a.1.b").sort();
assert.eq("d,e,f", res.toString());

// With index.
t.ensureIndex({"a.b": 1});
res = t.distinct("a.b");
res.sort();
// TODO SERVER-14832 The presence of an index may change results, but only if the index is not
// multikey.
// In a sharded scenario, an unlucky distribution of data will cause all the arrays to go to one
// shard, and one shard be left with only non-multikey documents, including one with {a: 9}, which
// will generate a null result.
if (res.includes(null)) {
    // The default sorting of an array is by string, so null will appear in an odd position.
    assert.eq(res, ["a", "b", "c", "d", "e", "f", null, "z"]);
} else {
    assert.eq("a,b,c,d,e,f,z", res.toString());
}

// _id as an document containing an array
t.save({_id: {a: [1, 2, 3]}});
t.save({_id: {a: [2, 3, 4]}});
t.save({_id: {a: [3, 4, 5]}});
t.save({_id: {a: 9}});

// Without index.
res = t.distinct("_id.a").sort();
assert.eq("1,2,3,4,5,9", res.toString());

// Array element 0 without index.
res = t.distinct("_id.a.0").sort();
assert.eq("1,2,3", res.toString());

// Array element 1 without index.
res = t.distinct("_id.a.1").sort();
assert.eq("2,3,4", res.toString());

// With index.
t.ensureIndex({"_id.a": 1});
res = t.distinct("_id.a").sort();
// TODO SERVER-14832: distinct() may incorrectly return null in presence of index.
assert.eq([1, 2, 3, 4, 5, 9, null], res);

// Array element 0 with index.
res = t.distinct("_id.a.0").sort();
assert.eq("1,2,3", res.toString());

// Array element 1 with index.
res = t.distinct("_id.a.1").sort();
assert.eq("2,3,4", res.toString());
