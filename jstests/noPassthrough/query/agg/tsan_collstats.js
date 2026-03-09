/*
 * This test ensures there is not a heap-use-after-free error when a collStats subpipeline
 * is cloned and converted to a AggCommandRequest to be sent to the other shards.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {
        setParameter: {
            featureFlagCostBasedRanker: true,
            internalQueryCBRCEMode: "heuristicCE",
        },
    },
});

const db = st.getDB("test");
const collName = jsTestName();
const coll = db[collName];
const other = db["other"];

coll.drop();
other.drop();

st.shardColl(coll, {_id: "hashed"}, false /* split */);
st.shardColl(other, {_id: "hashed"}, false /* split */);

assert.commandWorked(
    coll.insert([
        {_id: 0, a: 1},
        {_id: 1, a: 2},
    ]),
);

assert.commandWorked(
    other.insert([
        {_id: 0, b: 1},
        {_id: 1, b: 2},
    ]),
);

assert.commandWorked(coll.createIndex({_id: "hashed"}));
assert.commandWorked(other.createIndex({_id: "hashed"}));

// In $unionWith doGetNext(), the mongod routes the subpipeline to the other shards for local
// execution. As part of that work, the subpipeline gets cloned, validated, optimized and converted
// into the AggregateCommandRequest that will be sent to the shards.

// A fuzzer BF (BF-41927) identified a heap-use-after free issue in this codepath when the unionWith
// subpipeline contained a $collStats stage. Before this ticket, $collStats had the default clone()
// stage which serializes the stage, saves it to a temporary BSON obj, constructs a new
// DocumentSourceCollStats where its spec is a shallow view into that temp BSON obj. And when clone()
// returns that temp BSON is of course freed. Thus when we serialize() the cloned pipeline to pass to
// the AggregateCommandRequest{} constructor, DocumentSourceCollStats::serialize() derefences freed
// memory.

coll.aggregate([
    {
        $unionWith: {
            coll: other.getName(),
            pipeline: [
                {
                    $collStats: {
                        count: {},
                        storageStats: {},
                        latencyStats: {histograms: true},
                    },
                },
            ],
        },
    },
]);

st.stop();
