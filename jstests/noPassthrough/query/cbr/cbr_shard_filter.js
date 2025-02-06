/*
 * Test that a shard filter will cause CBR to fallback to multiplanning.
 */

import {getAllPlans} from "jstests/libs/query/analyze_plan.js";
import {assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st =
    new ShardingTest({shards: 2, mongos: 1, rs: {setParameter: {planRankerMode: 'heuristicCE'}}});

const db = st.getDB("test");
const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

const explain = coll.find({a: 1, b: 1}).explain();
getAllPlans(explain).forEach(assertPlanNotCosted);

st.stop();
