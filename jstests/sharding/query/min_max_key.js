/*
 * Test writing and targeting of Max/Min key values
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({});
const coll = st.s.getDB(jsTestName())['coll'];

assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

assert.commandWorked(coll.insert({x: MaxKey}));
assert.eq(1, coll.countDocuments({}));
assert.eq(1, coll.countDocuments({x: MaxKey}));
assert.commandWorked(coll.remove({x: MaxKey}));
assert.eq(0, coll.countDocuments({}));
assert.eq(0, coll.countDocuments({x: MaxKey}));

assert.commandWorked(coll.insert({x: MinKey}));
assert.eq(1, coll.countDocuments({}));
assert.eq(1, coll.countDocuments({x: MinKey}));
assert.commandWorked(coll.remove({x: MinKey}));
assert.eq(0, coll.countDocuments({}));
assert.eq(0, coll.countDocuments({x: MinKey}));

st.stop();
