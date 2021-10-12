/**
 * Confirms that the limit on number of aggregragation pipeline stages is respected.
 * @tags: [requires_fcv_49]
 */
(function() {
"use strict";

function testLimits(testDB, lengthLimit) {
    const maxLength = lengthLimit;
    const tooLarge = lengthLimit + 1;

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(maxLength).fill({$project: {_id: 1}})
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(tooLarge).fill({$project: {_id: 1}})
    }),
                                 ErrorCodes.FailedToParse);
    testDB.setLogLevel(1);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{
            $lookup:
                {from: "test", as: "as", pipeline: new Array(maxLength).fill({$project: {_id: 1}})}
        }]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{
            $lookup:
                {from: "test", as: "as", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})}
        }]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [
            {$unionWith: {coll: "test", pipeline: new Array(maxLength).fill({$project: {_id: 1}})}}
        ]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline:
            [{$unionWith: {coll: "test", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})}}]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{$facet: {foo: new Array(maxLength).fill({$project: {_id: 1}})}}]
    }));
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: [{$facet: {foo: new Array(tooLarge).fill({$project: {_id: 1}}), bar: []}}]
    }),
                                 ErrorCodes.FailedToParse);

    assert.commandWorked(testDB.runCommand(
        {update: "test", updates: [{q: {}, u: new Array(maxLength).fill({$project: {_id: 1}})}]}));
    assert.commandFailedWithCode(testDB.runCommand({
        update: "test",
        updates: [{q: {}, u: new Array(tooLarge).fill({$project: {_id: 1}})}]
    }),
                                 ErrorCodes.FailedToParse);

    const collname = "test";

    [  // Long pipelines with many unions.
        new Array(maxLength).fill({$unionWith: collname}),
        // maxLength * 2 total unions.
        new Array(maxLength).fill(
            {$unionWith: {coll: collname, pipeline: [{$unionWith: collname}]}}),
        // maxLength in subPipeline.
        [{
            $unionWith:
                {coll: collname, pipeline: new Array(maxLength).fill({$unionWith: collname})}
        }],
        // maxLength * 50 total unions, should be within max doc size.
        new Array(maxLength).fill(
            {$unionWith: {coll: collname, pipeline: new Array(50).fill({$unionWith: collname})}})]
        .forEach((pipeline) => {
            assert.commandWorked(testDB.runCommand({
                aggregate: collname,
                cursor: {},
                pipeline,
            }));
        });

    // Long pipelines filled with the same stage over and over.
    [{$addFields: {foo: 1}},
     {$bucketAuto: {groupBy: "$nonExistentField", buckets: 1}},
     {
         $graphLookup:
         {from: collname, startWith: "$_id", connectFromField: "_id", connectToField: "_id", as: "foo"}
     },
     {$group: {_id: "$_id"}},
     {$limit: 1},
     {$lookup: {from: collname, localField: "_id", foreignField: "_id", as: "foo"}},
     {$match: {_id: {$exists: true}}},
     {$project: {_id: 1}},
     {$redact: "$$KEEP"},
     {$replaceWith: "$$ROOT"},
     {$skip: 1},
     {$sort: {_id: 1}},
     // unionWith already covered.
     {$unwind: "$_id"}]
        .forEach((stage) => {
            assert.commandWorked(testDB.runCommand({
                aggregate: collname,
                cursor: {},
                pipeline: new Array(maxLength).fill(stage)
            }));
        });

    // Same test, but these to stages get replaced by 2 stages under the hood.
    [{$bucket: {groupBy: "$nonExistentField", boundaries: [0, 1], default: 2}},
     {$sortByCount: "$_id"}]
        .forEach((stage) => assert.commandWorked(testDB.runCommand({
            aggregate: collname,
            cursor: {},
            pipeline: new Array(parseInt(maxLength / 2)).fill(stage)
        })));
}

function runTest(lengthLimit, mongosConfig = {}, mongodConfig = {}) {
    const st = new ShardingTest(
        {shards: 2, rs: {nodes: 1}, other: {mongosOptions: mongosConfig, rsOptions: mongodConfig}});

    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: "test.foo", key: {_id: "hashed"}}));

    let mongosDB = st.s0.getDB("test");
    assert.commandWorked(mongosDB.test.insert([{}, {}, {}, {}]));

    // Run test against mongos.
    testLimits(mongosDB, lengthLimit);

    // Run test against shard.
    let shard0DB = st.rs0.getPrimary().getDB("test");
    testLimits(shard0DB, lengthLimit);

    st.stop();
}

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
const debugBuild = st.s0.getDB("TestDB").adminCommand("buildInfo").debug;
st.stop();

if (!debugBuild) {
    // Test default pipeline length limit.
    runTest(1000);
} else {
    // In debug builds we need to run with a lower limit because the available stack space is half
    // what is available in normal builds.
    runTest(200);
}

// Test with modified pipeline length limit.
runTest(50,
        {setParameter: {internalPipelineLengthLimit: 50}},
        {setParameter: {internalPipelineLengthLimit: 50}});
})();
