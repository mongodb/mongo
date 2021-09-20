/**
 * Tests $sample pushdown on sharded time-series collections for a small collection size.
 *
 * @tags: [requires_fcv_51]
 */

// Test deliberately inserts orphans.
TestData.skipCheckOrphans = true;

(function() {
load("jstests/aggregation/extras/utils.js");         // For arrayEq, documentEq.
load("jstests/core/timeseries/libs/timeseries.js");  // For TimeseriesTest.
load("jstests/libs/analyze_plan.js");                // For planHasStage.

const dbName = 'test';
const collName = 'weather';
const bucketCollName = `system.buckets.${collName}`;
const bucketCollFullName = `${dbName}.${bucketCollName}`;

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s;
const testDB = mongos.getDB(dbName);
const primary = st.shard0;
const primaryDB = primary.getDB(dbName);
const otherShard = st.shard1;
const otherShardDB = otherShard.getDB(dbName);

if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(primary)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

assert.commandWorked(testDB.adminCommand({enableSharding: dbName}));

st.ensurePrimaryShard(testDB.getName(), primary.shardName);

assert.commandWorked(testDB.createCollection(
    collName, {timeseries: {timeField: "time", metaField: "location", granularity: "hours"}}));

const testColl = testDB[collName];
(function setUpTestColl() {
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: testColl.getFullName(), key: {"location.city": 1, time: 1}}));

    const data = [
        // Cork.
        {
            location: {city: "Cork", coordinates: [-12, 10]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            location: {city: "Cork", coordinates: [0, 0]},
            time: ISODate("2021-05-18T07:30:00.000Z"),
            temperature: 15,
        },
        // Dublin.
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            location: {city: "Dublin", coordinates: [0, 0]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 22,
        },
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:30:00.000Z"),
            temperature: 12.5,
        },
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T09:00:00.000Z"),
            temperature: 13,
        },
        // Galway.
        {
            location: {city: "Galway", coordinates: [22, 44]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 20,
        },
        {
            location: {city: "Galway", coordinates: [0, 0]},
            time: ISODate("2021-05-18T09:00:00.000Z"),
            temperature: 20,
        },
    ];
    assert.commandWorked(testColl.insertMany(data));
})();

(function defineChunks() {
    function splitAndMove(city, minTime, destination) {
        assert.commandWorked(st.s.adminCommand(
            {split: bucketCollFullName, middle: {"meta.city": city, 'control.min.time': minTime}}));
        assert.commandWorked(st.s.adminCommand({
            movechunk: bucketCollFullName,
            find: {"meta.city": city, 'control.min.time': minTime},
            to: destination.shardName,
            _waitForDelete: true
        }));
    }

    // Place the Dublin buckets on the primary and split the other buckets across both shards.
    splitAndMove("Galway", ISODate("2021-05-18T08:00:00.000Z"), otherShard);
    splitAndMove("Dublin", MinKey, primary);
    splitAndMove("Cork", ISODate("2021-05-18T09:00:00.000Z"), otherShard);
})();

function containsDocs(actualDocs, expectedDocs) {
    let contains = true;
    for (const actualDoc of actualDocs) {
        for (const expectedDoc of expectedDocs) {
            contains = documentEq(actualDoc, expectedDoc);
            if (contains)
                break;
        }
        if (!contains)
            break;
    }
    return contains;
}

const randomCursor = "COLLSCAN";
const topK = "UNPACK_BUCKET";
const arhash = "QUEUED_DATA";
function assertPlanForSampleOnShard({root, planName}) {
    // The plan should only contain a TRIAL stage if we had to evaluate whether an ARHASH or Top-K
    // plan was best.
    const hasTrialStage = planHasStage(testDB, root, "TRIAL");
    if (planName === randomCursor) {
        assert(!hasTrialStage, root);
    } else {
        assert(hasTrialStage, root);
    }

    // Ensure the plan contains the stage we expect to see for that plan.
    assert(planHasStage(testDB, root, planName), root);
    if (planName !== arhash) {
        // The plan should always filter out orphans, but we only see this stage in the top-K case.
        assert(planHasStage(testDB, root, "SHARDING_FILTER"), root);
    }
}

function assertPlanForSample({explainRes, planForShards}) {
    const shardsExplain = explainRes.shards;
    for (const shardName of [primary.shardName, otherShard.shardName]) {
        const root = shardsExplain[shardName].stages[0].$cursor;
        assertPlanForSampleOnShard({root, planName: planForShards[shardName]});
    }
}

function testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount, planForShards}) {
    // Restart profiling.
    for (const db of [primaryDB, otherShardDB]) {
        db.setProfilingLevel(0);
        db.system.profile.drop();
        db.setProfilingLevel(2);
    }

    // Verify output documents.
    const result = testColl.aggregate(pipeline).toArray();

    // Verify plan used.
    if (planForShards) {
        const explainRes = testColl.explain().aggregate(pipeline);
        assertPlanForSample({explainRes, planForShards});
    }

    if (expectedCount) {
        assert.eq(result.length, expectedCount);
    }

    if (expectedCount == expectedDocs.length) {
        arrayEq(result, expectedDocs);
    } else {
        assert(containsDocs(result, expectedDocs), tojson(result));
    }

    // Verify profiling output.
    if (shardsTargetedCount > 0) {
        let filter = {"command.aggregate": bucketCollName};

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
        time: {$dateToString: {date: "$time", format: "%H:%M"}},
        temperature: 1,
        city: "$location.city",
        _id: 0,
    }
};
const initialExpectedDocs = [
    {time: "08:00", temperature: 12, city: "Dublin"},
    {time: "08:00", temperature: 22, city: "Dublin"},
    {time: "08:30", temperature: 12.5, city: "Dublin"},
    {time: "09:00", temperature: 13, city: "Dublin"},
    {time: "08:00", temperature: 20, city: "Galway"},
    {time: "09:00", temperature: 20, city: "Galway"},
    {time: "08:00", temperature: 12, city: "Cork"},
    {time: "07:30", temperature: 15, city: "Cork"},
];

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
function runTest(expectedDocs, proportion, planForShards) {
    let expectedCount = 1;
    let pipeline = [{$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount: 2});

    expectedCount = Math.floor(proportion * expectedDocs.length);

    pipeline = [{$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs, expectedCount, shardsTargetedCount: 2, planForShards});

    // Dublin documents are colocated on one shard, so we should only be targeting that shard.
    const dublinDocs = expectedDocs.filter(doc => doc.city === "Dublin");
    const matchDublin = {$match: {"location.city": "Dublin"}};

    expectedCount = Math.floor(proportion * dublinDocs.length);

    pipeline = [matchDublin, {$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs: dublinDocs, expectedCount, shardsTargetedCount: 1});

    // If the $sample precedes the $match, however, we still need to target both shards.
    // Don't use an expected count here, since we are filtering for Dublin docs after sampling.
    pipeline = [{$sample: {size: expectedCount}}, matchDublin, projection];
    testPipeline({pipeline, expectedDocs: dublinDocs, shardsTargetedCount: 2});

    // We should target both shards, since Cork and Galway are split across both shards.
    const nonDublinDocs = expectedDocs.filter(doc => doc.city !== "Dublin");
    const excludeDublin = {$match: {$expr: {$ne: ["$location.city", "Dublin"]}}};

    expectedCount = Math.floor(proportion * nonDublinDocs.length);

    pipeline = [excludeDublin, {$sample: {size: expectedCount}}, projection];
    testPipeline({pipeline, expectedDocs: nonDublinDocs, expectedCount, shardsTargetedCount: 2});

    // Don't use an expected count here, since we are filtering for non-Dublin docs after sampling.
    pipeline = [{$sample: {size: expectedCount}}, excludeDublin, projection];
    testPipeline({pipeline, expectedDocs: nonDublinDocs, shardsTargetedCount: 2});
}

runTest(initialExpectedDocs, 1);

// Insert orphans and make sure they are filtered out. All "Dublin" buckets are on the primary, so
// we can insert some Dublin documents on the other shard and make sure they don't appear in any of
// our searches.
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
    }
]);
runTest(initialExpectedDocs, 1);

// Insert more measurements for each city and try again.
const numMeasurements = 500;
let updatedExpectedDocs = initialExpectedDocs;
for (const city of ["Dublin", "Cork", "Galway"]) {
    let docs = [];
    let orphanDocs = [];
    for (let i = 0; i < numMeasurements; i++) {
        const minute = String(i % 60).padStart(2, '0');
        const temperature = i % 10;
        const time = `08:${minute}`;
        docs.push({
            location: {city, coordinates: [0, 0]},
            time: ISODate(`2021-05-18T${time}:00.000Z`),
            temperature,
        });
        updatedExpectedDocs.push({city, temperature, time});

        // Insert one orphan for every 10 measurements to increase the chances the test will fail if
        // we are not filtering out orphans correctly.
        if (city == "Dublin" && (i % 10 == 0)) {
            const orphanDoc = {
                location: {city, coordinates: [25, -43]},
                time: ISODate("2021-05-18T08:00:00.000Z"),
                temperature: 30,
            };
            orphanDocs.push(orphanDoc);
        }
    }

    // Insert all documents for a city.
    assert.commandWorked(testColl.insertMany(docs));

    // Insert any orphan documents.
    if (orphanDocs) {
        assert.commandWorked(otherShardDB[collName].insertMany(orphanDocs));
    }
}

// Test a variety of sample sizes to exercise both Top-K sort and ARHASH plans, where ARHASH is
// selected for sample sizes of <=1% on the primary, and we don't actually run a trial at all for
// sample sizes >= 5%. The secondary always picks Top-K when a trial is evaluated.
for (const proportion of [0.0025, 0.005, 0.01, 0.05]) {
    runTest(updatedExpectedDocs, proportion, {
        [primary.shardName]:
            (proportion >= 0.05 ? randomCursor : (proportion <= 0.01 ? arhash : topK)),
        [otherShard.shardName]: (proportion >= 0.05 ? randomCursor : topK),
    });
}

// Verify that for a sample size > 1000, we pick the Top-K sort plan without any trial.
testPipeline({
    pipeline: [{$sample: {size: 1001}}, projection],
    expectedDocs: updatedExpectedDocs,
    expectedCount: 1001,
    shardsTargetedCount: 2,
    planForShards: {
        [primary.shardName]: randomCursor,
        [otherShard.shardName]: randomCursor,
    }
});

st.stop();
})();
