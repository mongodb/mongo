// Be sure that an attempt to run a $lookup on a sharded collection fails when the flag for
// sharded $lookup is disabled.
(function() {
"use strict";

// TODO SERVER-60018: When the feature flag is removed, remove this test file.

const st = new ShardingTest({shards: 2, mongos: 1});
const testName = "lookup_sharded";

const mongosDB = st.s0.getDB(testName);
assert.commandWorked(mongosDB.dropDatabase());
const sourceColl = st.getDB(testName).lookUp;
const fromColl = st.getDB(testName).from;

const getShardedLookupParam = st.s.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;

// Only run the tests if the flag is disabled.
if (isShardedLookupEnabled) {
    st.stop();
    return;
}

// Re shard the foreign collection on _id.
st.shardColl(mongosDB.from, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assert.throwsWithCode(
    () =>
        sourceColl
            .aggregate([{
                $lookup: {localField: "a", foreignField: "b", from: fromColl.getName(), as: "same"}
            }])
            .itcount(),
    28769);
assert.throwsWithCode(
    () => sourceColl
              .aggregate(
                  [{
                      $lookup:
                          {localField: "a", foreignField: "b", from: fromColl.getName(), as: "same"}
                  }],
                  {allowDiskUse: true})
              .itcount(),
    28769);
assert.throwsWithCode(() => sourceColl
                                .aggregate(
                                    [
                                        {$_internalSplitPipeline: {mergeType: "anyShard"}},
                                        {
                                        $lookup: {
                                            localField: "a",
                                            foreignField: "b",
                                            from: fromColl.getName(),
                                            as: "same"
                                        }
                                        }
                                    ],
                                    {allowDiskUse: true})
                                .itcount(), 28769);
assert.throwsWithCode(
    () => sourceColl
            .aggregate(
                [{$facet: {
                    a: {
                    $lookup:
                        {localField: "a", foreignField: "b", from: fromColl.getName(), as: "same"}
                    }
                }}],
                {allowDiskUse: true})
            .itcount(), 40170);

st.stop();
}());