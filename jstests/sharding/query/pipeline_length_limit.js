/**
 * Confirms that the limit on number of aggregragation pipeline stages is respected.
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getExpectedPipelineLimit} from "jstests/libs/optimizer_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function testLimits(testDB, lengthLimit) {
    let maxLength = lengthLimit;
    let tooLarge = lengthLimit + 1;

    // Test that the enforced pre-parse length limit is the same as the post-parse limit.
    // We use $count because it is desugared into two separate stages, so it will pass the pre-parse
    // limit but fail after.
    let kPreParseErrCode = 7749501;
    let kPostParseErrCode = 5054701;

    // 1. This test case will pass the pre-parse enforcer but fail after.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(maxLength).fill({$count: "thecount"})
    }),
                                 kPostParseErrCode);

    // 2. This test case should be caught by the pre-parse enforcer, and the error code reflects
    // that.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: "test",
        cursor: {},
        pipeline: new Array(tooLarge).fill({$count: "thecount"})
    }),
                                 kPreParseErrCode);

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
                                 kPreParseErrCode);
    testDB.setLogLevel(1);

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
                                 kPreParseErrCode);

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
                                 kPreParseErrCode);

    assert.commandWorked(testDB.runCommand(
        {update: "test", updates: [{q: {}, u: new Array(maxLength).fill({$project: {_id: 1}})}]}));
    assert.commandFailedWithCode(testDB.runCommand({
        update: "test",
        updates: [{q: {}, u: new Array(tooLarge).fill({$project: {_id: 1}})}]
    }),
                                 kPreParseErrCode);

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

    // $lookup inserts a DocumentSourceSequentialDocumentCache stage in the subpipeline to perform
    // cacheing optimizations, so the subpipeline can have at most 'maxLength - 1' user-specified
    // stages. When we connect directly to a shard without mongos, it is treated as a standalone
    // and will not perform pipeline length validation after the cache stage is added.
    if (FixtureHelpers.isMongos(testDB)) {
        maxLength = maxLength - 1;
        tooLarge = tooLarge - 1;
    }

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
                                 [kPostParseErrCode, kPreParseErrCode]);
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

// This is a sanity check to make sure that the default value is correct. If the limit is changed,
// it will break for users and this check catches that.
const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
let pipelineLimit =
    assert.commandWorked(st.s0.adminCommand({"getParameter": 1, "internalPipelineLengthLimit": 1}));
assert.eq(getExpectedPipelineLimit(st.s0.getDB("test")),
          pipelineLimit["internalPipelineLengthLimit"]);

const shardPrimary = st.rs0.getPrimary().getDB("test");
pipelineLimit = assert.commandWorked(
    shardPrimary.adminCommand({"getParameter": 1, "internalPipelineLengthLimit": 1}));
assert.eq(getExpectedPipelineLimit(shardPrimary), pipelineLimit["internalPipelineLengthLimit"]);
st.stop();

// Test with modified pipeline length limit.
runTest(50,
        {setParameter: {internalPipelineLengthLimit: 50}},
        {setParameter: {internalPipelineLengthLimit: 50}});
