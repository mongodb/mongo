/**
 * This test verifies that the optimizer fast path is not used for queries against sharded
 * collections.
 * @tags: [
 *  requires_fcv_73,
 * ]
 */
import {isBonsaiFastPathPlan} from "jstests/libs/analyze_plan.js";

const bonsaiSettings = {
    internalQueryFrameworkControl: "tryBonsai",
    featureFlagCommonQueryFramework: true,
};

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {
            setParameter: {
                ...bonsaiSettings,
                "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
            }
        },
        mongosOptions: {setParameter: {...bonsaiSettings}},
    }
});

const db = st.getDB("test");

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([...Array(100).keys()].map(i => {
    return {_id: i, a: 1};
})));

st.shardColl(coll.getName(), {_id: 1}, {_id: 50}, {_id: 51});

{
    // Empty find on a sharded collection should not use fast path.
    const explain = coll.explain().find().finish();
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Pipeline with empty match on a sharded collection should not use fast path.
    const explain = coll.explain().aggregate([{$match: {}}]);
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Empty aggregate on a sharded collection should not use fast path.
    const explain = coll.explain().aggregate([]);
    assert(!isBonsaiFastPathPlan(db, explain));
}

st.stop();
