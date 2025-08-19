/**
 * Test that a $unionWith with recursively nested $lookup pipelines that target other shards behave
 * correctly when the query requires multiple getMore calls to be completed.
 *
 * @tags: [
 *   requires_sharding,
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

const name = jsTestName();

const st = new ShardingTest({shards: 2, mongos: 1});

function runTest(conn) {
    const db = conn.getDB(name);
    const shardedCollName = `${jsTestName()}_sharded`;
    const unshardedCollName = `${jsTestName()}_unsharded`;

    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    assert.commandWorked(
        db.adminCommand({shardCollection: `${db.getName()}.${shardedCollName}`, key: {_id: 1}}));
    assert.commandWorked(
        db.adminCommand({split: `${db.getName()}.${shardedCollName}`, middle: {_id: 1}}));

    const docs = Array.from(
        {length: 10}, (_, i) => ({_id: 0.2 * i, key: i % 5, arr: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}));
    assert.commandWorked(db[shardedCollName].insert(docs));
    assert.commandWorked(db[unshardedCollName].insert(docs));

    let result = db[unshardedCollName]
        .aggregate(
            [
                {$match: {_id: 0}},
                {$unwind: "$arr"},
                {
                    $unionWith: {
                        coll: shardedCollName,
                        pipeline: [{
                            $lookup: {
                                from: unshardedCollName,
                                "let": {localKey: "$key"},
                                pipeline: [
                                    {$match: {$expr: {$eq: ["$key", "$$localKey"]}}},
                                    {
                                        $lookup: {
                                            from: unshardedCollName,
                                            "let": {localKey: "$key"},
                                            pipeline: [
                                                {$match: {$expr: {$eq: ["$key", "$$localKey"]}}},
                                                {$addFields: {computed: {$add: ["$$localKey", 1]}}},
                                            ],
                                            as: "nestedMatches",
                                        }
                                    },
                                ],
                                as: "matches",
                            },
                        }]
                    }
                }
            ],
            // Note: a small batchSize is required here so that we have multiple getMore calls.
            {batchSize: 2})
        .toArray();

    assert.eq(20, result.length);
}

runTest(st.s0);

st.stop();
}());
