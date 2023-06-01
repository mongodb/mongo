/**
 * Verifies multi-updates on sharded timeseries collection. These commands operate on multiple
 * individual measurements by targeting them with their meta and/or time field value.
 *
 * @tags: [
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

Random.setRandomSeed();

const dbName = jsTestName();
const collName = 'sharded_timeseries_update_multi';
const timeField = 'time';
const metaField = 'hostid';
const testStringCamelCase = "testString";
const testStringNoCase = "teststring";
const caseInsensitiveCollation = {
    locale: "en",
    strength: 2
};

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

// Databases.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const testDB = mongos.getDB(dbName);

function generateTimeValue(index) {
    return ISODate(`${2000 + index}-01-01`);
}

// The split point between two shards. This value guarantees that generated time values do not fall
// on this boundary.
const splitTimePointBetweenTwoShards = ISODate("2001-06-30");
const numOfDocs = 4;

function generateDocsForTestCase(collConfig) {
    const documents = TimeseriesTest.generateHosts(numOfDocs);
    for (let i = 0; i < numOfDocs; i++) {
        documents[i]._id = i;
        if (collConfig.metaGenerator) {
            documents[i][metaField] = collConfig.metaGenerator(i);
        }
        documents[i][timeField] = generateTimeValue(i);
        documents[i].f = i;
        documents[i].stringField = testStringCamelCase;
    }
    return documents;
}

const collectionConfigurations = {
    // Shard key only on meta field/subfields.
    metaShardKey: {
        metaGenerator: (id => id),
        shardKey: {[metaField]: 1},
        splitPoint: {meta: 2},
    },
    metaShardKeyString: {
        metaGenerator: (id => `string:${id}`),
        shardKey: {[metaField]: 1},
        splitPoint: {meta: `string:2`},
    },
    metaObjectShardKey: {
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField]: 1},
        splitPoint: {meta: {a: 2}},
    },
    metaSubFieldShardKey: {
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField + '.a']: 1},
        splitPoint: {'meta.a': 2},
    },

    // Shard key on time field.
    timeShardKey: {
        shardKey: {[timeField]: 1},
        splitPoint: {[`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },

    // Shard key on both meta and time field.
    metaTimeShardKey: {
        metaGenerator: (id => id),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
    metaObjectTimeShardKey: {
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: {a: 2}, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
    metaSubFieldTimeShardKey: {
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField + '.a']: 1, [timeField]: 1},
        splitPoint: {'meta.a': 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
};

const requestConfigurations = {
    // Empty filter leads to broadcasted request.
    emptyFilter: {
        updateList: [{
            q: {},
            u: {$set: {"newField": 1}},
            multi: true,
        }],
        expectedUpdates: {findQuery: {"newField": {$eq: 1}}, expectedMatchingIds: [0, 1, 2, 3]},
        reachesPrimary: true,
        reachesOther: true,
    },
    // Non-shard key filter without meta or time field leads to broadcasted request.
    nonShardKeyFilter: {
        updateList: [
            {
                q: {f: 0},
                u: {$unset: {f: ""}},
                multi: true,
            },
            {
                q: {f: 2},
                u: {$unset: {f: ""}},
                multi: true,
            }
        ],
        expectedUpdates: {findQuery: {f: {$exists: false}}, expectedMatchingIds: [0, 2]},
        reachesPrimary: true,
        reachesOther: true,
    },
    // This time field filter has the request targeted to the shard0.
    timeFilterOneShard: {
        updateList: [
            {
                q: {[timeField]: generateTimeValue(0), f: 0},
                u: [
                    {$unset: "f"},
                    {$set: {"newField": 1}},
                ],
                multi: true,
            },
            {
                q: {[timeField]: generateTimeValue(1), f: 1},
                u: [
                    {$unset: "f"},
                    {$set: {"newField": 1}},
                ],
                multi: true,
            }
        ],
        expectedUpdates: {
            findQuery: {$and: [{"f": {$exists: false}}, {"newField": {$exists: true}}]},
            expectedMatchingIds: [0, 1]
        },
        reachesPrimary: true,
        reachesOther: false,
    },
    // This time field filter has the request targeted to both shards.
    timeFilterTwoShards: {
        updateList: [
            {
                q: {[timeField]: generateTimeValue(1), f: 1},
                u: {$set: {f: ["arr", "ay"]}},
                multi: true,
            },
            {
                q: {[timeField]: generateTimeValue(3), f: 3},
                u: {$set: {f: ["arr", "ay"]}},
                multi: true,
            }
        ],
        expectedUpdates: {findQuery: {f: ["arr", "ay"]}, expectedMatchingIds: [1, 3]},
        reachesPrimary: true,
        reachesOther: true,
    },
    // This meta field filter targets shard1 and queries on the 'stringField' using the default
    // collation. We expect no document to be modified.
    metaFilterOneShard: {
        updateList: [{
            q: {[metaField]: 2, f: 2, stringField: testStringNoCase},
            u: [
                {$unset: "f"},
                {$set: {"newField": 1}},
                {$set: {"_id": 200}},
            ],
            multi: true,
        }],
        expectedUpdates: {
            findQuery: {$and: [{[metaField]: {$eq: 2}}, {"_id": {$eq: 200}}]},
            expectedMatchingIds: []
        },
        reachesPrimary: false,
        reachesOther: true,
    },
    // This meta field filter targets shard1 and queries on the 'stringField' using a case
    // insensitive collation.
    metaFilterOneShardWithCaseInsensitiveCollation: {
        updateList: [{
            q: {[metaField]: 2, f: 2, stringField: testStringNoCase},
            u: [
                {$unset: "f"},
                {$set: {"newField": 1}},
                {$set: {"_id": 200}},
            ],
            multi: true,
            collation: caseInsensitiveCollation,
        }],
        expectedUpdates: {
            findQuery: {$and: [{[metaField]: {$eq: 2}}, {"_id": {$eq: 200}}]},
            expectedMatchingIds: [200]
        },
        reachesPrimary: false,
        reachesOther: true,
    },
    // This string, meta field filter targets shard1 using the default collation.
    metaFilterOneShardString: {
        updateList: [{
            q: {[metaField]: `string:3`},
            u: [
                {$set: {"newField": 1}},
                {$set: {"_id": 300}},
            ],
            multi: true,
        }],
        expectedUpdates: {findQuery: {"_id": {$eq: 300}}, expectedMatchingIds: [300]},
        reachesPrimary: false,
        reachesOther: true,
    },
    // This string, meta field filter broadcasts the request because collection routing info is
    // organized by the collection default collation and modifies the corresponding doc using a case
    // insensitive collation.
    metaFilterTwoShardsStringCaseInsensitive: {
        updateList: [{
            q: {[metaField]: `StrinG:3`},
            u: [
                {$set: {"newField": 1}},
                {$set: {"_id": 300}},
            ],
            multi: true,
            collation: caseInsensitiveCollation,
        }],
        expectedUpdates: {findQuery: {"_id": {$eq: 300}}, expectedMatchingIds: [300]},
        reachesPrimary: true,
        reachesOther: true,
    },
    // Meta + time filter has the request targeted to shard1.
    metaTimeFilterOneShard: {
        updateList: [{
            q: {[metaField]: 2, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {f: 1000}},
            multi: true,
        }],
        expectedUpdates: {findQuery: {f: 1000}, expectedMatchingIds: [2]},
        reachesPrimary: false,
        reachesOther: true,
    },
    metaFilterTwoShards: {
        updateList: [
            {
                q: {[timeField]: generateTimeValue(1), f: 1},
                u: {$set: {"newField": 101}},
                multi: true,
            },
            {
                q: {[timeField]: generateTimeValue(3), f: 3},
                u: {$set: {"newField": 101}},
                multi: true,
            }
        ],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [1, 3]},
        reachesPrimary: true,
        reachesOther: true,
    },
    metaObjectFilterOneShard: {
        updateList: [{
            q: {[metaField]: {a: 2}, f: 2},
            u: {$set: {"newField": 101}},
            multi: true,
        }],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [2]},
        reachesPrimary: false,
        reachesOther: true,
    },
    // Meta object + time filter has the request targeted to shard1.
    metaObjectTimeFilterOneShard: {
        updateList: [{
            q: {[metaField]: {a: 2}, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {f: 2000}},
            multi: true,
        }],
        expectedUpdates: {findQuery: {f: 2000}, expectedMatchingIds: [2]},
        reachesPrimary: false,
        reachesOther: true,
    },
    metaObjectFilterTwoShards: {
        updateList: [
            {
                q: {[metaField]: {a: 1}, f: 1},
                u: {$set: {"newField": 101}},
                multi: true,
            },
            {
                q: {[metaField]: {a: 2}, f: 2},
                u: {$set: {"newField": 101}},
                multi: true,
            }
        ],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [1, 2]},
        reachesPrimary: true,
        reachesOther: true,
    },
    metaSubFieldFilterOneShard: {
        updateList: [{
            q: {[metaField + '.a']: 2, f: 2},
            u: [
                {$set: {"newField": 101}},
            ],
            multi: true,
        }],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [2]},
        reachesPrimary: false,
        reachesOther: true,
    },
    // Meta sub field + time filter has the request targeted to shard1.
    metaSubFieldTimeFilterOneShard: {
        updateList: [{
            q: {[metaField + '.a']: 2, [timeField]: generateTimeValue(2), f: 2},
            u: {$set: {"newField": 101}},
            multi: true,
        }],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [2]},
        reachesPrimary: false,
        reachesOther: true,
    },
    metaSubFieldFilterTwoShards: {
        updateList: [
            {
                q: {[metaField + '.a']: 1, f: 1},
                u: {$set: {"newField": 101}},
                multi: true,
            },
            {
                q: {[metaField + '.a']: 2, f: 2},
                u: {$set: {"newField": 101}},
                multi: true,
            }
        ],
        expectedUpdates: {findQuery: {"newField": 101}, expectedMatchingIds: [1, 2]},
        reachesPrimary: true,
        reachesOther: true,
    },
};

function getProfilerEntriesForSuccessfulMultiUpdate(db) {
    const profilerFilter = {
        op: 'update',
        ns: `${dbName}.${collName}`,
        // Filters out events recorded because of StaleConfig error.
        ok: {$ne: 0},
    };
    return db.system.profile.find(profilerFilter).toArray();
}

function assertAndGetProfileEntriesIfRequestIsRoutedToCorrectShards(reqConfig, primaryDB, otherDB) {
    const primaryEntries = getProfilerEntriesForSuccessfulMultiUpdate(primaryDB);
    const otherEntries = getProfilerEntriesForSuccessfulMultiUpdate(otherDB);

    if (reqConfig.reachesPrimary) {
        assert.gt(primaryEntries.length, 0, tojson(primaryEntries));
    } else {
        assert.eq(primaryEntries.length, 0, tojson(primaryEntries));
    }

    if (reqConfig.reachesOther) {
        assert.gt(otherEntries.length, 0, tojson(otherEntries));
    } else {
        assert.eq(otherEntries.length, 0, tojson(otherEntries));
    }

    return [primaryEntries, otherEntries];
}

function prepareShardedTimeseriesCollection(collConfig, insertFn) {
    // Ensures that the collection does not exist.
    const coll = testDB.getCollection(collName);
    coll.drop();

    // Creates timeseries collection.
    const tsOptions = {timeField: timeField};
    const hasMetaField = !!collConfig.metaGenerator;
    if (hasMetaField) {
        tsOptions.metaField = metaField;
    }
    assert.commandWorked(testDB.createCollection(collName, {timeseries: tsOptions}));

    // Shards timeseries collection.
    assert.commandWorked(coll.createIndex(collConfig.shardKey));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: collConfig.shardKey,
    }));

    // Inserts initial set of documents.
    const documents = generateDocsForTestCase(collConfig);
    assert.commandWorked(insertFn(coll, documents));

    // Manually splits the data into two chunks.
    assert.commandWorked(mongos.adminCommand(
        {split: `${dbName}.system.buckets.${collName}`, middle: collConfig.splitPoint}));

    // Ensures that currently both chunks reside on the primary shard.
    let counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    const primaryShard = st.getPrimaryShard(dbName);
    assert.eq(2, counts[primaryShard.shardName], counts);

    // Moves one of the chunks into the second shard.
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(mongos.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: collConfig.splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensures that each shard owns one chunk.
    counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    return [coll, documents];
}

function runTest(collConfig, reqConfig, insertFn) {
    jsTestLog(`Running a test with configuration: ${tojson({collConfig, reqConfig})}`);

    // Prepares a sharded timeseries collection.
    const [coll, documents] = prepareShardedTimeseriesCollection(collConfig, insertFn);

    // Resets database profiler to verify that the update request is routed to the correct shards.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    const primaryDB = primaryShard.getDB(dbName);
    const otherDB = otherShard.getDB(dbName);
    for (let shardDB of [primaryDB, otherDB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }

    // Performs updates.
    const updateCommand = {update: coll.getName(), updates: reqConfig.updateList};
    assert.commandWorked(testDB.runCommand(updateCommand));

    // Checks that the query was routed to the correct shards and gets profile entries if so.
    const [primaryEntries, otherEntries] =
        assertAndGetProfileEntriesIfRequestIsRoutedToCorrectShards(reqConfig, primaryDB, otherDB);

    // Ensures that the collection contains only expected documents.
    const matchingPred = reqConfig.expectedUpdates.findQuery;
    const updatedDocIds =
        coll.find(matchingPred, {_id: 1}).sort({_id: 1}).toArray().map(x => x._id);
    const updatedDocs =
        coll.find(matchingPred, {time: 1, hostid: 1, f: 1}).sort({_id: 1}).toArray();

    reqConfig.expectedUpdates.expectedMatchingIds.sort();

    assert.eq(updatedDocIds, reqConfig.expectedUpdates.expectedMatchingIds, `
    Update list: ${tojsononeline(reqConfig.updateList)}
    Input documents:
        Ids: ${tojsononeline(documents.map(x => x._id))}
        Meta: ${tojsononeline(documents.map(x => x[metaField]))}
        Time: ${tojsononeline(documents.map(x => x[timeField]))}
    Remaining ids: ${tojsononeline(updatedDocIds)}
    Remaining docs: ${tojsononeline(updatedDocs)}
    Match query: ${tojsononeline(reqConfig.expectedUpdates.findQuery)}
    Expected remaining ids: ${tojsononeline(reqConfig.expectedUpdates.expectedMatchingIds)}
    Primary shard profiler entries: ${tojson(primaryEntries)}
    Other shard profiler entries: ${tojson(otherEntries)}`);
}

function runOneTestCase(collConfigName, reqConfigName) {
    const collConfig = collectionConfigurations[collConfigName];
    const reqConfig = requestConfigurations[reqConfigName];

    TimeseriesTest.run((insertFn) => {
        runTest(collConfig, reqConfig, insertFn);
    }, testDB);
}

runOneTestCase("metaShardKey", "emptyFilter");
runOneTestCase("metaShardKey", "nonShardKeyFilter");
runOneTestCase("metaShardKey", "metaFilterOneShard");
runOneTestCase("metaShardKey", "metaFilterOneShardWithCaseInsensitiveCollation");
runOneTestCase("metaShardKeyString", "metaFilterOneShardString");
runOneTestCase("metaShardKeyString", "metaFilterTwoShardsStringCaseInsensitive");
runOneTestCase("metaShardKey", "metaFilterTwoShards");

runOneTestCase("metaObjectShardKey", "emptyFilter");
runOneTestCase("metaObjectShardKey", "nonShardKeyFilter");
runOneTestCase("metaObjectShardKey", "metaObjectFilterOneShard");
runOneTestCase("metaObjectShardKey", "metaObjectFilterTwoShards");
runOneTestCase("metaObjectShardKey", "metaSubFieldFilterTwoShards");

runOneTestCase("metaSubFieldShardKey", "emptyFilter");
runOneTestCase("metaSubFieldShardKey", "nonShardKeyFilter");
runOneTestCase("metaSubFieldShardKey", "metaObjectFilterTwoShards");
runOneTestCase("metaSubFieldShardKey", "metaSubFieldFilterOneShard");
runOneTestCase("metaSubFieldShardKey", "metaSubFieldFilterTwoShards");

runOneTestCase("timeShardKey", "nonShardKeyFilter");
runOneTestCase("timeShardKey", "timeFilterOneShard");
runOneTestCase("timeShardKey", "timeFilterTwoShards");

runOneTestCase("metaTimeShardKey", "emptyFilter");
runOneTestCase("metaTimeShardKey", "nonShardKeyFilter");
runOneTestCase("metaTimeShardKey", "metaTimeFilterOneShard");
runOneTestCase("metaTimeShardKey", "metaFilterTwoShards");

runOneTestCase("metaObjectTimeShardKey", "emptyFilter");
runOneTestCase("metaObjectTimeShardKey", "nonShardKeyFilter");
runOneTestCase("metaObjectTimeShardKey", "metaObjectTimeFilterOneShard");
runOneTestCase("metaObjectTimeShardKey", "metaObjectFilterTwoShards");
runOneTestCase("metaObjectTimeShardKey", "metaSubFieldFilterTwoShards");

runOneTestCase("metaSubFieldTimeShardKey", "emptyFilter");
runOneTestCase("metaSubFieldTimeShardKey", "nonShardKeyFilter");
runOneTestCase("metaSubFieldTimeShardKey", "metaSubFieldTimeFilterOneShard");
runOneTestCase("metaSubFieldTimeShardKey", "metaObjectFilterTwoShards");
runOneTestCase("metaSubFieldTimeShardKey", "metaSubFieldFilterTwoShards");

st.stop();
})();
