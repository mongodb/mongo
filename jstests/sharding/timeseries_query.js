/**
 * Tests the queries on sharded time-series collection with various combination of shard key
 * patterns.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command,
 * ]
 */

(function() {
load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const unshardedColl = "unsharded";
const timeField = 'time';
const metaField = 'hostid';

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const sDB = st.s.getDB(dbName);
assert.commandWorked(sDB.adminCommand({enableSharding: dbName}));
const shard0DB = st.shard0.getDB(dbName);
const shard1DB = st.shard1.getDB(dbName);

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

// Helpers.
let currentId = 0;
function generateId() {
    return currentId++;
}

function generateDoc(time, metaValue) {
    return TimeseriesTest.generateHosts(1).map((host, index) => Object.assign(host, {
        _id: generateId(),
        [metaField]: metaValue,
        [timeField]: ISODate(time),
    }))[0];
}

function runQueryOnTimeField({date, matchOperator, expectedDocs, expectedShards}) {
    runQuery({query: {time: {[matchOperator]: ISODate(date)}}, expectedDocs, expectedShards});
}

function runInsert(timestamp, metaValue) {
    const doc = generateDoc(timestamp, metaValue);
    assert.commandWorked(sDB.getCollection(collName).insert(doc));
    assert.commandWorked(sDB.getCollection(unshardedColl).insert(doc));
}

function runQuery(
    {query, expectedDocs, expectedShards, expectQueryRewrite = true, expectCollScan = false}) {
    // Restart profiler.
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }

    // Run the query with both find and aggregate commands. Both should return same number of
    // entries.
    const coll = sDB.getCollection(collName);
    const findOutput = coll.find(query).toArray();
    assert.eq(findOutput.length, expectedDocs, findOutput);

    const unshardedOutput = sDB.getCollection(unshardedColl).find(query).toArray();
    assert.sameMembers(unshardedOutput, findOutput);

    const aggOutput = coll.aggregate([{$match: query}]).toArray();
    assert.sameMembers(unshardedOutput, aggOutput);

    if (expectedShards) {
        let filter = {
            "command.aggregate": `system.buckets.${collName}`,
        };

        // If the query was rewritten to be a match expression on the buckets collection, we should
        // expect a $match stage at the beginning of the pipeline.
        if (expectQueryRewrite) {
            filter["command.pipeline.0.$match"] = {$exists: true};
        } else {
            filter["command.pipeline.0.$_internalUnpackBucket"] = {$exists: true};
        }

        const shard0Entries = shard0DB.system.profile.find(filter).toArray();
        const shard1Entries = shard1DB.system.profile.find(filter).toArray();

        const primaryShard = st.getPrimaryShard(dbName);
        if (expectedShards.includes(primaryShard.shardName)) {
            // There should one profiler entry for each of the find and aggregate commands.
            assert.eq(shard0Entries.length, 2, shard0Entries);
        } else {
            assert.eq(shard0Entries.length, 0, shard0Entries);
        }

        const otherShard = st.getOther(primaryShard);
        if (expectedShards.includes(otherShard.shardName)) {
            // There should one profiler entry for each of the find and aggregate commands.
            assert.eq(shard1Entries.length, 2, shard1Entries);
        } else {
            assert.eq(shard1Entries.length, 0, shard1Entries);
        }

        // Test the explain plans from all of the expected shards.
        const plans = [
            coll.find(query).explain(),
            coll.explain().aggregate([{$match: query}]),
            coll.aggregate([{$match: query}], {explain: true})
        ];
        plans.forEach(plan => {
            assert.eq(expectedShards.sort(), Object.keys(plan.shards).sort());
            expectedShards.forEach(shard => {
                const winningPlan = plan.shards[shard].stages[0].$cursor.queryPlanner.winningPlan;
                if (expectCollScan) {
                    assert(isCollscan(sDB, winningPlan));
                } else {
                    assert(isIxscan(sDB, winningPlan));
                }
            });
        });
    }
}

// Shard key on just the time field.
(function timeShardKey() {
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard time-series collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(sDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
        timeseries: {timeField, granularity: "hours"}
    }));

    // Split the chunks such that primary shard has chunk: [MinKey, 2020-01-01) and other shard has
    // chunk [2020-01-01, MaxKey].
    splitPoint = {[`control.min.${timeField}`]: ISODate(`2020-01-01`)};
    assert.commandWorked(
        sDB.adminCommand({split: `${dbName}.system.buckets.${collName}`, middle: splitPoint}));

    // Move one of the chunks into the second shard.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(sDB.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensure that each shard owns one chunk.
    const counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    const coll = sDB.getCollection(collName);
    for (let i = 0; i < 2; i++) {
        runInsert("2019-11-11");
        runInsert("2019-12-31");
        runInsert("2020-01-21");
        runInsert("2020-11-31");
    }

    // EQ.
    runQueryOnTimeField({
        date: "2019-11-11",
        matchOperator: "$eq",
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2019-12-31",
        matchOperator: "$eq",
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-01-21",
        matchOperator: "$eq",
        expectedDocs: 2,
        expectedShards: [otherShard.shardName, primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-11-31",
        matchOperator: "$eq",
        expectedDocs: 2,
        expectedShards: [otherShard.shardName]
    });

    // LTE.
    runQueryOnTimeField({
        date: "2019-11-11",
        matchOperator: "$lte",
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2019-12-31",
        matchOperator: "$lte",
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-02-11",
        matchOperator: "$lte",
        expectedDocs: 6,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2021-01-01",
        matchOperator: "$lte",
        expectedDocs: 8,
        expectedShards: [
            primaryShard.shardName,
            otherShard.shardName,
        ]
    });

    // LT.
    runQueryOnTimeField({
        date: "2019-11-11",
        matchOperator: "$lt",
        expectedDocs: 0,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2019-12-31",
        matchOperator: "$lte",
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-02-11",
        matchOperator: "$lt",
        expectedDocs: 6,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2021-01-01",
        matchOperator: "$lt",
        expectedDocs: 8,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });

    // GTE
    runQueryOnTimeField({
        date: "2019-11-11",
        matchOperator: "$gte",
        expectedDocs: 8,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-01-11",
        matchOperator: "$gte",
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2021-01-01",
        matchOperator: "$gte",
        expectedDocs: 0,
        expectedShards: [otherShard.shardName]
    });

    // GT
    runQueryOnTimeField({
        date: "2019-11-11",
        matchOperator: "$gt",
        expectedDocs: 6,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2020-01-11",
        matchOperator: "$gt",
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQueryOnTimeField({
        date: "2021-01-01",
        matchOperator: "$gt",
        expectedDocs: 0,
        expectedShards: [otherShard.shardName]
    });

    // OR queries are currently not re-written. So expect the query to be sent to both the shards,
    // and do a full collection scan on each shard.
    runQuery({
        query: {$or: [{time: ISODate("2019-11-11")}, {time: ISODate("2019-11-12")}]},
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName, otherShard.shardName],
        expectQueryRewrite: false,
        expectCollScan: true,
    });

    assert(coll.drop());
})();

// Shard key on the metadata field and time fields.
(function metaAndTimeShardKey() {
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(sDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: {[metaField]: 1, 'time': 1},
        timeseries: {timeField, metaField, granularity: "hours"}
    }));

    // Split the chunks such that primary shard has chunk: [{MinKey, MinKey}, {0, 2020-01-01}) and
    // other shard has chunk [{0, 2020-01-01}, {MaxKey, MaxKey}].
    assert.commandWorked(st.s.adminCommand({
        split: `${dbName}.system.buckets.${collName}`,
        middle: {meta: 0, 'control.min.time': ISODate(`2020-01-01`)}
    }));

    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(st.s.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: {meta: 10, 'control.min.time': MinKey},
        to: otherShard.shardName,
        _waitForDelete: true
    }));

    const counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[st.shard0.shardName]);
    assert.eq(1, counts[st.shard1.shardName]);

    const coll = sDB.getCollection(collName);

    for (let i = 0; i < 2; i++) {
        runInsert("2019-11-11", i);
        runInsert("2019-12-31", -i);
        runInsert("2020-01-21", i);
        runInsert("2020-11-31", -i);
    }
    runInsert("2020-11-31", {subField: 1});

    // EQ.
    runQuery({
        query: {[metaField]: 0},
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({query: {[metaField]: 1}, expectedDocs: 2, expectedShards: [otherShard.shardName]});

    // Query on sub-fields of shard key pattern will be routed to all shards.
    runQuery({
        query: {[`${metaField}.subField`]: 1},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName, otherShard.shardName],
        // Since the index cannot be used on the meta sub-field, expect a full collection scan.
        expectCollScan: true,
    });

    runQuery({
        query: {[metaField]: 0, [timeField]: ISODate("2019-12-31")},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: ISODate("2020-01-21")},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });

    // LTE.
    runQuery({
        query: {[metaField]: 0, [timeField]: {$lte: ISODate("2019-12-31")}},
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName]
    });
    runQuery({
        query: {[metaField]: 1, [timeField]: {$lte: ISODate("2020-11-11")}},
        expectedDocs: 2,
        expectedShards: [otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$lte: ISODate("2020-11-11")}},
        expectedDocs: 3,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: {$lte: 0}, [timeField]: {$lte: ISODate("2019-12-31")}},
        expectedDocs: 3,
        expectedShards: [primaryShard.shardName]
    });

    // LT.
    runQuery({
        query: {[metaField]: 0, [timeField]: {$lt: ISODate("2019-12-31")}},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName]
    });
    runQuery({
        query: {[metaField]: 1, [timeField]: {$lt: ISODate("2020-11-11")}},
        expectedDocs: 2,
        expectedShards: [otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$lt: ISODate("2020-11-11")}},
        expectedDocs: 3,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: {$lt: 0}, [timeField]: {$lt: ISODate("2019-12-31")}},
        expectedDocs: 0,
        expectedShards: [primaryShard.shardName]
    });

    // GTE.
    runQuery({
        query: {[metaField]: -1, [timeField]: {$gte: ISODate("2020-11-31")}},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$gte: ISODate("2020-11-11")}},
        expectedDocs: 1,
        expectedShards: [otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$gte: ISODate("2020-01-02")}},
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: {$gte: 0}, [timeField]: {$gte: ISODate("2020-02-01")}},
        expectedDocs: 1,
        expectedShards: [otherShard.shardName]
    });

    // GT.
    runQuery({
        query: {[metaField]: -1, [timeField]: {$gt: ISODate("2020-11-31")}},
        expectedDocs: 0,
        expectedShards: [primaryShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$gt: ISODate("2020-11-11")}},
        expectedDocs: 1,
        expectedShards: [otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: 0, [timeField]: {$gt: ISODate("2020-01-02")}},
        expectedDocs: 2,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({
        query: {[metaField]: {$gt: 0}, [timeField]: {$gt: ISODate("2020-02-01")}},
        expectedDocs: 0,
        expectedShards: [otherShard.shardName]
    });

    assert(coll.drop());
})();

// Shard key on the metadata fields.
(function metaFieldShardKey() {
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard timeseries collection.
    const metaPrefix = `${metaField}.prefix`;
    const metaSuffix = `${metaField}.suffix`;
    const shardKey = {[metaPrefix]: 1, [metaSuffix]: 1};
    assert.commandWorked(sDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
        timeseries: {timeField, metaField}
    }));

    splitPoint = {'meta.prefix': 0, 'meta.suffix': 0};
    assert.commandWorked(
        sDB.adminCommand({split: `${dbName}.system.buckets.${collName}`, middle: splitPoint}));

    // Move one of the chunks into the second shard.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(sDB.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensure that each shard owns one chunk.
    const counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    const coll = sDB.getCollection(collName);

    for (let i = 0; i < 2; i++) {
        runInsert("2019-11-11", {prefix: -i, suffix: -i});
        runInsert("2019-12-31", {prefix: -i, suffix: i});
        runInsert("2020-01-21", {prefix: i, suffix: -i});
        runInsert("2020-11-31", {prefix: i, suffix: i});
    }

    // Equality queries on prefix and full shard key values.
    runQuery({
        query: {[metaPrefix]: 0},
        expectedDocs: 4,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });
    runQuery({
        query: {[metaPrefix]: 0, [metaSuffix]: 0},
        expectedDocs: 4,
        expectedShards: [otherShard.shardName]
    });
    runQuery({query: {[metaPrefix]: 1}, expectedDocs: 2, expectedShards: [otherShard.shardName]});

    runQuery({
        query: {[metaPrefix]: 0, [timeField]: ISODate("2019-12-31")},
        expectedDocs: 1,
        expectedShards: [primaryShard.shardName, otherShard.shardName]
    });

    assert(coll.drop());
})();

st.stop();
})();
