// Test that we can sort on fields, produces by unwind

const coll = db.agg_unwind_sort;
coll.drop();
assert.commandWorked(coll.insertOne({a: [3, 4, 5]}));
assert.commandWorked(coll.insertOne({a: [1, 2]}));

let result = coll.aggregate([{$unwind: "$a"}, {$sort: {a: 1}}]).toArray();
assert.eq([1, 2, 3, 4, 5], result.map(function(z) {
    return z.a;
}));
result = coll.aggregate([{$unwind: "$a"}, {$sort: {a: -1}}]).toArray();
assert.eq([5, 4, 3, 2, 1], result.map(function(z) {
    return z.a;
}));
result =
    coll.aggregate([{$unwind: {path: "$a", includeArrayIndex: "i"}}, {$sort: {i: 1}}]).toArray();
assert.eq([NumberLong(0), NumberLong(0), NumberLong(1), NumberLong(1), NumberLong(2)],
          result.map(function(z) {
              return z.i;
          }));
result =
    coll.aggregate([{$unwind: {path: "$a", includeArrayIndex: "i"}}, {$sort: {i: -1}}]).toArray();
assert.eq([NumberLong(2), NumberLong(1), NumberLong(1), NumberLong(0), NumberLong(0)],
          result.map(function(z) {
              return z.i;
          }));
