/*
 * In the case of an always false predicate the shards will return an EOF plan. Additionally this
 * tests that mongos can deal with EOF plans coming from the shard.
 *
 * @tags: [
 *   # EOF plans are only with
 *   requires_fcv_73,
 * ]
 */

import {getWinningPlanFromExplain} from 'jstests/libs/query/analyze_plan.js';
import {ShardingTest} from "jstests/libs/shardingtest.js";
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

st.s.adminCommand({enableSharding: "test"});
const db = st.getDB("test");
const coll = db[jsTestName()];
st.shardColl(coll, {array: 1});

assert.commandWorked(coll.insertMany([...Array(10).keys()].map(i => {
    return {_id: i, a: 1};
})));

{
    assert.eq(coll.aggregate([
                      {"$match": {"array": {"$all": []}}},
                      {"$sort": {"a": -1, "_id": 1}},
                      {"$limit": 6}
                  ])
                  .itcount(),
              0);
    const explain = coll.explain().aggregate([{"$match": {"array": {"$all": []}}}]);
    // check that we received an EOF plan
    assert.eq(getWinningPlanFromExplain(explain).stage, "EOF");
    st.stop();
}
