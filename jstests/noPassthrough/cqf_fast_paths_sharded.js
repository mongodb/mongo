/**
 * This test verifies that the optimizer fast path is not used for queries against sharded
 * collections.
 */
import {planHasStage} from "jstests/libs/analyze_plan.js";

const bonsaiSettings = {
    internalQueryFrameworkControl: "tryBonsai",
    featureFlagCommonQueryFramework: true,
    // TODO SERVER-80582: Uncomment this setting. Some sharding code calls empty find on an
    // unsharded collection internally, which causes this test to fail as that fast path is not
    // implemented yet. internalCascadesOptimizerDisableFastPath: false,
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

function assertNotUsingFastPath(explainCmd) {
    const explain = assert.commandWorked(explainCmd);
    assert(!planHasStage(db, explain, "FASTPATH"));
}

{
    // Empty find on a sharded collection should not use fast path.
    const explain = coll.explain().find().finish();
    assertNotUsingFastPath(explain);
}
{
    // Pipeline with empty match on a sharded collection should not use fast path.
    const explain = coll.explain().aggregate([{$match: {}}]);
    assertNotUsingFastPath(explain);
}
{
    // Empty aggregate on a sharded collection should not use fast path.
    const explain = coll.explain().aggregate([]);
    assertNotUsingFastPath(explain);
}

st.stop();
