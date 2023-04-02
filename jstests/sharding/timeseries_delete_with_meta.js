/**
 * Test deletes from sharded timeseries collection. These commands operate on the full bucket
 * document by targeting them with their meta field value.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'hostid';

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

// Sanity checks.
if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

const deletesEnabled = TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(st.shard0);
if (!deletesEnabled) {
    jsTestLog(
        "Sharded time-series updates and deletes feature flag is disabled, expecting all delete commands to fail.");
}

// Databases.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const mainDB = mongos.getDB(dbName);

function generateTimeValue(index) {
    return ISODate(`${2000 + index}-01-01`);
}

function generateDocsForTestCase(collConfig) {
    const documents = TimeseriesTest.generateHosts(collConfig.nDocs);
    for (let i = 0; i < collConfig.nDocs; i++) {
        documents[i]._id = i;
        if (collConfig.metaGenerator) {
            documents[i][metaField] = collConfig.metaGenerator(i);
        }
        documents[i][timeField] = generateTimeValue(i);
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
        splitPoint: {[`control.min.${timeField}`]: generateTimeValue(2)},
    },

    // Shard key on both meta and time field.
    metaTimeShardKey: {
        nDocs: 4,
        metaGenerator: (id => id),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: 2, [`control.min.${timeField}`]: generateTimeValue(2)},
    },
    metaObjectTimeShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField]: 1, [timeField]: 1},
        splitPoint: {meta: {a: 2}, [`control.min.${timeField}`]: generateTimeValue(2)},
    },
    metaSubFieldTimeShardKey: {
        nDocs: 4,
        metaGenerator: (index => ({a: index})),
        shardKey: {[metaField + '.a']: 1, [timeField]: 1},
        splitPoint: {'meta.a': 1, [`control.min.${timeField}`]: generateTimeValue(2)},
    },
};

const requestConfigurations = {
    emptyFilter: {
        deleteQuery: {},
        remainingDocumentsIds: [],
        reachesShard0: true,
        reachesShard1: true,
    },
    metaFilterOneShard: {
        deletePredicates: [{[metaField]: 2}, {[metaField]: 3}],
        remainingDocumentsIds: [0, 1],
        reachesShard0: false,
        reachesShard1: true,
    },
    metaFilterTwoShards: {
        deletePredicates: [{[metaField]: 1}, {[metaField]: 2}],
        remainingDocumentsIds: [0, 3],
        reachesShard0: true,
        reachesShard1: true,
    },
    metaObjectFilterOneShard: {
        deletePredicates: [{[metaField]: {a: 2}}, {[metaField]: {a: 3}}],
        remainingDocumentsIds: [0, 1],
        reachesShard0: false,
        reachesShard1: true,
    },
    metaObjectFilterTwoShards: {
        deletePredicates: [{[metaField]: {a: 1}}, {[metaField]: {a: 2}}],
        remainingDocumentsIds: [0, 3],
        reachesShard0: true,
        reachesShard1: true,
    },
    metaSubFieldFilterOneShard: {
        deletePredicates: [{[metaField + '.a']: 2}, {[metaField + '.a']: 3}],
        remainingDocumentsIds: [0, 1],
        reachesShard0: false,
        reachesShard1: true,
    },
    metaSubFieldFilterTwoShards: {
        deletePredicates: [{[metaField + '.a']: 1}, {[metaField + '.a']: 2}],
        remainingDocumentsIds: [0, 3],
        reachesShard0: true,
        reachesShard1: true,
    },
};

const testCases = {
    // Shard key only on meta field/subfields.
    metaShardKey: ['emptyFilter', 'metaFilterOneShard', 'metaFilterTwoShards'],
    metaObjectShardKey: [
        'emptyFilter',
        'metaObjectFilterOneShard',
        'metaObjectFilterTwoShards',
        'metaSubFieldFilterTwoShards'
    ],
    metaSubFieldShardKey: [
        'emptyFilter',
        'metaObjectFilterTwoShards',
        'metaSubFieldFilterOneShard',
        'metaSubFieldFilterTwoShards'
    ],

    // Shard key on time field.
    timeShardKey: ['emptyFilter'],

    // Shard key on both meta and time field.
    metaTimeShardKey: ['emptyFilter', 'metaFilterTwoShards'],
    metaObjectTimeShardKey:
        ['emptyFilter', 'metaObjectFilterTwoShards', 'metaSubFieldFilterTwoShards'],
    metaSubFieldTimeShardKey:
        ['emptyFilter', 'metaObjectFilterTwoShards', 'metaSubFieldFilterTwoShards'],
};

function runTest(collConfig, reqConfig, insert) {
    jsTestLog(`Running a test with configuration: ${tojson({collConfig, reqConfig})}`);

    // Ensure that the collection does not exist.
    const coll = mainDB.getCollection(collName);
    coll.drop();

    // Create timeseries collection.
    const tsOptions = {timeField: timeField};
    const hasMetaField = !!collConfig.metaGenerator;
    if (hasMetaField) {
        tsOptions.metaField = metaField;
    }
    assert.commandWorked(mainDB.createCollection(collName, {timeseries: tsOptions}));

    // Shard timeseries collection.
    assert.commandWorked(coll.createIndex(collConfig.shardKey));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: collConfig.shardKey,
    }));

    // Insert initial set of documents.
    const documents = generateDocsForTestCase(collConfig);
    assert.commandWorked(insert(coll, documents));

    // Manually split the data into two chunks.
    assert.commandWorked(mongos.adminCommand(
        {split: `${dbName}.system.buckets.${collName}`, middle: collConfig.splitPoint}));

    // Ensure that currently both chunks reside on the primary shard.
    let counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    const primaryShard = st.getPrimaryShard(dbName);
    assert.eq(2, counts[primaryShard.shardName], counts);

    // Move one of the chunks into the second shard.
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(mongos.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: collConfig.splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensure that each shard owns one chunk.
    counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    const isBulkOperation = !reqConfig.deleteQuery;
    if (!isBulkOperation) {
        // If sharded updates and deletes feature flag is disabled, we only test that the delete
        // command fails.
        if (!deletesEnabled) {
            assert.throwsWithCode(() => coll.deleteMany(reqConfig.deleteQuery),
                                  ErrorCodes.NotImplemented);
            return;
        }

        // The 'isTimeseriesNamespace' parameter is not allowed on mongos.
        const failingDeleteCommand = {
            delete: `system.buckets.${collName}`,
            deletes: [
                {
                    q: reqConfig.deleteQuery,
                    limit: 0,
                },
            ],
            isTimeseriesNamespace: true,
        };
        assert.commandFailedWithCode(mainDB.runCommand(failingDeleteCommand), 5916401);

        // On a mongod node, 'isTimeseriesNamespace' can only be used on time-series buckets
        // namespace.
        failingDeleteCommand.delete = collName;
        assert.commandFailedWithCode(st.shard0.getDB(dbName).runCommand(failingDeleteCommand),
                                     5916400);

        // TODO (SERVER-75379): Remove these tests.
        if (!FeatureFlagUtil.isPresentAndEnabled(mainDB, "TimeseriesDeletesSupport")) {
            delete failingDeleteCommand.isTimeseriesNamespace;
            for (let additionalField of [timeField, 'randomFieldWhichShouldNotBeHere']) {
                // JavaScript does not have a reliable way to perform deep copy of an object. So
                // instead of copying delete query each time, we just set and unset additional
                // fields in it. See https://stackoverflow.com/a/122704 for details.
                failingDeleteCommand.deletes[0].q[additionalField] = 1;

                // Currently, we do not support queries on non-meta fields for delete commands.
                assert.commandFailedWithCode(mainDB.runCommand(failingDeleteCommand),
                                             ErrorCodes.InvalidOptions);
                delete failingDeleteCommand.deletes[0].q[additionalField];
            }

            // Currently, we support only delete commands with 'limit: 0' for sharded time-series
            // collections.
            failingDeleteCommand.deletes[0].limit = 1;
            assert.commandFailedWithCode(mainDB.runCommand(failingDeleteCommand),
                                         ErrorCodes.IllegalOperation);
        }
    }

    // Reset database profiler.
    const primaryDB = primaryShard.getDB(dbName);
    const otherDB = otherShard.getDB(dbName);
    for (let shardDB of [primaryDB, otherDB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }

    // Perform valid delete.
    if (!isBulkOperation) {
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
        if (deletesEnabled) {
            assert.commandWorked(bulk.execute());
        } else {
            assert.throws(() => bulk.execute());
            return;
        }
    }

    // Check that the query was routed to the correct shards.
    const profilerFilter = {
        op: 'remove',
        ns: `${dbName}.${collName}`,
        // Filter out events recorded because of StaleConfig error.
        ok: {$ne: 0},
    };
    const shard0Entries = primaryDB.system.profile.find(profilerFilter).itcount();
    const shard1Entries = otherDB.system.profile.find(profilerFilter).itcount();
    if (reqConfig.reachesShard0) {
        assert.gt(shard0Entries, 0);
    } else {
        assert.eq(shard0Entries, 0);
    }
    if (reqConfig.reachesShard1) {
        assert.gt(shard1Entries, 0);
    } else {
        assert.eq(shard1Entries, 0);
    }

    // Ensure that the collection contains only expected documents.
    const remainingIds = coll.find({}, {_id: 1}).sort({_id: 1}).toArray().map(x => x._id);

    reqConfig.remainingDocumentsIds.sort();

    assert.eq(remainingIds, reqConfig.remainingDocumentsIds, `
    Delete query: ${tojsononeline(reqConfig.deleteQuery)}
    Input documents:
        Ids: ${tojsononeline(documents.map(x => x._id))}
        Meta: ${tojsononeline(documents.map(x => x[metaField]))}
        Time: ${tojsononeline(documents.map(x => x[timeField]))}
    Remaining ids: ${tojsononeline(remainingIds)}
    Expected remaining ids: ${tojsononeline(reqConfig.remainingDocumentsIds)}
    `);
}

TimeseriesTest.run((insert) => {
    for (const [collConfigName, reqConfigNames] of Object.entries(testCases)) {
        const collConfig = collectionConfigurations[collConfigName];
        for (let reqConfigName of reqConfigNames) {
            const reqConfig = requestConfigurations[reqConfigName];

            try {
                // Some request configurations do not support bulk operations.
                if (reqConfig.deleteQuery) {
                    runTest(collConfig, reqConfig, insert);
                    continue;
                }

                const deletePredicates = reqConfig.deletePredicates;

                // Test single 'coll.deleteMany()' call with $or predicate.
                reqConfig.deleteQuery = {$or: deletePredicates};
                runTest(collConfig, reqConfig, insert);
                delete reqConfig.deleteQuery;

                // Test multiple deletes sent through unordered bulk interface.
                reqConfig.unorderedBulkDeletes = deletePredicates;
                runTest(collConfig, reqConfig, insert);
                delete reqConfig.unorderedBulkDeletes;

                // Test multiple deletes sent through ordered bulk interface.
                reqConfig.orderedBulkDeletes = deletePredicates;
                runTest(collConfig, reqConfig, insert);
                delete reqConfig.orderedBulkDeletes;
            } catch (e) {
                jsTestLog(`Test case failed. Configurations:
                - Collection  "${collConfigName}" = ${tojson(collConfig)}
                - Request "${reqConfigName}" =  ${tojson(reqConfig)}
                `);
                throw e;
            }
        }
    }
}, mainDB);

st.stop();
})();
