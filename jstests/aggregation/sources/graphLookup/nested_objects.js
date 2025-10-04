// In MongoDB 3.4, $graphLookup was introduced. In this file, we test the behavior of graphLookup
// when the 'connectToField' is a nested array, or when the 'connectFromField' is a nested array.

let local = db.local;
let foreign = db.foreign;

local.drop();
foreign.drop();

// 'connectFromField' is an array of objects.
let bulk = foreign.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, neighbors: [{id: i + 1}, {id: i + 2}]});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(local.insert({starting: 0}));

let res = local
    .aggregate({
        $graphLookup: {
            from: "foreign",
            startWith: "$starting",
            connectFromField: "neighbors.id",
            connectToField: "_id",
            as: "integers",
        },
    })
    .toArray()[0];
assert.eq(res.integers.length, 100);

foreign.drop();

// 'connectToField' is an array of objects.
bulk = foreign.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({previous: [{neighbor: i}, {neighbor: i - 1}], value: i + 1});
}
assert.commandWorked(bulk.execute());

res = local
    .aggregate({
        $graphLookup: {
            from: "foreign",
            startWith: "$starting",
            connectFromField: "value",
            connectToField: "previous.neighbor",
            as: "integers",
        },
    })
    .toArray()[0];
assert.eq(res.integers.length, 100);

foreign.drop();

// Both 'connectToField' and 'connectFromField' are arrays of objects.
bulk = foreign.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({
        previous: [{neighbor: i}, {neighbor: i - 1}],
        values: [{neighbor: i + 1}, {neighbor: i + 2}],
    });
}
assert.commandWorked(bulk.execute());

res = local
    .aggregate({
        $graphLookup: {
            from: "foreign",
            startWith: "$starting",
            connectFromField: "values.neighbor",
            connectToField: "previous.neighbor",
            as: "integers",
        },
    })
    .toArray()[0];
assert.eq(res.integers.length, 100);
