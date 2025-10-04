/**
 * Tests $sample pushdown on sharded time-series collections for a small collection size.
 *
 * @tags: [
 * ]
 */
import {documentEq} from "jstests/aggregation/extras/utils.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {planHasStage} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test deliberately inserts orphans.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "weather";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s;
const testDB = mongos.getDB(dbName);
const primary = st.shard0;
const primaryDB = primary.getDB(dbName);
const otherShard = st.shard1;
const otherShardDB = otherShard.getDB(dbName);

let currentId = 0;
function generateId() {
    return currentId++;
}

assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: primary.shardName}));

const testColl = testDB[collName];

function defineChunks() {
    function splitAndMove(city, minTime, destination) {
        assert.commandWorked(
            st.s.adminCommand({
                split: getTimeseriesCollForDDLOps(testDB, testColl).getFullName(),
                middle: {"meta.city": city, "control.min.time": minTime},
            }),
        );
        assert.commandWorked(
            st.s.adminCommand({
                movechunk: getTimeseriesCollForDDLOps(testDB, testColl).getFullName(),
                find: {"meta.city": city, "control.min.time": minTime},
                to: destination.shardName,
                _waitForDelete: true,
            }),
        );
    }

    // Split the chunks such that we have the following distrubtion.
    // {MinKey - Cork, 2021-05-18::9:00} - PrimaryShard
    // {Cork, 2021-05-18::9:00 - Dublin} - OtherShard
    // {Dublin - Galway,2021-05-18::8:00} - PrimaryShard
    // {Galway, 2021-05-18::9:00 - MaxKey} - OtherShard
    splitAndMove("Cork", ISODate("2021-05-18T09:00:00.000Z"), otherShard);
    splitAndMove("Dublin", MinKey, primary);
    splitAndMove("Galway", ISODate("2021-05-18T08:00:00.000Z"), otherShard);
}

function setUpTestColl(generateAdditionalData) {
    assert(testColl.drop());
    assert.commandWorked(
        testDB.adminCommand({
            shardCollection: testColl.getFullName(),
            timeseries: {timeField: "time", metaField: "location", granularity: "hours"},
            key: {"location.city": 1, time: 1},
        }),
    );
    defineChunks();

    const data = [
        // Cork.
        {
            _id: generateId(),
            location: {city: "Cork", coordinates: [-12, 10]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            _id: generateId(),
            location: {city: "Cork", coordinates: [0, 0]},
            time: ISODate("2021-05-18T07:30:00.000Z"),
            temperature: 15,
        },
        // Dublin.
        {
            _id: generateId(),
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            _id: generateId(),
            location: {city: "Dublin", coordinates: [0, 0]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 22,
        },
        {
            _id: generateId(),
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:30:00.000Z"),
            temperature: 12.5,
        },
        {
            _id: generateId(),
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T09:00:00.000Z"),
            temperature: 13,
        },
        // Galway.
        {
            _id: generateId(),
            location: {city: "Galway", coordinates: [22, 44]},
            time: ISODate("2021-05-19T08:00:00.000Z"),
            temperature: 20,
        },
        {
            _id: generateId(),
            location: {city: "Galway", coordinates: [0, 0]},
            time: ISODate("2021-05-19T09:00:00.000Z"),
            temperature: 20,
        },
    ];
    assert.commandWorked(testColl.insertMany(data), {ordered: false});

    let expectedDocs = data.reduce((acc, measure) => {
        acc[measure._id] = {
            _id: measure._id,
            time: measure.time,
            temperature: measure.temperature,
            city: measure.location.city,
        };
        return acc;
    }, {});

    if (generateAdditionalData) {
        expectedDocs = Object.assign({}, expectedDocs, generateAdditionalData());
    }
    return expectedDocs;
}

function containsDocs(actualDocs, expectedDocs) {
    for (const actualDoc of actualDocs) {
        const expectedDoc = expectedDocs[actualDoc._id];
        if (!expectedDoc || !documentEq(actualDoc, expectedDoc)) {
            return false;
        }
    }
    return true;
}

const randomCursor = "COLLSCAN";
const topK = "UNPACK_BUCKET";
const arhash = "QUEUED_DATA";

function checkShardPlanHasStage({root, planName}) {
    // The plan should only contain a TRIAL stage if we had to evaluate whether an ARHASH or Top-K
    // plan was best.
    const hasTrialStage = planHasStage(testDB, root, "TRIAL");
    if (planName === randomCursor) {
        assert(!hasTrialStage, root);
    } else {
        assert(hasTrialStage, root);
    }

    if (planName !== arhash) {
        // The plan should always filter out orphans, but we only see this stage in the top-K case.
        assert(planHasStage(testDB, root, "SHARDING_FILTER"), root);
    }

    return planHasStage(testDB, root, planName);
}

function assertPlanForSample({explainResults, expectedPlan}) {
    for (const shardName of [primary.shardName, otherShard.shardName]) {
        let shardHasPlan = false;
        for (const explainRes of explainResults) {
            const shardsExplain = explainRes.shards;
            const root = shardsExplain[shardName].stages[0].$cursor;
            shardHasPlan = shardHasPlan || checkShardPlanHasStage({root, planName: expectedPlan});
        }
        assert(shardHasPlan, {shardName: shardName, explain: explainResults});
    }
}

function testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount, expectedPlan}) {
    // Restart profiling.
    for (const db of [primaryDB, otherShardDB]) {
        db.setProfilingLevel(0);
        db.system.profile.drop();
        db.setProfilingLevel(2);
    }

    // Verify output documents.
    const result = testColl.aggregate(pipeline).toArray();

    // Verify plan used.
    if (expectedPlan) {
        // The ARHash plan is probabilistic. We may not always pick the plan. So we run the explain
        // command three times to increase the chance of the plan getting picked.
        const numInteration = expectedPlan == arhash ? 3 : 1;
        const explainResults = [];
        for (let i = 0; i < numInteration; ++i) {
            explainResults.push(testColl.explain().aggregate(pipeline));
        }
        assertPlanForSample({explainResults, expectedPlan});
    }

    if (expectedCount) {
        assert.eq(result.length, expectedCount);
    }

    assert(containsDocs(result, expectedDocs), {output: result, expectedDocs: expectedDocs});

    // Verify profiling output.
    if (shardsTargetedCount > 0) {
        let filter = {"command.aggregate": getTimeseriesCollForDDLOps(testDB, testColl).getName()};

        // Filter out any concurrent admin operations.
        if (Object.keys(pipeline[0])[0] == "$match") {
            filter["command.pipeline.0.$match"] = {$exists: true};
        } else {
            filter["command.pipeline.0.$_internalUnpackBucket"] = {$exists: true};
        }

        let actualCount = 0;
        for (const db of [primaryDB, otherShardDB]) {
            const expectedEntries = db.system.profile.find(filter).toArray();
            actualCount += expectedEntries.length;
        }
        assert.eq(actualCount, shardsTargetedCount);
    }
}

const projection = {
    $project: {
        time: 1,
        temperature: 1,
        city: "$location.city",
        _id: 1,
    },
};

/**
 * This function verifies that $sample correctly obtains only documents in the input 'expectedDocs'
 * and ensures shards are targeted correctly. It does the following:
 *  1. Sample a single document from the collection and verify this targets both shards.
 *  2. Sample the given 'proportion' of documents and verify this targets both shards and uses the
 * specified plan.
 *  3. Sample the given 'proportion' of Dublin documents, which are all colocated on the primary and
 * ensure only the primary shard is targeted when we preface $sample with a $match.
 *  4. Sample the given 'proportion' of non-Dublin (Galway, Cork) documents, which can be found on
 * both shards, and ensure we target both shards.
 */
function runTest({proportion, expectedPlan, generateAdditionalData}) {
    const expectedDocs = setUpTestColl(generateAdditionalData);

    let expectedCount = Math.floor(proportion * Object.keys(expectedDocs).length);
    jsTestLog(
        "Running test with proportion: " +
            proportion +
            ", expected count: " +
            expectedCount +
            ", expected plan: " +
            tojson(expectedPlan),
    );

    let pipeline = [{$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount: 2, expectedPlan});

    expectedCount = 1;
    pipeline = [{$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount: 2});

    // Dublin documents are colocated on one shard, so we should only be targeting that shard.
    const dublinDocs = {};
    for (let key in expectedDocs) {
        const doc = expectedDocs[key];
        if (doc.city === "Dublin") {
            dublinDocs[key] = doc;
        }
    }
    const matchDublin = {$match: {"location.city": "Dublin"}};

    expectedCount = Math.floor(proportion * Object.keys(dublinDocs).length);

    pipeline = [matchDublin, {$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs: dublinDocs, expectedCount, shardsTargetedCount: 1});

    // If the $sample precedes the $match, however, we still need to target both shards.
    // Don't use an expected count here, since we are filtering for Dublin docs after sampling.
    pipeline = [{$sample: {size: expectedCount}}, matchDublin, projection];
    testPipeline({pipeline, expectedDocs: dublinDocs, shardsTargetedCount: 2});

    // We should target both shards, since Cork and Galway are split across both shards.
    const nonDublinDocs = {};
    for (let key in expectedDocs) {
        const doc = expectedDocs[key];
        if (doc.city !== "Dublin") {
            nonDublinDocs[key] = doc;
        }
    }
    const excludeDublin = {$match: {$expr: {$ne: ["$location.city", "Dublin"]}}};

    expectedCount = Math.floor(proportion * Object.keys(nonDublinDocs).length);

    pipeline = [excludeDublin, {$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs: nonDublinDocs, expectedCount, shardsTargetedCount: 2});

    // Don't use an expected count here, since we are filtering for non-Dublin docs after sampling.
    pipeline = [{$sample: {size: expectedCount}}, excludeDublin, projection];
    testPipeline({pipeline, expectedDocs: nonDublinDocs, shardsTargetedCount: 2});
    assert(testColl.drop());
}

runTest({proportion: 1});

function generateOrphanData() {
    // Insert orphans and make sure they are filtered out. All "Dublin" buckets are on the primary,
    // so we can insert some Dublin documents on the other shard and make sure they don't appear in
    // any of our searches.
    otherShardDB[collName].insertMany([
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 30,
        },
        {
            location: {city: "Dublin", coordinates: [0, 0]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: -30,
        },
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:30:00.000Z"),
            temperature: 42,
        },
    ]);
    return {};
}
runTest({proportion: 1, generateAdditionalData: generateOrphanData});

function insertAdditionalData(sparselyPackBucket) {
    // Insert more measurements for each city and try again.
    const numMeasurements = 5000;
    let expectedDocs = {};
    for (const city of ["Dublin", "Cork", "Galway"]) {
        let docs = [];
        let orphanDocs = [];
        const startTime = ISODate("2021-05-19T08:00:00.000Z").getTime();
        for (let i = 0; i < numMeasurements; i++) {
            const temperature = i % 10;
            const time = new Date(startTime + i);
            const _id = generateId();
            docs.push({
                _id: _id,
                location: {city, coordinates: [0, sparselyPackBucket ? i : 0]},
                time: time,
                temperature,
            });
            expectedDocs[_id] = {_id, city, temperature, time};

            // Insert one orphan for every 10 measurements to increase the chances the test will
            // fail if we are not filtering out orphans correctly.
            if (city == "Dublin" && i % 10 == 0) {
                const orphanDoc = {
                    location: {city, coordinates: [25, -43]},
                    time: ISODate("2021-05-18T08:00:00.000Z"),
                    temperature: 30,
                };
                orphanDocs.push(orphanDoc);
            }
        }

        // Insert all documents for a city.
        assert.commandWorked(testColl.insertMany(docs, {ordered: false}));

        // Insert any orphan documents.
        if (orphanDocs.length > 0) {
            assert.commandWorked(otherShardDB[collName].insertMany(orphanDocs, {ordered: false}));
        }
    }
    return expectedDocs;
}

// Test a variety of sample sizes to exercise different plans. We run a trail stage if the sample
// size is less than 5% of the total documents. When a trail stage is run, an ARHASH plan is
// generally selected when the buckets are tightly packed and the sample size is small. A Top-K plan
// is selected if the buckets are sparsely packed.
runTest({
    proportion: 0.005,
    generateAdditionalData: () => {
        return insertAdditionalData(false);
    },
    expectedPlan: arhash,
});
runTest({
    proportion: 0.005,
    generateAdditionalData: () => {
        return insertAdditionalData(true);
    },
    expectedPlan: topK,
});

// Top-K plan without the trail stage.
runTest({
    proportion: 0.1,
    generateAdditionalData: () => {
        return insertAdditionalData(false);
    },
    expectedPlan: randomCursor,
});

// Verify that for a sample size > 1000, we pick the Top-K sort plan without any trial.
const expectedDocs = setUpTestColl(() => {
    return insertAdditionalData(false);
});
testPipeline({
    pipeline: [{$sample: {size: 1001}}, projection],
    expectedCount: 1001,
    expectedDocs: expectedDocs,
    shardsTargetedCount: 2,
    expectedPlan: randomCursor,
});

st.stop();
