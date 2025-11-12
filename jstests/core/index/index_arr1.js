// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
// requires_getmore,
// ]

import {IndexUtils} from "jstests/libs/index_utils.js";

let t = db.index_arr1;
t.drop();

t.insert({_id: 1, a: 5, b: [{x: 1}]});
t.insert({_id: 2, a: 5, b: []});
t.insert({_id: 3, a: 5});

assert.eq(3, t.find({a: 5}).itcount(), "A1");

t.createIndex({a: 1, "b.x": 1});
assert.eq(3, t.find({a: 5}).itcount(), "A2"); // SERVER-1082
IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1, "b.x": 1}], "B1");

t.insert({_id: 4, a: 5, b: []});
t.createIndex({a: 1, "b.a": 1, "b.c": 1});
IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1, "b.x": 1}, {a: 1, "b.a": 1, "b.c": 1}], "B1");
