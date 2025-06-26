// Helper functions for testing time-series collections.

import {documentEq} from "jstests/aggregation/extras/utils.js";
import {
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps,
    isShardedTimeseries,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

/**
 * Read-only object with each of the numBucketReopeningsFailed* counters set to 0.
 */
const bucketReopeningsFailedCounters = Object.freeze({
    numBucketReopeningsFailedDueToEraMismatch: 0,
    numBucketReopeningsFailedDueToMalformedIdField: 0,
    numBucketReopeningsFailedDueToHashCollision: 0,
    numBucketReopeningsFailedDueToMarkedFrozen: 0,
    numBucketReopeningsFailedDueToValidator: 0,
    numBucketReopeningsFailedDueToMarkedClosed: 0,
    numBucketReopeningsFailedDueToMinMaxCalculation: 0,
    numBucketReopeningsFailedDueToSchemaGeneration: 0,
    numBucketReopeningsFailedDueToUncompressedTimeColumn: 0,
    numBucketReopeningsFailedDueToCompressionFailure: 0,
    numBucketReopeningsFailedDueToWriteConflict: 0
});

export var TimeseriesTest = class {
    static verifyAndDropIndex(coll, shouldHaveOriginalSpec, indexName) {
        const checkIndexSpec = function(spec, userIndex) {
            assert(spec.hasOwnProperty("v"));
            assert(spec.hasOwnProperty("name"));
            assert(spec.hasOwnProperty("key"));

            if (userIndex) {
                assert(!spec.hasOwnProperty("originalSpec"));
                return;
            }

            if (shouldHaveOriginalSpec) {
                assert(spec.hasOwnProperty("originalSpec"));
                assert.eq(spec.v, spec.originalSpec.v);
                assert.eq(spec.name, spec.originalSpec.name);
                assert.neq(spec.key, spec.originalSpec.key);
                assert.eq(spec.collation, spec.originalSpec.collation);
            } else {
                assert(!spec.hasOwnProperty("originalSpec"));
            }
        };
        let sawIndex = false;

        let userIndexes = coll.getIndexes();
        for (const index of userIndexes) {
            if (index.name === indexName) {
                sawIndex = true;
                checkIndexSpec(index, /*userIndex=*/ true);
            }
        }

        let bucketIndexes =
            getTimeseriesCollForRawOps(db, coll).getIndexes(getRawOperationSpec(db));
        for (const index of bucketIndexes) {
            if (index.name === indexName) {
                sawIndex = true;
                checkIndexSpec(index, /*userIndex=*/ false);
            }
        }

        assert(sawIndex,
               `Index with name: ${indexName} is missing: ${tojson({userIndexes, bucketIndexes})}`);

        assert.commandWorked(coll.dropIndexes(indexName));
    }

    static insertManyDocs(coll) {
        jsTestLog("Inserting documents to a bucket.");
        coll.insertMany(
            [...Array(10).keys()].map(i => ({
                                          "metadata": {"sensorId": 1, "type": "temperature"},
                                          "timestamp": ISODate(),
                                          "temp": i
                                      })),
            {ordered: false});
    }
    static getBucketMaxSpanSecondsFromGranularity(granularity) {
        switch (granularity) {
            case 'seconds':
                return 60 * 60;
            case 'minutes':
                return 60 * 60 * 24;
            case 'hours':
                return 60 * 60 * 24 * 30;
            default:
                assert(false, 'Invalid granularity: ' + granularity);
        }
    }

    static getBucketRoundingSecondsFromGranularity(granularity) {
        switch (granularity) {
            case 'seconds':
                return 60;
            case 'minutes':
                return 60 * 60;
            case 'hours':
                return 60 * 60 * 24;
            default:
                assert(false, 'Invalid granularity: ' + granularity);
        }
    }

    static bucketsMayHaveMixedSchemaData(coll) {
        const catalog = getTimeseriesCollForDDLOps(coll.getDB(), coll)
                            .aggregate([{$listCatalog: {}}])
                            .toArray()[0];
        const tsMixedSchemaOptionNewFormat = catalog.md.options.storageEngine &&
            catalog.md.options.storageEngine.wiredTiger &&
            catalog.md.options.storageEngine.wiredTiger.configString;
        // TODO SERVER-92533 Simplify once SERVER-91195 is backported to all supported branches
        if (tsMixedSchemaOptionNewFormat !== undefined) {
            return tsMixedSchemaOptionNewFormat ==
                "app_metadata=(timeseriesBucketsMayHaveMixedSchemaData=true)";
        } else {
            return catalog.md.timeseriesBucketsMayHaveMixedSchemaData;
        }
    }

    // TODO SERVER-68058 remove this helper.
    static arbitraryUpdatesEnabled(conn) {
        return FeatureFlagUtil.isPresentAndEnabled(conn, "TimeseriesUpdatesSupport");
    }

    /**
     * Adjusts the values in 'fields' by a random amount.
     * Ensures that the new values stay in the range [0, 100].
     */
    static updateUsages(fields) {
        for (const field in fields) {
            fields[field] += Math.round(Random.genNormal(0, 1));
            fields[field] = Math.max(fields[field], 0);
            fields[field] = Math.min(fields[field], 100);
        }
    }

    /**
     * Decompresses a compressed bucket document. Replaces the compressed data in-place.
     */
    static decompressBucket(compressedBucket) {
        assert.hasFields(
            compressedBucket,
            ["control"],
            "TimeseriesTest.decompressBucket() should only be called on a bucket document");
        if (compressedBucket.control.version == 1) {
            // Bucket is already decompressed.
            return;
        }

        for (const column in compressedBucket.data) {
            compressedBucket.data[column] = decompressBSONColumn(compressedBucket.data[column]);
        }

        // The control object should reflect that the data is uncompressed.
        compressedBucket.control.version = TimeseriesTest.BucketVersion.kUncompressed;
        delete compressedBucket.control.count;
    }

    /**
     * Returns a random element from an array.
     */
    static getRandomElem(arr) {
        return arr[Random.randInt(arr.length)];
    }

    static getRandomUsage() {
        return Random.randInt(101);
    }

    /**
     * Generates time-series data based on the TSBS document-per-event format.
     *
     * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/cpu.go
     * https://github.com/timescale/tsbs/blob/7508b34755e05f55a14ec4bac2913ae758b4fd78/cmd/tsbs_generate_data/devops/host.go
     */
    static generateHosts(numHosts) {
        const hosts = new Array(numHosts);

        const regions = [
            "ap-northeast-1",
            "ap-southeast-1",
            "ap-southeast-2",
            "eu-central-1",
            "eu-west-1",
            "sa-east-1",
            "us-east-1",
            "us-west-1",
            "us-west-2",
        ];

        const dataCenters = [
            [
                "ap-northeast-1a",
                "ap-northeast-1c",
            ],
            [
                "ap-southeast-1a",
                "ap-southeast-1b",
            ],
            [
                "ap-southeast-2a",
                "ap-southeast-2b",
            ],
            [
                "eu-central-1a",
                "eu-central-1b",
            ],
            [
                "eu-west-1a",
                "eu-west-1b",
                "eu-west-1c",
            ],
            [
                "sa-east-1a",
                "sa-east-1b",
                "sa-east-1c",
            ],
            [
                "us-east-1a",
                "us-east-1b",
                "us-east-1c",
                "us-east-1e",
            ],
            [
                "us-west-1a",
                "us-west-1b",
            ],
            [
                "us-west-2a",
                "us-west-2b",
                "us-west-2c",
            ],
        ];

        for (let i = 0; i < hosts.length; i++) {
            const regionIndex = Random.randInt(regions.length);
            hosts[i] = {
                fields: {
                    usage_guest: TimeseriesTest.getRandomUsage(),
                    usage_guest_nice: TimeseriesTest.getRandomUsage(),
                    usage_idle: TimeseriesTest.getRandomUsage(),
                    usage_iowait: TimeseriesTest.getRandomUsage(),
                    usage_irq: TimeseriesTest.getRandomUsage(),
                    usage_nice: TimeseriesTest.getRandomUsage(),
                    usage_softirq: TimeseriesTest.getRandomUsage(),
                    usage_steal: TimeseriesTest.getRandomUsage(),
                    usage_system: TimeseriesTest.getRandomUsage(),
                    usage_user: TimeseriesTest.getRandomUsage(),
                },
                tags: {
                    arch: TimeseriesTest.getRandomElem(["x64", "x86"]),
                    datacenter: TimeseriesTest.getRandomElem(dataCenters[regionIndex]),
                    hostname: "host_" + i,
                    hostid: i,
                    os: TimeseriesTest.getRandomElem(
                        ["Ubuntu15.10", "Ubuntu16.10", "Ubuntu16.04LTS"]),
                    rack: Random.randInt(100).toString(),
                    region: regions[regionIndex],
                    service: Random.randInt(20).toString(),
                    service_environment:
                        TimeseriesTest.getRandomElem(["production", "staging", "test"]),
                    service_version: Random.randInt(2).toString(),
                    team: TimeseriesTest.getRandomElem(["CHI", "LON", "NYC", "SF"]),
                }

            };
        }

        return hosts;
    }

    /**
     * Runs the provided test with both ordered and unordered inserts.
     */
    static run(testFn, theDb) {
        const insert = function(ordered) {
            jsTestLog('Running test with {ordered: ' + ordered + '} inserts');
            return function(coll, docs, options) {
                return coll.insert(docs, Object.extend({ordered: ordered}, options));
            };
        };

        testFn(insert(true));
        testFn(insert(false));
    }

    static ensureDataIsDistributedIfSharded(coll, splitPointDate) {
        const db = coll.getDB();
        if (isShardedTimeseries(coll)) {
            const timeFieldName =
                db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.timeField;

            const splitPoint = {[`control.min.${timeFieldName}`]: splitPointDate};
            assert.commandWorked(db.adminCommand(
                {split: getTimeseriesCollForDDLOps(db, coll).getFullName(), middle: splitPoint}));

            const allShards = db.getSiblingDB("config").shards.find().sort({_id: 1}).toArray().map(
                doc => doc._id);
            const currentShards = getTimeseriesCollForDDLOps(db, coll)
                                      .aggregate([
                                          {"$collStats": {storageStats: {}}},
                                          {$project: {shard: 1}},
                                          {$sort: {shard: 1}}
                                      ])
                                      .toArray()
                                      .map(doc => doc.shard);

            if (!documentEq(allShards, currentShards)) {
                let otherShard;
                for (let i in allShards) {
                    if (!currentShards.includes(allShards[i])) {
                        otherShard = allShards[i];
                        break;
                    }
                }
                assert(otherShard);

                assert.commandWorked(db.adminCommand({
                    movechunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
                    find: splitPoint,
                    to: otherShard,
                    _waitForDelete: true
                }));

                const updatedShards = getTimeseriesCollForDDLOps(db, coll)
                                          .aggregate([
                                              {"$collStats": {storageStats: {}}},
                                              {$project: {shard: 1}},
                                              {$sort: {shard: 1}}
                                          ])
                                          .toArray()
                                          .map(doc => doc.shard);
                assert.eq(updatedShards.length, currentShards.length + 1);
            }
        }
    }

    static getBucketsCollName(collName) {
        return getTimeseriesBucketsColl(collName);
    }

    static assertInsertWorked(res) {
        // TODO (SERVER-85548): Remove helper and revert to assert.commandWorked, no expected error
        // codes
        return assert.commandWorkedOrFailedWithCode(res, [8555700, 8555701]);
    }

    static isBucketCompressed(version) {
        return (version == TimeseriesTest.BucketVersion.kCompressedSorted ||
                version == TimeseriesTest.BucketVersion.kCompressedUnsorted);
    }

    // Timeseries stats are not returned if there are no timeseries collection. This is a helper to
    // handle that case when tests drop their timeseries collections.
    static getStat(stats, name) {
        if (stats.hasOwnProperty(name))
            return stats[name];
        return 0;
    }

    // Check that the hint method can be used with the buckets/user index specified by `indexName`.
    // Ensure that this is not possible when the specified index is hidden. Note that the
    // `indexName` parameter is expected to be the same for both the user-visible index, and the raw
    // index over the buckets.
    static checkHint(coll, indexName, numDocsExpected) {
        const db = coll.getDB();

        // Tests hint() using the index name.
        assert.eq(
            numDocsExpected,
            getTimeseriesCollForRawOps(db, coll).find().rawData().hint(indexName).toArray().length);
        assert.eq(numDocsExpected, coll.find().hint(indexName).toArray().length);

        // Tests that hint() cannot be used when the index is hidden.
        assert.commandWorked(coll.hideIndex(indexName));
        assert.commandFailedWithCode(assert.throws(() => getTimeseriesCollForRawOps(db, coll)
                                                             .find()
                                                             .rawData()
                                                             .hint(indexName)
                                                             .toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(indexName).toArray()),
                                                  ErrorCodes.BadValue);

        // Unhide the index and make sure that it can be used again.
        assert.commandWorked(coll.unhideIndex(indexName));
        assert.eq(
            numDocsExpected,
            getTimeseriesCollForRawOps(db, coll).find().rawData().hint(indexName).toArray().length);
        assert.eq(numDocsExpected, coll.find().hint(indexName).toArray().length);
    }

    static checkIndex(coll, userKeyPattern, bucketsKeyPattern, numDocsExpected) {
        const db = coll.getDB();

        const expectedName =
            Object.entries(userKeyPattern).map(([key, value]) => `${key}_${value}`).join('_');

        // Check definition on user-visible index
        const userIndexes = Object.fromEntries(coll.getIndexes().map(idx => [idx.name, idx]));
        assert.contains(expectedName, Object.keys(userIndexes));
        assert.eq(expectedName, userIndexes[expectedName].name);
        assert.eq(userKeyPattern, userIndexes[expectedName].key);

        // Check definition on raw index over the bucket documents
        const bucketIndexes = Object.fromEntries(getTimeseriesCollForRawOps(db, coll)
                                                     .getIndexes(getRawOperationSpec(db))
                                                     .map(idx => [idx.name, idx]));
        assert.contains(expectedName, Object.keys(bucketIndexes));
        assert.eq(expectedName, bucketIndexes[expectedName].name);
        assert.eq(bucketsKeyPattern, bucketIndexes[expectedName].key);

        this.checkHint(coll, expectedName, numDocsExpected);
    }

    /**
     * Checks that for each numBucketReopeningsFailed* key in `expected`, the associated counter in
     * `stats` has the specified value. Each counter that is not specified in `expected` is expected
     * to have the value 0.
     * @param {object} stats
     * @param {Record<keyof typeof bucketReopeningsFailedCounters, number>} expected
     */
    static checkBucketReopeningsFailedCounters(stats, expected) {
        for (const key in expected) {
            assert.eq(bucketReopeningsFailedCounters[key],
                      0,
                      `Unexpected key ${
                          key} passed to TimeseriesTest.checkBucketReopeningsFailedCounters`);
        }
        for (const key in bucketReopeningsFailedCounters) {
            const value = TimeseriesTest.getStat(stats, key);
            const expectedValue = TimeseriesTest.getStat(expected, key);
            assert.eq(expectedValue,
                      value,
                      `TimeseriesTest.checkBucketReopeningsFailedCounters found value ${
                          value} for key ${key} where ${expectedValue} was expected.`);
        }
    }
};

TimeseriesTest.BucketVersion = {
    kUncompressed: 1,
    kCompressedSorted: 2,
    kCompressedUnsorted: 3
};
