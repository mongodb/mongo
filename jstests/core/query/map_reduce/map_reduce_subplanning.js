// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   requires_getmore,
//   requires_non_retryable_writes,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   # Uses mapReduce command.
//   requires_scripting,
// ]

import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.map_reduce_subplanning;
coll.drop();
db.getCollection("mrOutput").drop();

coll.createIndex({a: 1, c: 1});
coll.createIndex({b: 1, c: 1});
// Use descending indexes to avoid index deduplication.
coll.createIndex({a: -1});
coll.createIndex({b: -1});

assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({b: 3}));
assert.commandWorked(coll.insert({b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 3}));

assert.commandWorked(coll.mapReduce(
    function() {
        if (!this.hasOwnProperty('a')) {
            emit('a', 0);
        } else {
            emit('a', this.a);
        }
    },
    function(key, vals) {
        return vals.reduce((a, b) => a + b, 0);
    },
    {out: {merge: "mrOutput"}, query: {$or: [{a: 2}, {b: 3}]}}));

assert(resultsEq([{"_id": "a", "value": 4}], db.getCollection("mrOutput").find().toArray()),
       db.getCollection("mrOutput").find().toArray());
