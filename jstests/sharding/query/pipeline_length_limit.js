/**
 * Confirms that the limit on number of aggregation pipeline stages is respected.
 * @tags: [
 *  # In 8.3 we perform an additional pipeline validation after a $lookup cache optimization.
 *  requires_fcv_83,
 * ]
 */
import {getExpectedPipelineLimit} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kPreParseErrCode = 7749501;
const kPostParseErrCode = 5054701;

function testLimits(testDB, lengthLimit) {
    let maxLength = lengthLimit;
    let tooLarge = lengthLimit + 1;

    // Test that the enforced pre-parse length limit is the same as the post-parse limit.
    // We use $count because it is desugared into two separate stages, so it will pass the pre-parse
    // limit but fail after.
    // 1. This test case will pass the pre-parse enforcer but fail after.
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: new Array(maxLength).fill({$count: "thecount"}),
        }),
        kPostParseErrCode,
    );

    // 2. This test case should be caught by the pre-parse enforcer, and the error code reflects
    // that.
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: new Array(tooLarge).fill({$count: "thecount"}),
        }),
        kPreParseErrCode,
    );

    assert.commandWorked(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: new Array(maxLength).fill({$project: {_id: 1}}),
        }),
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: new Array(tooLarge).fill({$project: {_id: 1}}),
        }),
        kPreParseErrCode,
    );
    testDB.setLogLevel(1);

    assert.commandWorked(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [{$unionWith: {coll: "test", pipeline: new Array(maxLength).fill({$project: {_id: 1}})}}],
        }),
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [{$unionWith: {coll: "test", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})}}],
        }),
        kPreParseErrCode,
    );

    assert.commandWorked(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [{$facet: {foo: new Array(maxLength).fill({$project: {_id: 1}})}}],
        }),
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [{$facet: {foo: new Array(tooLarge).fill({$project: {_id: 1}}), bar: []}}],
        }),
        kPreParseErrCode,
    );

    assert.commandWorked(
        testDB.runCommand({update: "test", updates: [{q: {}, u: new Array(maxLength).fill({$project: {_id: 1}})}]}),
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            update: "test",
            updates: [{q: {}, u: new Array(tooLarge).fill({$project: {_id: 1}})}],
        }),
        kPreParseErrCode,
    );

    const collname = "test";

    [
        // Long pipelines with many unions.
        new Array(maxLength).fill({$unionWith: collname}),
        // maxLength * 2 total unions.
        new Array(maxLength).fill({$unionWith: {coll: collname, pipeline: [{$unionWith: collname}]}}),
        // maxLength in subPipeline.
        [
            {
                $unionWith: {coll: collname, pipeline: new Array(maxLength).fill({$unionWith: collname})},
            },
        ],
        // maxLength * 50 total unions, should be within max doc size.
        new Array(maxLength).fill({$unionWith: {coll: collname, pipeline: new Array(50).fill({$unionWith: collname})}}),
    ].forEach((pipeline) => {
        assert.commandWorked(
            testDB.runCommand({
                aggregate: collname,
                cursor: {},
                pipeline,
            }),
        );
    });

    // Long pipelines filled with the same stage over and over.
    [
        {$addFields: {foo: 1}},
        {$bucketAuto: {groupBy: "$nonExistentField", buckets: 1}},
        {
            $graphLookup: {
                from: collname,
                startWith: "$_id",
                connectFromField: "_id",
                connectToField: "_id",
                as: "foo",
            },
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
        {$unwind: "$_id"},
    ].forEach((stage) => {
        assert.commandWorked(
            testDB.runCommand({
                aggregate: collname,
                cursor: {},
                pipeline: new Array(maxLength).fill(stage),
            }),
        );
    });

    // Same test, but these to stages get replaced by 2 stages under the hood.
    [{$bucket: {groupBy: "$nonExistentField", boundaries: [0, 1], default: 2}}, {$sortByCount: "$_id"}].forEach(
        (stage) =>
            assert.commandWorked(
                testDB.runCommand({
                    aggregate: collname,
                    cursor: {},
                    pipeline: new Array(parseInt(maxLength / 2)).fill(stage),
                }),
            ),
    );
}

function testLimitsWithLookupCache(testDB, lengthLimit) {
    let maxLength = lengthLimit;
    let tooLarge = lengthLimit + 1;

    const kPreParseErrCode = 7749501;
    const kPostParseErrCode = 5054701;

    // $lookup inserts a DocumentSourceSequentialDocumentCache stage in the subpipeline to perform
    // caching optimizations. We perform pipeline validation again after this, so the
    // subpipeline can have at most 'maxLength - 1' user-specified stages.
    maxLength = maxLength - 1;
    tooLarge = tooLarge - 1;

    assert.commandWorked(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [
                {
                    $lookup: {from: "test", as: "as", pipeline: new Array(maxLength).fill({$project: {_id: 1}})},
                },
            ],
            comment: "lookup maxLength",
        }),
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            aggregate: "test",
            cursor: {},
            pipeline: [
                {
                    $lookup: {from: "test", as: "as", pipeline: new Array(tooLarge).fill({$project: {_id: 1}})},
                },
            ],
            comment: "lookup tooLarge",
        }),
        [kPostParseErrCode, kPreParseErrCode],
    );
}

function runTest(lengthLimit, mongosConfig = {}, mongodConfig = {}) {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
        other: {mongosOptions: mongosConfig, rsOptions: mongodConfig},
    });

    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: "test.foo", key: {_id: "hashed"}}));

    let mongosDB = st.s0.getDB("test");
    assert.commandWorked(mongosDB.test.insert([{}, {}, {}, {}]));

    jsTest.log.info("Running test against mongos");
    testLimits(mongosDB, lengthLimit);

    jsTest.log.info("Running test against shard");
    let shard0DB = st.rs0.getPrimary().getDB("test");
    testLimits(shard0DB, lengthLimit);

    st.stop();
}

function runTestWithLookupCache(lengthLimit, mongosConfig = {}, mongodConfig = {}) {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
        other: {mongosOptions: mongosConfig, rsOptions: mongodConfig},
    });

    // Setup sharded collection with predictable document distribution across shards.
    // This ensures that both shard0 and shard1 have documents to process, which is
    // required for $lookup to execute its subpipeline logic.
    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: "test.test", key: {_id: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: "test.test", middle: {_id: 2}}));

    const mongosDB = st.s0.getDB("test");
    mongosDB.test.insertOne({_id: 1, field: "test1"});
    mongosDB.test.insertOne({_id: 2, field: "test2"});
    mongosDB.test.insertOne({_id: 3, field: "test3"});
    mongosDB.test.insertOne({_id: 4, field: "test4"});

    // Explicitly move chunks to ensure predictable shard distribution:
    // - Shard0 gets documents with _id: 1 (chunk [MinKey, 2))
    // - Shard1 gets documents with _id: 2, 3, 4 (chunk [2, MaxKey))
    assert.commandWorked(st.s0.adminCommand({moveChunk: "test.test", find: {_id: 0}, to: st.rs0.getURL()}));
    assert.commandWorked(st.s0.adminCommand({moveChunk: "test.test", find: {_id: 2}, to: st.rs1.getURL()}));

    jsTest.log.info("Running test against mongos");
    testLimitsWithLookupCache(mongosDB, lengthLimit);

    jsTest.log.info("Running test against shard");
    const shard0DB = st.rs0.getPrimary().getDB("test");
    testLimitsWithLookupCache(shard0DB, lengthLimit);
    st.stop();
}

// This is a sanity check to make sure that the default value is correct. If the limit is changed,
// it will break for users and this check catches that.
const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
let pipelineLimit = assert.commandWorked(st.s0.adminCommand({"getParameter": 1, "internalPipelineLengthLimit": 1}));
assert.eq(getExpectedPipelineLimit(st.s0.getDB("test")), pipelineLimit["internalPipelineLengthLimit"]);

const shardPrimary = st.rs0.getPrimary().getDB("test");
pipelineLimit = assert.commandWorked(shardPrimary.adminCommand({"getParameter": 1, "internalPipelineLengthLimit": 1}));
assert.eq(getExpectedPipelineLimit(shardPrimary), pipelineLimit["internalPipelineLengthLimit"]);
st.stop();

// Test with modified pipeline length limit.
runTest(50, {setParameter: {internalPipelineLengthLimit: 50}}, {setParameter: {internalPipelineLengthLimit: 50}});
runTestWithLookupCache(
    50,
    {setParameter: {internalPipelineLengthLimit: 50}},
    {setParameter: {internalPipelineLengthLimit: 50}},
);
