/**
 * Verifies multi-deletes on sharded timeseries collection. These commands operate on multiple
 * individual measurements by targeting them with their meta and/or time field value.
 *
 * @tags: [
 *   # To avoid multiversion tests
 *   requires_fcv_71,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

Random.setRandomSeed();

const dbName = jsTestName();
const collName = 'sharded_timeseries_delete_multi';
const timeField = 'time';
const metaField = 'hostid';

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

function generateDocsForTestCase(collConfig) {
    const documents = TimeseriesTest.generateHosts(collConfig.nDocs);
    for (let i = 0; i < collConfig.nDocs; i++) {
        documents[i]._id = i;
        if (collConfig.metaGenerator) {
            documents[i][metaField] = collConfig.metaGenerator(i);
        }
        documents[i][timeField] = generateTimeValue(i);
        documents[i].f = i;
    }
    return documents;
}

const collectionConfigurations = {
    // Shard key only on meta field/subfields.
    metaShardKey: {
        nDocs: 4,
        metaGenerator: (id => id),
        shardKey: {[metaField]: 1},
        splitPoint: {meta: 2},
    },
    metaObjectShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField]: 1},
        splitPoint: {meta: {a: 2}},
    },
    metaSubFieldShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField + '.a']: 1},
        splitPoint: {'meta.a': 2},
    },

    // Shard key on time field.
    timeShardKey: {
        nDocs: 4,
        shardKey: {[timeField]: 1},
        splitPoint: {[`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },

    // Shard key on both meta and time field.
    metaTimeShardKey: {
        nDocs: 4,
        metaGenerator: (id => id),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
    metaObjectTimeShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: {a: 2}, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
    metaSubFieldTimeShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField + '.a']: 1, [timeField]: 1},
        splitPoint: {'meta.a': 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
    },
};

const requestConfigurations = {
    // Empty filter leads to broadcasted request.
    emptyFilter: {
        deleteQuery: {},
        remainingDocumentsIds: [],
        reachesPrimary: true,
        reachesOther: true,
    },
    // Non-shard key filter without meta or time field leads to broadcasted request.
    nonShardKeyFilter: {
        deletePredicates: [{f: 0}, {f: 2}],
        remainingDocumentsIds: [1, 3],
        reachesPrimary: true,
        reachesOther: true,
    },
    // This time field filter has the request targeted to the shard0.
    timeFilterOneShard: {
        deletePredicates:
            [{[timeField]: generateTimeValue(0), f: 0}, {[timeField]: generateTimeValue(1), f: 1}],
        remainingDocumentsIds: [2, 3],
        reachesPrimary: true,
        reachesOther: false,
    },
    // This time field filter has the request targeted to both shards.
    timeFilterTwoShards: {
        deletePredicates:
            [{[timeField]: generateTimeValue(1), f: 1}, {[timeField]: generateTimeValue(3), f: 3}],
        remainingDocumentsIds: [0, 2],
        reachesPrimary: true,
        reachesOther: true,
    },
    metaFilterOneShard: {
        deletePredicates: [{[metaField]: 2, f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    // Meta + time filter has the request targeted to shard1.
    metaTimeFilterOneShard: {
        deletePredicates: [{[metaField]: 2, [timeField]: generateTimeValue(2), f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    metaFilterTwoShards: {
        deletePredicates: [{[metaField]: 1, f: 1}, {[metaField]: 2, f: 2}],
        remainingDocumentsIds: [0, 3],
        reachesPrimary: true,
        reachesOther: true,
    },
    metaObjectFilterOneShard: {
        deletePredicates: [{[metaField]: {a: 2}, f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    // Meta object + time filter has the request targeted to shard1.
    metaObjectTimeFilterOneShard: {
        deletePredicates: [{[metaField]: {a: 2}, [timeField]: generateTimeValue(2), f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    metaObjectFilterTwoShards: {
        deletePredicates: [{[metaField]: {a: 1}, f: 1}, {[metaField]: {a: 2}, f: 2}],
        remainingDocumentsIds: [0, 3],
        reachesPrimary: true,
        reachesOther: true,
    },
    metaSubFieldFilterOneShard: {
        deletePredicates: [{[metaField + '.a']: 2, f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    // Meta sub field + time filter has the request targeted to shard1.
    metaSubFieldTimeFilterOneShard: {
        deletePredicates: [{[metaField + '.a']: 2, [timeField]: generateTimeValue(2), f: 2}],
        remainingDocumentsIds: [0, 1, 3],
        reachesPrimary: false,
        reachesOther: true,
    },
    metaSubFieldFilterTwoShards: {
        deletePredicates: [{[metaField + '.a']: 1, f: 1}, {[metaField + '.a']: 2, f: 2}],
        remainingDocumentsIds: [0, 3],
        reachesPrimary: true,
        reachesOther: true,
    },
};

function getProfilerEntriesForSuccessfulMultiDelete(db) {
    const profilerFilter = {
        op: 'remove',
        ns: `${dbName}.${collName}`,
        // Filters out events recorded because of StaleConfig error.
        ok: {$ne: 0},
    };
    return db.system.profile.find(profilerFilter).toArray();
}

function assertAndGetProfileEntriesIfRequestIsRoutedToCorrectShards(reqConfig, primaryDB, otherDB) {
    const primaryEntries = getProfilerEntriesForSuccessfulMultiDelete(primaryDB);
    const otherEntries = getProfilerEntriesForSuccessfulMultiDelete(otherDB);

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

    // Resets database profiler to verify that the delete request is routed to the correct shards.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    const primaryDB = primaryShard.getDB(dbName);
    const otherDB = otherShard.getDB(dbName);
    for (let shardDB of [primaryDB, otherDB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }

    // Performs delete(s).
    if (reqConfig.deleteQuery) {
        assert.commandWorked(coll.deleteMany(reqConfig.deleteQuery));
    } else {
        let bulk;
        let predicates;
        if (reqConfig.unorderedBulkDeletes) {
            bulk = coll.initializeUnorderedBulkOp();
            predicates = reqConfig.unorderedBulkDeletes;
        } else {
            bulk = coll.initializeOrderedBulkOp();
            predicates = reqConfig.orderedBulkDeletes;
        }

        for (let predicate of predicates) {
            bulk.find(predicate).remove();
        }
        assert.commandWorked(bulk.execute());
    }

    // Checks that the query was routed to the correct shards and gets profile entries if so.
    const [primaryEntries, otherEntries] =
        assertAndGetProfileEntriesIfRequestIsRoutedToCorrectShards(reqConfig, primaryDB, otherDB);

    // Ensures that the collection contains only expected documents.
    const remainingIds = coll.find({}, {_id: 1}).sort({_id: 1}).toArray().map(x => x._id);
    const remainingDocs = coll.find({}, {time: 1, hostid: 1, f: 1}).sort({_id: 1}).toArray();

    reqConfig.remainingDocumentsIds.sort();

    assert.eq(remainingIds, reqConfig.remainingDocumentsIds, `
Delete query: ${tojsononeline(reqConfig.deleteQuery)}
Delete predicates: ${tojsononeline(reqConfig.deletePredicates)}
Input documents:
    Ids: ${tojsononeline(documents.map(x => x._id))}
    Meta: ${tojsononeline(documents.map(x => x[metaField]))}
    Time: ${tojsononeline(documents.map(x => x[timeField]))}
Remaining ids: ${tojsononeline(remainingIds)}
Remaining docs: ${tojsononeline(remainingDocs)}
Expected remaining ids: ${tojsononeline(reqConfig.remainingDocumentsIds)}
Primary shard profiler entries: ${tojson(primaryEntries)}
Other shard profiler entries: ${tojson(otherEntries)}`);
}

function runOneTestCase(collConfigName, reqConfigName) {
    const collConfig = collectionConfigurations[collConfigName];
    const reqConfig = requestConfigurations[reqConfigName];

    // If there's a query in the 'deleteQuery', then we don't test bulk operations.
    if (reqConfig.deleteQuery) {
        TimeseriesTest.run((insertFn) => {
            runTest(collConfig, reqConfig, insertFn);
        }, testDB);
        return;
    }

    // Tests multiple deletes sent through unordered bulk interface.
    reqConfig.unorderedBulkDeletes = reqConfig.deletePredicates;
    TimeseriesTest.run((insertFn) => {
        runTest(collConfig, reqConfig, insertFn);
    }, testDB);
    delete reqConfig.unorderedBulkDeletes;

    // Tests multiple deletes sent through ordered bulk interface.
    reqConfig.orderedBulkDeletes = reqConfig.deletePredicates;
    TimeseriesTest.run((insertFn) => {
        runTest(collConfig, reqConfig, insertFn);
    }, testDB);
    delete reqConfig.orderedBulkDeletes;
}

runOneTestCase("metaShardKey", "emptyFilter");
runOneTestCase("metaShardKey", "nonShardKeyFilter");
runOneTestCase("metaShardKey", "metaFilterOneShard");
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

runOneTestCase("timeShardKey", "emptyFilter");
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
