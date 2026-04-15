// Helper functions for testing time-series collections.

import {documentEq} from "jstests/aggregation/extras/utils.js";
import {
    runningWithViewlessTimeseriesUpgradeDowngrade,
    isShardedTimeseries,
    isViewlessTimeseriesOnlySuite,
    runTimeseriesChunkCommand,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {isStableFCVSuite} from "jstests/libs/feature_compatibility_version.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
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
    numBucketReopeningsFailedDueToWriteConflict: 0,
});

export var TimeseriesTest = class {
    static verifyAndDropIndex(coll, shouldHaveOriginalSpec, indexName) {
        const checkIndexSpec = function (spec, userIndex) {
            assert(spec.hasOwnProperty("v"), indexName);
            assert(spec.hasOwnProperty("name"), indexName);
            assert(spec.hasOwnProperty("key"), indexName);

            if (userIndex) {
                assert(!spec.hasOwnProperty("originalSpec"));
                return;
            }

            if (shouldHaveOriginalSpec) {
                assert(spec.hasOwnProperty("originalSpec"), indexName);
                assert.eq(spec.v, spec.originalSpec.v, indexName);
                assert.eq(spec.name, spec.originalSpec.name, indexName);
                assert.neq(spec.key, spec.originalSpec.key, indexName);
                assert.eq(spec.collation, spec.originalSpec.collation, indexName);
            } else {
                assert(!spec.hasOwnProperty("originalSpec"), indexName);
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

        let bucketIndexes = getTimeseriesCollForRawOps(db, coll).getIndexes(getRawOperationSpec(db));
        for (const index of bucketIndexes) {
            if (index.name === indexName) {
                sawIndex = true;
                checkIndexSpec(index, /*userIndex=*/ false);
            }
        }

        assert(sawIndex, `Index with name: ${indexName} is missing: ${tojson({userIndexes, bucketIndexes})}`);

        assert.commandWorked(coll.dropIndexes(indexName));
    }

    static insertManyDocs(coll) {
        jsTestLog("Inserting documents to a bucket.");
        coll.insertMany(
            [...Array(10).keys()].map((i) => ({
                "metadata": {"sensorId": 1, "type": "temperature"},
                "timestamp": ISODate(),
                "temp": i,
            })),
            {ordered: false},
        );
    }
    static getBucketMaxSpanSecondsFromGranularity(granularity) {
        switch (granularity) {
            case "seconds":
                return 60 * 60;
            case "minutes":
                return 60 * 60 * 24;
            case "hours":
                return 60 * 60 * 24 * 30;
            default:
                assert(false, "Invalid granularity: " + granularity);
        }
    }

    static getBucketRoundingSecondsFromGranularity(granularity) {
        switch (granularity) {
            case "seconds":
                return 60;
            case "minutes":
                return 60 * 60;
            case "hours":
                return 60 * 60 * 24;
            default:
                assert(false, "Invalid granularity: " + granularity);
        }
    }

    static bucketsMayHaveMixedSchemaData(coll) {
        const metadata = coll.getMetadata();
        const tsMixedSchemaOptionNewFormat =
            metadata.options.storageEngine &&
            metadata.options.storageEngine.wiredTiger &&
            metadata.options.storageEngine.wiredTiger.configString;
        return tsMixedSchemaOptionNewFormat == "app_metadata=(timeseriesBucketsMayHaveMixedSchemaData=true)";
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
            "TimeseriesTest.decompressBucket() should only be called on a bucket document",
        );
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
            ["ap-northeast-1a", "ap-northeast-1c"],
            ["ap-southeast-1a", "ap-southeast-1b"],
            ["ap-southeast-2a", "ap-southeast-2b"],
            ["eu-central-1a", "eu-central-1b"],
            ["eu-west-1a", "eu-west-1b", "eu-west-1c"],
            ["sa-east-1a", "sa-east-1b", "sa-east-1c"],
            ["us-east-1a", "us-east-1b", "us-east-1c", "us-east-1e"],
            ["us-west-1a", "us-west-1b"],
            ["us-west-2a", "us-west-2b", "us-west-2c"],
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
                    os: TimeseriesTest.getRandomElem(["Ubuntu15.10", "Ubuntu16.10", "Ubuntu16.04LTS"]),
                    rack: Random.randInt(100).toString(),
                    region: regions[regionIndex],
                    service: Random.randInt(20).toString(),
                    service_environment: TimeseriesTest.getRandomElem(["production", "staging", "test"]),
                    service_version: Random.randInt(2).toString(),
                    team: TimeseriesTest.getRandomElem(["CHI", "LON", "NYC", "SF"]),
                },
            };
        }

        return hosts;
    }

    /**
     * Runs the provided test with both ordered and unordered inserts.
     */
    static run(testFn, theDb) {
        const insert = function (ordered) {
            jsTestLog("Running test with {ordered: " + ordered + "} inserts");
            return function (coll, docs, options) {
                return coll.insert(docs, Object.extend({ordered: ordered}, options));
            };
        };

        testFn(insert(true));
        testFn(insert(false));
    }

    /**
     * Ensures the given collection is distributed on multiple shards by calling moveRange on @coll with @splitPointDate
     * @param {*} coll The collection to distribute
     * @param {*} splitPointDate The splitpoint to use to split the data
     */
    static ensureDataIsDistributedIfSharded(coll, splitPointDate) {
        if (!isShardedTimeseries(coll)) {
            return;
        }

        const db = coll.getDB();
        const getDataBearingShards = () => {
            // TODO SERVER-120014: Remove once 9.0 becomes last LTS and all timeseries collections are viewless.
            if (!isViewlessTimeseriesOnlySuite(db)) {
                return coll
                    .aggregate([{"$collStats": {storageStats: {}}}, {$project: {shard: 1}}, {$sort: {shard: 1}}])
                    .toArray()
                    .map((doc) => doc.shard);
            }

            const shardedDataDistribution = db
                .getSiblingDB("admin")
                .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: coll.getFullName()}}])
                .toArray();

            assert.eq(shardedDataDistribution.length, 1);
            return shardedDataDistribution[0].shards.map((shard) => shard.shardName);
        };

        assert.soon(() => {
            const allShards = db.adminCommand({listShards: 1}).shards.map((shard) => shard._id);
            const currentShards = getDataBearingShards();

            if (documentEq(allShards, currentShards)) {
                return true;
            }

            const otherShard = (() => {
                for (let i in allShards) {
                    if (!currentShards.includes(allShards[i])) {
                        return allShards[i];
                    }
                }
                return undefined;
            })();
            if (!otherShard) {
                return false;
            }

            const timeFieldName = `control.min.${db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.timeField}`;
            try {
                assert.commandWorked(
                    runTimeseriesChunkCommand(db, {
                        moveRange: coll.getFullName(),
                        min: {[timeFieldName]: splitPointDate},
                        max: {[timeFieldName]: MaxKey},
                        toShard: otherShard,
                        waitForDelete: true,
                    }),
                );
            } catch (error) {
                const acceptedErrors = [
                    // If there is an active chunk moving operation, this move range will fail
                    ErrorCodes.ConflictingOperationInProgress,
                ];
                if (TestData.shardsAddedRemoved) {
                    // If this is a suite that adds and removes shards, it's acceptable to have a shard not found as we might had removed the target given shard already
                    acceptedErrors.push(ErrorCodes.ShardNotFound);
                    // If this is a suite that adds and removes shards, it's acceptable to have an operation failed as we might selected a target shard that is actively draining (preparing for remove)
                    acceptedErrors.push(ErrorCodes.OperationFailed);
                }
                assert.commandFailedWithCode(error, acceptedErrors);
                return false;
            }

            if (!TestData.runningWithBalancer) {
                const updatedShards = getDataBearingShards();
                assert.eq(updatedShards.length, currentShards.length + 1);
            }

            return true;
        });
    }

    static assertInsertWorked(res) {
        // TODO (SERVER-85548): Remove helper and revert to assert.commandWorked, no expected error
        // codes
        return assert.commandWorkedOrFailedWithCode(res, [8555700, 8555701]);
    }

    static isBucketCompressed(version) {
        return (
            version == TimeseriesTest.BucketVersion.kCompressedSorted ||
            version == TimeseriesTest.BucketVersion.kCompressedUnsorted
        );
    }

    // Timeseries stats are not returned if there are no timeseries collection. This is a helper to
    // handle that case when tests drop their timeseries collections.
    static getStat(stats, name) {
        if (stats.hasOwnProperty(name)) return stats[name];
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
            getTimeseriesCollForRawOps(db, coll).find().rawData().hint(indexName).toArray().length,
        );
        assert.eq(numDocsExpected, coll.find().hint(indexName).toArray().length);

        // Tests that hint() cannot be used when the index is hidden.
        assert.commandWorked(coll.hideIndex(indexName));
        assert.commandFailedWithCode(
            assert.throws(() => getTimeseriesCollForRawOps(db, coll).find().rawData().hint(indexName).toArray()),
            ErrorCodes.BadValue,
        );
        assert.commandFailedWithCode(
            assert.throws(() => coll.find().hint(indexName).toArray()),
            ErrorCodes.BadValue,
        );

        // Unhide the index and make sure that it can be used again.
        assert.commandWorked(coll.unhideIndex(indexName));
        assert.eq(
            numDocsExpected,
            getTimeseriesCollForRawOps(db, coll).find().rawData().hint(indexName).toArray().length,
        );
        assert.eq(numDocsExpected, coll.find().hint(indexName).toArray().length);
    }

    static checkIndex(coll, userKeyPattern, bucketsKeyPattern, numDocsExpected) {
        const db = coll.getDB();

        const expectedName = Object.entries(userKeyPattern)
            .map(([key, value]) => `${key}_${value}`)
            .join("_");

        // Check definition on user-visible index
        const userIndexes = Object.fromEntries(coll.getIndexes().map((idx) => [idx.name, idx]));
        assert.contains(expectedName, Object.keys(userIndexes));
        assert.eq(expectedName, userIndexes[expectedName].name);
        assert.eq(userKeyPattern, userIndexes[expectedName].key);

        // Check definition on raw index over the bucket documents
        const bucketIndexes = Object.fromEntries(
            getTimeseriesCollForRawOps(db, coll)
                .getIndexes(getRawOperationSpec(db))
                .map((idx) => [idx.name, idx]),
        );
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
            assert.eq(
                bucketReopeningsFailedCounters[key],
                0,
                `Unexpected key ${key} passed to TimeseriesTest.checkBucketReopeningsFailedCounters`,
            );
        }
        for (const key in bucketReopeningsFailedCounters) {
            const value = TimeseriesTest.getStat(stats, key);
            const expectedValue = TimeseriesTest.getStat(expected, key);
            assert.eq(
                expectedValue,
                value,
                `TimeseriesTest.checkBucketReopeningsFailedCounters found value ${
                    value
                } for key ${key} where ${expectedValue} was expected.`,
            );
        }
    }

    /**
     * Returns true if we are running in a suite that is not interfering with
     * Timeseries buckets layout. Meaning that the suites is not performing any background operations
     * that could cause buckets closure.
     * For instance, data migrations operations performed in suites with random balancing can cause
     * closure of open buckets and subsequent writes to end-up in a newly created buckets.
     */
    static canAssumeCanonicalTimeseriesBucketsLayout() {
        if (TestData.runningWithBalancer) {
            // If we are running with moveCollection in the background, we may run into the issue
            // described by SERVER-89349 which can result in more bucket documents than needed.
            return false;
        }

        // TODO(SERVER-100328): remove after 9.0 is branched.
        if (
            TestData.isRunningFCVUpgradeDowngradeSuite &&
            TestData.sessionOptions &&
            TestData.sessionOptions.retryWrites
        ) {
            // FCV upgrade/downgrade + retriable writes can cause more buckets to be created or re-opened,
            // (even in suites that do not perform viewful <-> viewless timeseries conversions),
            // due to the issue described by SERVER-119937.
            return false;
        }

        // TODO(SERVER-101609): remove once 9.0 becomes last LTS.
        if (runningWithViewlessTimeseriesUpgradeDowngrade(db)) {
            // FCV transitions between viewful and viewless timeseries can cause candidate buckets to not be re-opened,
            // or cause retries that produce extra buckets, due to the issues described by SERVER-119937 and SERVER-122949.
            return false;
        }

        return true;
    }

    /**
     * In suites running moveCollection in the background, it is possible to hit the issue
     * described by SERVER-89349 which will result in more bucket documents being created.
     * Creating an index on the time field allows the buckets to be reopened, allowing the
     * counts in this test to be accurate.
     */
    static createTimeFieldIndexToAllowBucketsReopening(coll) {
        if (this.canAssumeCanonicalTimeseriesBucketsLayout()) {
            return;
        }

        const metadata = coll.getMetadata();
        if (!metadata.options.timeseries || metadata.options.timeseries.metaField) {
            return;
        }

        const timeField = metadata.options.timeseries.timeField;
        jsTest.log(`Creating index on '${coll.getName()}' to allow bucket reopening`);
        assert.commandWorked(coll.createIndex({[timeField]: 1}));
    }

    /**
     * Checks for log entries generated by failed document validation.
     * @param {DBCollection}
     * @param {object} record
     * @param {number|number[]} errorIds
     */
    static checkForDocumentValidationFailureLog(coll, record, errorIds = [6698300, 11634800]) {
        // To avoid making log checks too strict, either the buckets namespace or view namespace is acceptable in the log message.
        // Due to differences in EJSON format, only a subset of record data is checked in the log message.
        const oidStr = JSON.parse(toJsonForLog(record))._id["$oid"];
        const db = coll.getDB();
        const conn = coll.getMongo();
        const relaxMatch = true;
        const attrsMatcherBuckets = {
            namespace: `${db.getName()}.system.buckets.${coll.getName()}`,
            record: {
                "_id": {
                    "$oid": oidStr,
                },
            },
        };
        const attrsMatcherView = {
            namespace: `${db.getName()}.${coll.getName()}`,
            record: {
                "_id": {
                    "$oid": oidStr,
                },
            },
        };

        const errorIdList = Array.isArray(errorIds) ? errorIds : [errorIds];
        assert.soon(function () {
            return errorIdList.some(
                (errorId) =>
                    checkLog.checkContainsWithCountJson(conn, errorId, attrsMatcherBuckets, 1, null, relaxMatch) ||
                    checkLog.checkContainsWithCountJson(conn, errorId, attrsMatcherView, 1, null, relaxMatch),
            );
        }, `Could not find log entries containing any of the following ids: ${errorIdList}, and attrs: ${attrsMatcherBuckets} or attrs: ${attrsMatcherView}`);
    }
};

TimeseriesTest.BucketVersion = {
    kUncompressed: 1,
    kCompressedSorted: 2,
    kCompressedUnsorted: 3,
};
