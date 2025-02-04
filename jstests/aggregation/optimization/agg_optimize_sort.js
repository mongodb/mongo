const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany(
    [{_id: 1, a: "foo", b: "bar"}, {_id: 2, a: "feh", b: "baz"}, {_id: 3, a: "fee", b: "fum"}]));

const a1 = coll.aggregate({$match: {b: "baz"}});

const a1result = [{"_id": 2, "a": "feh", "b": "baz"}];

assert.eq(a1.toArray(), a1result, 'agg_optimize_sort.a1 failed');

const a2 = coll.aggregate({$sort: {a: 1}});

const a2result = [
    {"_id": 3, "a": "fee", "b": "fum"},
    {"_id": 2, "a": "feh", "b": "baz"},
    {"_id": 1, "a": "foo", "b": "bar"}
];

assert.eq(a2.toArray(), a2result, 'agg_optimize_sort.a2 failed');

const a3 = coll.aggregate({$match: {b: "baz"}}, {$sort: {a: 1}});

assert.eq(a3.toArray(), a1result, 'agg_optimize_sort.a3 failed');

assert.commandWorked(coll.createIndex({b: 1}, {name: "agg_optimize_sort_b"}));

const a4 = coll.aggregate({$match: {b: "baz"}});

assert.eq(a4.toArray(), a1result, 'agg_optimize_sort.a4 failed');

const a5 = coll.aggregate({$sort: {a: 1}});

assert.eq(a5.toArray(), a2result, 'agg_optimize_sort.a5 failed');

const a6 = coll.aggregate({$match: {b: "baz"}}, {$sort: {a: 1}});

assert.eq(a6.toArray(), a1result, 'agg_optimize_sort.a6 failed');

assert.commandWorked(coll.dropIndex("agg_optimize_sort_b"));

assert.commandWorked(coll.createIndex({a: 1}, {name: "agg_optimize_sort_a"}));

const a7 = coll.aggregate({$match: {b: "baz"}});

assert.eq(a7.toArray(), a1result, 'agg_optimize_sort.a7 failed');

const a8 = coll.aggregate({$sort: {a: 1}});

assert.eq(a8.toArray(), a2result, 'agg_optimize_sort.a8 failed');

const a9 = coll.aggregate({$match: {b: "baz"}}, {$sort: {a: 1}});

assert.eq(a9.toArray(), a1result, 'agg_optimize_sort.a9 failed');
