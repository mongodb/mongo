// Tests for $elemMatch projections and $ positional operator projection.
t = db.SERVER828Test;
t.drop();

date1 = new Date();

// Insert various styles of arrays
for (i = 0; i < 100; i++) {
    t.insert({group: 1, x: [1, 2, 3, 4, 5]});
    t.insert({group: 2, x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}]});
    t.insert({
        group: 3,
        x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
        y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
    });
    t.insert({group: 3, x: [{a: 1, b: 3}, {a: -6, c: 3}]});
    t.insert({group: 4, x: [{a: 1, b: 4}, {a: -6, c: 3}]});
    t.insert({group: 5, x: [new Date(), 5, 10, 'string', new ObjectId(), 123.456]});
    t.insert({
        group: 6,
        x: [{a: 'string', b: date1}, {a: new ObjectId(), b: 1.2345}, {a: 'string2', b: date1}]
    });
    t.insert({group: 7, x: [{y: [1, 2, 3, 4]}]});
    t.insert({group: 8, x: [{y: [{a: 1, b: 2}, {a: 3, b: 4}]}]});
    t.insert({group: 9, x: [{y: [{a: 1, b: 2}, {a: 3, b: 4}]}, {z: [{a: 1, b: 2}, {a: 3, b: 4}]}]});
    t.insert({group: 10, x: [{a: 1, b: 2}, {a: 3, b: 4}], y: [{c: 1, d: 2}, {c: 3, d: 4}]});
    t.insert({group: 10, x: [{a: 1, b: 2}, {a: 3, b: 4}], y: [{c: 1, d: 2}, {c: 3, d: 4}]});
    t.insert({
        group: 11,
        x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
        covered: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
    });
    t.insert({group: 12, x: {y: [{a: 1, b: 1}, {a: 1, b: 2}]}});
    t.insert({group: 13, x: [{a: 1, b: 1}, {a: 1, b: 2}]});
    t.insert({group: 13, x: [{a: 1, b: 2}, {a: 1, b: 1}]});
}
t.ensureIndex({
    group: 1,
    'y.d': 1
});  // for regular index test (not sure if this is really adding anything useful)
t.ensureIndex({group: 1, covered: 1});  // for covered index test

//
// SERVER-828:  Positional operator ($) projection tests
//
assert.eq(1,
          t.find({group: 3, 'x.a': 2}, {'x.$': 1}).toArray()[0].x.length,
          "single object match (array length match)");

assert.eq(
    2, t.find({group: 3, 'x.a': 1}, {'x.$': 1}).toArray()[0].x[0].b, "single object match first");

assert.eq(undefined,
          t.find({group: 3, 'x.a': 2}, {_id: 0, 'x.$': 1}).toArray()[0]._id,
          "single object match with filtered _id");

assert.eq(1,
          t.find({group: 3, 'x.a': 2}, {'x.$': 1}).sort({_id: 1}).toArray()[0].x.length,
          "sorted single object match with filtered _id (array length match)");

assert.eq(
    1,
    t.find({'group': 2, 'x': {'$elemMatch': {'a': 1, 'b': 2}}}, {'x.$': 1}).toArray()[0].x.length,
    "single object match with elemMatch");

assert.eq(1,
          t.find({'group': 2, 'x': {'$elemMatch': {'a': 1, 'b': 2}}}, {'x.$': {'$slice': 1}})
              .toArray()[0]
              .x.length,
          "single object match with elemMatch and positive slice");

assert.eq(1,
          t.find({'group': 2, 'x': {'$elemMatch': {'a': 1, 'b': 2}}}, {'x.$': {'$slice': -1}})
              .toArray()[0]
              .x.length,
          "single object match with elemMatch and negative slice");

assert.eq(1,
          t.find({'group': 12, 'x.y.a': 1}, {'x.y.$': 1}).toArray()[0].x.y.length,
          "single object match with two level dot notation");

assert.eq(1,
          t.find({group: 3, 'x.a': 2}, {'x.$': 1}).sort({x: 1}).toArray()[0].x.length,
          "sorted object match (array length match)");

assert.eq({aa: 1, dd: 5},
          t.find({group: 3, 'y.dd': 5}, {'y.$': 1}).toArray()[0].y[0],
          "single object match (value match)");

assert.throws(function() {
    t.find({group: 3, 'x.a': 2}, {'y.$': 1}).toArray();
}, [], "throw on invalid projection (field mismatch)");

assert.throws(function() {
    t.find({group: 3, 'x.a': 2}, {'y.$': 1}).sort({x: 1}).toArray();
}, [], "throw on invalid sorted projection (field mismatch)");

assert.throws(function() {
    x;
    t.find({group: 3, 'x.a': 2}, {'x.$': 1, group: 0}).sort({x: 1}).toArray();
}, [], "throw on invalid projection combination (include and exclude)");

assert.throws(function() {
    t.find({group: 3, 'x.a': 1, 'y.aa': 1}, {'x.$': 1, 'y.$': 1}).toArray();
}, [], "throw on multiple projections");

assert.throws(function() {
    t.find({group: 3}, {'g.$': 1}).toArray();
}, [], "throw on invalid projection (non-array field)");

assert.eq({aa: 1, dd: 5},
          t.find({group: 11, 'covered.dd': 5}, {'covered.$': 1}).toArray()[0].covered[0],
          "single object match (covered index)");

assert.eq({aa: 1, dd: 5},
          t.find({group: 11, 'covered.dd': 5}, {'covered.$': 1})
              .sort({covered: 1})
              .toArray()[0]
              .covered[0],
          "single object match (sorted covered index)");

assert.eq(1,
          t.find({group: 10, 'y.d': 4}, {'y.$': 1}).toArray()[0].y.length,
          "single object match (regular index");

if (false) {
    assert.eq(2,  // SERVER-1013: allow multiple positional operators
              t.find({group: 3, 'y.bb': 2, 'x.d': 5}, {'y.$': 1, 'x.$': 1}).toArray()[0].y[0].bb,
              "multi match, multi proj 1");

    assert.eq(5,  // SSERVER-1013: allow multiple positional operators
              t.find({group: 3, 'y.bb': 2, 'x.d': 5}, {'y.$': 1, 'x.$': 1}).toArray()[0].x[0].d,
              "multi match, multi proj 2");

    assert.eq(2,  // SERVER-1243: allow multiple results from same matcher
              t.find({group: 2, x: {$elemMatchAll: {a: 1}}}, {'x.$': 1}).toArray()[0].x.length,
              "multi element match, single proj");

    assert.eq(2,  // SERVER-1013: multiple array matches with one prositional operator
              t.find({group: 3, 'y.bb': 2, 'x.d': 5}, {'y.$': 1}).toArray()[0].y[0].bb,
              "multi match, single proj 1");

    assert.eq(2,  // SERVER-1013: multiple array matches with one positional operator
              t.find({group: 3, 'y.cc': 3, 'x.b': 2}, {'x.$': 1}).toArray()[0].x[0].b,
              "multi match, single proj 2");
}

//
// SERVER-2238:  $elemMatch projections
//
assert.eq(
    -6, t.find({group: 4}, {x: {$elemMatch: {a: -6}}}).toArray()[0].x[0].a, "single object match");

assert.eq(1,
          t.find({group: 4}, {x: {$elemMatch: {a: -6}}}).toArray()[0].x.length,
          "filters non-matching array elements");

assert.eq(1,
          t.find({group: 4}, {x: {$elemMatch: {a: -6, c: 3}}}).toArray()[0].x.length,
          "filters non-matching array elements with multiple elemMatch criteria");

assert.eq(
    1,
    t.find({group: 13}, {'x': {'$elemMatch': {a: {$gt: 0, $lt: 2}}}}).toArray()[0].x.length,
    "filters non-matching array elements with multiple criteria for a single element in the array");

assert.eq(3,
          t.find({group: 4}, {x: {$elemMatch: {a: {$lt: 1}}}}).toArray()[0].x[0].c,
          "object operator match");

assert.eq([4],
          t.find({group: 1}, {x: {$elemMatch: {$in: [100, 4, -123]}}}).toArray()[0].x,
          "$in number match");

assert.eq([{a: 1, b: 2}],
          t.find({group: 2}, {x: {$elemMatch: {a: {$in: [1]}}}}).toArray()[0].x,
          "$in number match");

assert.eq([1],
          t.find({group: 1}, {x: {$elemMatch: {$nin: [4, 5, 6]}}}).toArray()[0].x,
          "$nin number match");

// but this may become a user assertion, since a single element of an array can't match more than
// one value
assert.eq(
    [1], t.find({group: 1}, {x: {$elemMatch: {$all: [1]}}}).toArray()[0].x, "$in number match");

assert.eq([{a: 'string', b: date1}],
          t.find({group: 6}, {x: {$elemMatch: {a: 'string'}}}).toArray()[0].x,
          "mixed object match on string eq");

assert.eq([{a: 'string2', b: date1}],
          t.find({group: 6}, {x: {$elemMatch: {a: /ring2/}}}).toArray()[0].x,
          "mixed object match on regexp");

assert.eq([{a: 'string', b: date1}],
          t.find({group: 6}, {x: {$elemMatch: {a: {$type: 2}}}}).toArray()[0].x,
          "mixed object match on type");

assert.eq([{a: 2, c: 3}],
          t.find({group: 2}, {x: {$elemMatch: {a: {$ne: 1}}}}).toArray()[0].x,
          "mixed object match on ne");

assert.eq([{a: 1, d: 5}],
          t.find({group: 3}, {x: {$elemMatch: {d: {$exists: true}}}}).toArray()[0].x,
          "mixed object match on exists");

assert.eq([{a: 2, c: 3}],
          t.find({group: 3}, {x: {$elemMatch: {a: {$mod: [2, 0]}}}}).toArray()[0].x,
          "mixed object match on mod");

assert.eq(
    {"x": [{"a": 1, "b": 2}], "y": [{"c": 3, "d": 4}]},
    t.find({group: 10}, {_id: 0, x: {$elemMatch: {a: 1}}, y: {$elemMatch: {c: 3}}}).toArray()[0],
    "multiple $elemMatch on unique fields 1");

assert.eq({"x": [{"y": [{"a": 1, "b": 2}, {"a": 3, "b": 4}]}]},
          t.find({group: 8}, {_id: 0, x: {$elemMatch: {y: {$elemMatch: {a: 3}}}}}).toArray()[0],
          "nested $elemMatch");

assert.throws(function() {
    t.find({group: 3, 'x.a': 1}, {'x.$': 1, y: {$elemMatch: {aa: 1}}}).toArray();
}, [], "throw on positional operator with $elemMatch");

if (false) {
    assert.eq(2,  // SERVER-1243: handle multiple $elemMatch results
              t.find({group: 4}, {x: {$elemMatchAll: {a: {$lte: 2}}}}).toArray()[0].x.length,
              "multi object match");

    assert.eq(3,  // SERVER-1243: handle multiple $elemMatch results
              t.find({group: 1}, {x: {$elemMatchAll: {$in: [1, 2, 3]}}}).toArray()[0].x.length,
              "$in number match");

    assert.eq(1,  // SERVER-1243: handle multiple $elemMatch results
              t.find({group: 5}, {x: {$elemMatchAll: {$ne: 5}}}).toArray()[0].x.length,
              "single mixed type match 1");

    assert.eq(1,  // SERVER-831: handle nested arrays
              t.find({group: 9}, {'x.y': {$elemMatch: {a: 1}}}).toArray()[0].x.length,
              "single dotted match");
}

//
// Batch/getMore tests
//
// test positional operator across multiple batches
a = t.find({group: 3, 'x.b': 2}, {'x.$': 1}).batchSize(1);
while (a.hasNext()) {
    assert.eq(2, a.next().x[0].b, "positional getMore test");
}

// test $elemMatch operator across multiple batches
a = t.find({group: 3}, {x: {$elemMatch: {a: 1}}}).batchSize(1);
while (a.hasNext()) {
    assert.eq(1, a.next().x[0].a, "positional getMore test");
}
