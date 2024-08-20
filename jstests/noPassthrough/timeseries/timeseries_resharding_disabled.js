/*
 * Tests that moveCollection, reshardCollection and unshardCollection fail for timeseries
 * when FeatureFlagReshardingForTimeseries is disabled.
 *
 * @tags: [
 *   requires_timeseries
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const kDbName = jsTestName();
const kCollName = 'test';
const kFullName = kDbName + '.' + kCollName;
const testDB = st.s.getDB(kDbName);

function runTest() {
    const kPrimary = st.shard0.shardName;
    const kOther = st.shard1.shardName;

    assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: kPrimary}));
    assert.commandWorked(testDB.runCommand({create: kCollName, timeseries: {timeField: "t"}}));
    assert.commandWorked(testDB[kCollName].insert({a: 1, t: ISODate()}));

    assert.commandFailedWithCode(st.s.adminCommand({moveCollection: kFullName, toShard: kOther}),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(st.s.adminCommand({
        reshardCollection: kFullName,
        key: {b: 1},
        shardDistribution: [
            {shard: kPrimary, min: {newKey: MinKey}, max: {newKey: 0}},
            {shard: kOther, min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(st.s.adminCommand({unshardCollection: kFullName, toShard: kOther}),
                                 ErrorCodes.IllegalOperation);
}

if (!FeatureFlagUtil.isEnabled(testDB, "ReshardingForTimeseries")) {
    runTest();
}

st.stop();
