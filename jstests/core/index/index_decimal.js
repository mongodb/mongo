// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
//   requires_getmore,
// ]

// Test indexing of decimal numbers
// Include helpers for analyzing explain output.
import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let t = db.decimal_indexing;
t.drop();

// Create doubles and NumberDecimals. The double 0.1 is actually 0.10000000000000000555
// and the double 0.3 is actually 0.2999999999999999888, so we can check ordering.
assert.commandWorked(t.insert({x: 0.1, y: NumberDecimal("0.3000")}));
assert.commandWorked(t.insert({x: 0.1}));
assert.commandWorked(t.insert({y: 0.3}));

// Create an index on existing numbers.
assert.commandWorked(t.createIndex({x: 1}));
assert.commandWorked(t.createIndex({y: -1}));

// Insert some more items after index creation. Use _id for decimal.
assert.commandWorked(t.insert({x: NumberDecimal("0.10")}));
assert.commandWorked(t.insert({_id: NumberDecimal("0E3")}));
assert.writeError(t.insert({_id: -0.0}));

// Check that we return exactly the right document, use an index to do so, and that the
// result of the covered query has the right number of trailing zeros.
let qres = t.find({x: NumberDecimal("0.1")}, {_id: 0, x: 1}).toArray();
let qplan = t.find({x: NumberDecimal("0.1")}, {_id: 0, x: 1}).explain();
assert.neq(
    tojson(NumberDecimal("0.1")),
    tojson(NumberDecimal("0.10")),
    "trailing zeros are significant for exact equality",
);
assert.eq(qres, [{x: NumberDecimal("0.10")}], "query for x equal to decimal 0.10 returns wrong value");
assert(isIndexOnly(db, qplan.queryPlanner.winningPlan), "query on decimal should be covered: " + tojson(qplan));

// Check that queries for exact floating point numbers don't return nearby decimals.
assert.eq(
    t.find({x: 0.1}, {_id: 0}).sort({x: 1, y: 1}).toArray(),
    [{x: 0.1}, {x: 0.1, y: NumberDecimal("0.3000")}],
    "wrong result for querying {x: 0.1}",
);
assert.eq(
    t.find({x: {$lt: 0.1}}, {_id: 0}).toArray(),
    [{x: NumberDecimal("0.10")}],
    "querying for decimal less than double 0.1 should return decimal 0.10",
);
assert.eq(
    t.find({y: {$lt: NumberDecimal("0.3")}}, {y: 1, _id: 0}).toArray(),
    [{y: 0.3}],
    "querying for double less than decimal 0.3 should return double 0.3",
);
assert.eq(
    t.find({_id: 0}, {_id: 1}).toArray(),
    [{_id: NumberDecimal("0E3")}],
    "querying for zero does not return the correct decimal",
);
