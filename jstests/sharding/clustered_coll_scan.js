/*
 * Testing if mongos can deal with clustered collections and the related clustered IDX scan bounds
 * (SERVER-83119)
 */

import {isClusteredIxscan} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

st.s.adminCommand({enableSharding: "test"});

const db = st.getDB("test");
// Create the collection as a clustered collection.
const coll = assertDropAndRecreateCollection(
    db, jsTestName(), {clusteredIndex: {key: {_id: 1}, unique: true}})
st.shardColl(coll, {a: 1});
// First of all check that we can execute the query.
assert.commandWorked(coll.insertMany([...Array(10).keys()].map(i => {
    return {_id: i, a: i};
})));

{
    var explain = coll.find({_id: 2}).explain();
    // Make sure that we have a clusteredIDXScan in the plan.

    assert(isClusteredIxscan(db, explain));
    assert.commandWorked(
        st.getPrimaryShard("test").adminCommand({setParameter: 1, notablescan: 1}));
    // Do the same thing only with notablescan enabled.
    explain = coll.find({_id: 2}).explain();
    assert(isClusteredIxscan(db, explain));
    // Sanity count check.
    assert.eq(1, coll.find({_id: 2}).itcount());
}
// Test the same with aggregate.
{
    var explain = coll.explain().aggregate([{$match: {_id: 22}}]);
    assert(isClusteredIxscan(db, explain));
}

st.stop();
