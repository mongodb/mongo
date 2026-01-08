/**
 * Tests that $$NOW and $$CLUSTER_TIME have the same value in subpipelines as in the parent pipeline.
 * @tags: [
 *   requires_fcv_83,
 *   do_not_wrap_aggregations_in_facets,
 *   assumes_unsharded_collection,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";

db.dropDatabase();
db.test.insertOne({a: 1});

function testSystemTimeVariable(pipeline) {
    const docs = db.test.aggregate(pipeline).toArray();
    for (const doc of docs) {
        const variableMainPipeline = doc["variableMainPipeline"];
        const variableSubPipeline = doc["variableSubPipeline"];
        assert.eq(
            variableMainPipeline,
            variableSubPipeline,
            `Expected variable to be equal in main and subpipeline. Pipeline: ${tojson(pipeline)}`,
        );
    }
}

function generateTestPipelineLookup(variableName) {
    return [
        {
            $lookup: {
                from: "test",
                pipeline: [{$project: {_id: 0, variableSubPipeline: `$$${variableName}`}}],
                as: "out",
            },
        },
        {
            $project: {
                _id: 0,
                variableSubPipeline: {$first: "$out.variableSubPipeline"},
                variableMainPipeline: `$$${variableName}`,
            },
        },
    ];
}

function generateTestPipelineUnionWith(variableName) {
    return [
        {$match: {a: 0}},
        {
            $unionWith: {
                coll: "test",
                pipeline: [{$project: {_id: 0, variableSubPipeline: `$$${variableName}`}}],
            },
        },
        {
            $addFields: {
                variableMainPipeline: `$$${variableName}`,
            },
        },
    ];
}

function generateTestMultipleSubPipelines(variableName) {
    return [
        {$match: {a: 0}},
        {
            $unionWith: {
                coll: "test",
                pipeline: [{$project: {_id: 0, variableMainPipeline: `$$${variableName}`}}],
            },
        },
        {
            $lookup: {
                from: "test",
                pipeline: [{$project: {_id: 0, variableSubPipeline: `$$${variableName}`}}],
                as: "out",
            },
        },
        {
            $project: {
                variableMainPipeline: 1,
                variableSubPipeline: {$first: "$out.variableSubPipeline"},
            },
        },
    ];
}

function runTests(variableName) {
    testSystemTimeVariable(generateTestPipelineLookup(variableName));
    testSystemTimeVariable(generateTestPipelineUnionWith(variableName));
    testSystemTimeVariable(generateTestMultipleSubPipelines(variableName));
}

// Because we are testing time-based variables, we run the test multiple times to reduce the chance of a false positive.
const TEST_COUNT = 100;
const isStandalone = FixtureHelpers.isStandalone(db);

// Run parallel shell to continuously tick cluster time.
const latch = new CountDownLatch(1);
const updateThread = new Thread((latch) => {
    db.background_update.drop();
    let count = 0;
    while (true) {
        db.background_update.insertOne({_id: count});
        count++;
        for (let id = 0; id < count; ++id) {
            db.background_update.updateOne({_id: id}, {$inc: {u: 1}});
        }
        if (latch.getCount() === 0) {
            break;
        }
    }
}, latch);
if (!isStandalone) {
    updateThread.start();
}

for (let testAttempt = 0; testAttempt < TEST_COUNT; testAttempt++) {
    jsTest.log.info("Starting test attempt", {testAttempt, TEST_COUNT});
    runTests("NOW");
    if (!isStandalone) {
        runTests("CLUSTER_TIME");
    }
}

if (!isStandalone) {
    latch.countDown();
    updateThread.join();
}
