// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
// ]
import {queryIDS} from "jstests/libs/fts.js";

const coll = db.text1;
coll.drop();

assert.commandWorked(coll.createIndex({x: "text"}, {name: "x_text"}));

assert.eq([], queryIDS(coll, "az"), "A0");

assert.commandWorked(coll.insert({_id: 1, x: "az b c"}));
assert.commandWorked(coll.insert({_id: 2, x: "az b"}));
assert.commandWorked(coll.insert({_id: 3, x: "b c"}));
assert.commandWorked(coll.insert({_id: 4, x: "b c d"}));

assert.eq([1, 2, 3, 4], queryIDS(coll, "c az").sort(), "A1");
assert.eq([4], queryIDS(coll, "d"), "A2");

const index = coll.getIndexes().find((index) => index.name === "x_text");
assert.neq(index, undefined);
assert.gte(index.textIndexVersion, 1, tojson(index));
