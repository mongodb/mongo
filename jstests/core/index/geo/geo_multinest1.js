// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
//   requires_getmore,
// ]

// Test distance queries with interleaved distances

let t = db.multinest;
t.drop();

t.insert({
    zip: "10001",
    data: [
        {loc: [10, 10], type: "home"},
        {loc: [29, 29], type: "work"},
    ],
});
t.insert({
    zip: "10002",
    data: [
        {loc: [20, 20], type: "home"},
        {loc: [39, 39], type: "work"},
    ],
});
let res = t.insert({
    zip: "10003",
    data: [
        {loc: [30, 30], type: "home"},
        {loc: [49, 49], type: "work"},
    ],
});
assert.commandWorked(res);

assert.commandWorked(t.createIndex({"data.loc": "2d", zip: 1}));
assert.eq(2, t.getIndexKeys().length);

res = t.insert({
    zip: "10004",
    data: [
        {loc: [40, 40], type: "home"},
        {loc: [59, 59], type: "work"},
    ],
});
assert.commandWorked(res);

// test normal access

let result = t.find({"data.loc": {$near: [0, 0]}}).toArray();

printjson(result);

assert.eq(4, result.length);

let order = [1, 2, 3, 4];

for (let i = 0; i < result.length; i++) {
    assert.eq("1000" + order[i], result[i].zip);
}
