// SERVER-8088: test $unwind with a scalar

t = db.agg_unwind;
t.drop();

t.insert({_id: 1});
t.insert({_id: 2, x: null});
t.insert({_id: 3, x: []});
t.insert({_id: 4, x: [1, 2]});
t.insert({_id: 5, x: [3]});
t.insert({_id: 6, x: 4});

var res = t.aggregate([{$unwind: "$x"}, {$sort: {_id: 1}}]).toArray();
assert.eq(4, res.length);
assert.eq([1, 2, 3, 4], res.map(function(z) {
    return z.x;
}));

// Test unwinding an empty path
coll = db.agg_unwind_empty_path;
coll.drop();
coll.insert({_id: 1});
coll.insert({_id: 2, "": [1, 2]});
coll.insert({_id: 3, "": {"": [3, 4, 5]}});

assert.throwsWithCode(() => coll.aggregate([{$unwind: {path: "$"}}]).toArray(), 40352);
assert.throwsWithCode(() => coll.aggregate([{$unwind: {path: "$."}}]).toArray(), 40353);

// Test includeArrayIndex
coll = db.agg_unwind_with_index;
coll.drop();
coll.insert({_id: 1});
coll.insert({_id: 2, array: [1, 2]});
coll.insert({_id: 3, obj: {array: [3, 4, 5]}});
coll.insert({_id: 4, obj: {subObj: {array: [6, 7, 8, 9]}}});

assert.eq(
    coll.aggregate([{$unwind: {path: "$array", includeArrayIndex: "idx"}}]).toArray(),
    [{"_id": 2, "array": 1, "idx": NumberLong(0)}, {"_id": 2, "array": 2, "idx": NumberLong(1)}]);

assert.eq(coll.aggregate([{$unwind: {path: "$array", includeArrayIndex: "array"}}]).toArray(),
          [{"_id": 2, "array": NumberLong(0)}, {"_id": 2, "array": NumberLong(1)}]);

assert.eq(coll.aggregate([{$unwind: {path: "$obj.array", includeArrayIndex: "idx"}}]).toArray(), [
    {"_id": 3, "obj": {"array": 3}, "idx": NumberLong(0)},
    {"_id": 3, "obj": {"array": 4}, "idx": NumberLong(1)},
    {"_id": 3, "obj": {"array": 5}, "idx": NumberLong(2)}
]);

assert.eq(coll.aggregate([{$unwind: {path: "$obj.array", includeArrayIndex: "obj"}}]).toArray(), [
    {"_id": 3, "obj": NumberLong(0)},
    {"_id": 3, "obj": NumberLong(1)},
    {"_id": 3, "obj": NumberLong(2)}
]);

assert.eq(
    coll.aggregate([{$unwind: {path: "$obj.array", includeArrayIndex: "obj.array"}}]).toArray(), [
        {"_id": 3, "obj": {"array": NumberLong(0)}},
        {"_id": 3, "obj": {"array": NumberLong(1)}},
        {"_id": 3, "obj": {"array": NumberLong(2)}}
    ]);

assert.eq(coll.aggregate([{$unwind: {path: "$obj.subObj.array", includeArrayIndex: "obj.subObj"}}])
              .toArray(),
          [
              {"_id": 4, "obj": {"subObj": NumberLong(0)}},
              {"_id": 4, "obj": {"subObj": NumberLong(1)}},
              {"_id": 4, "obj": {"subObj": NumberLong(2)}},
              {"_id": 4, "obj": {"subObj": NumberLong(3)}}
          ]);
