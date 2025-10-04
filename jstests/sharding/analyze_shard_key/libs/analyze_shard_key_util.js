/**
 * Utilities for testing the analyzeShardKey command.
 */
export var AnalyzeShardKeyUtil = {
    /**
     * Returns true if the given key pattern contains a hashed key.
     */
    isHashedKeyPattern(keyPattern) {
        for (let fieldName in keyPattern) {
            if (keyPattern[fieldName] == "hashed") {
                return true;
            }
        }
        return false;
    },

    /**
     * Returns true if the given key pattern is {_id: 1}.
     */
    isIdKeyPattern(keyPattern) {
        return bsonWoCompare(keyPattern, {_id: 1}) == 0;
    },

    /**
     * Returns a set of current shard key field names, candidate shard key field names and
     * index key field names combined.
     */
    getCombinedFieldNames(currentShardKey, candidateShardKey, indexKey) {
        const fieldNames = new Set([]);
        for (let fieldName in currentShardKey) {
            fieldNames.add(fieldName);
        }
        for (let fieldName in candidateShardKey) {
            fieldNames.add(fieldName);
        }
        for (let fieldName in indexKey) {
            fieldNames.add(fieldName);
        }
        return fieldNames;
    },

    /**
     * Returns the value for the given field.
     */
    getDottedField(doc, fieldName) {
        let val = doc;
        const fieldNames = fieldName.split(".");
        for (let i = 0; i < fieldNames.length; i++) {
            val = val[fieldNames[i]];
        }
        return val;
    },

    /**
     * Sets the given field to the given value. The field name can be dotted.
     */
    setDottedField(doc, fieldName, val) {
        let obj = doc;
        const fieldNames = fieldName.split(".");
        for (let i = 0; i < fieldNames.length; i++) {
            const fieldName = fieldNames[i];
            if (i == fieldNames.length - 1) {
                obj[fieldName] = val;
                return;
            }
            if (!doc[fieldName]) {
                obj[fieldName] = {};
            }
            obj = obj[fieldName];
        }
    },

    /**
     * Extracts the shard key value from the given document.
     */
    extractShardKeyValueFromDocument(doc, shardKey) {
        const shardKeyValue = {};
        for (let fieldName in shardKey) {
            shardKeyValue[fieldName] = AnalyzeShardKeyUtil.getDottedField(doc, fieldName);
        }
        return shardKeyValue;
    },

    /**
     * Returns a random integer between the given range (inclusive).
     */
    getRandInteger(min, max) {
        return Math.floor(Math.random() * (max - min + 1)) + min;
    },

    /**
     * Returns a random element in the given array.
     */
    getRandomElement(arr) {
        return arr[this.getRandInteger(0, arr.length - 1)];
    },

    /**
     * Returns the field name "<prefix>", "<prefix>.x" or "<prefix>.x.y" with roughly equal
     * probability.
     */
    getRandomFieldName(prefix) {
        const prob = Math.random();
        if (prob < 0.3) {
            return prefix;
        } else if (prob < 0.6) {
            return prefix + ".x";
        }
        return prefix + ".x.y";
    },

    /**
     * Returns true if the collection is a clustered collection. Assumes that the collection
     * exists.
     */
    isClusterCollection(conn, dbName, collName) {
        const listCollectionRes = assert.commandWorked(
            conn.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}),
        );
        return listCollectionRes.cursor.firstBatch[0].options.hasOwnProperty("clusteredIndex");
    },

    /**
     * Enables profiling of the given database on all the given mongods.
     */
    enableProfiler(mongodConns, dbName) {
        mongodConns.forEach((conn) => {
            assert.commandWorked(conn.getDB(dbName).setProfilingLevel(2));
        });
    },

    /**
     * Disables profiling of the given database on all the given mongods.
     */
    disableProfiler(mongodConns, dbName) {
        mongodConns.forEach((conn) => {
            assert.commandWorked(conn.getDB(dbName).setProfilingLevel(0));
        });
    },

    calculatePercentage(part, whole) {
        assert.gte(part, 0);
        assert.gt(whole, 0);
        assert.lte(part, whole);
        return (part * 100.0) / whole;
    },

    /**
     * Returns true if 'objs' contains 'obj'.
     */
    containsBSONObj(objs, obj) {
        for (let tmpObj of objs) {
            if (bsonWoCompare(obj, tmpObj) == 0) {
                return true;
            }
        }
        return false;
    },

    // The analyzeShardKey command rounds the percentages 10 decimal places. The epsilon is chosen
    // to account for that.
    assertApprox(actual, expected, msg, epsilon = 1e-9) {
        return assert.lte(Math.abs(actual - expected), epsilon, {actual, expected, msg});
    },

    /**
     * Asserts that the difference between 'actual' and 'expected' is less than 'maxPercentage' of
     * 'expected'.
     */
    assertDiffPercentage(actual, expected, maxPercentage) {
        const actualPercentage = (Math.abs(actual - expected) * 100) / expected;
        assert.lt(actualPercentage, maxPercentage, tojson({actual, expected, maxPercentage, actualPercentage}));
    },

    validateKeyCharacteristicsMetrics(metrics) {
        assert.gt(metrics.numDocsTotal, 0, metrics);
        assert.gt(metrics.numDocsSampled, 0, metrics);
        assert.gt(metrics.numDistinctValues, 0, metrics);
        assert.gt(metrics.mostCommonValues.length, 0, metrics);
        assert.gt(metrics.avgDocSizeBytes, 0, metrics);

        assert.gte(metrics.numDocsTotal, metrics.numDocsSampled, metrics);
        if (metrics.hasOwnProperty("numOrphanDocs")) {
            assert.gte(metrics.numOrphanDocs, 0, metrics);
        }
        if (metrics.isUnique) {
            assert.eq(metrics.numDocsSampled, metrics.numDistinctValues, metrics);
        } else {
            assert.gte(metrics.numDocsSampled, metrics.numDistinctValues, metrics);
        }
        assert.gte(metrics.numDistinctValues, metrics.mostCommonValues.length, metrics);

        let totalFrequency = 0;
        let prevFrequency = Number.MAX_VALUE;
        for (let {value, frequency} of metrics.mostCommonValues) {
            assert.lte(frequency, prevFrequency, metrics);
            if (metrics.isUnique) {
                assert.eq(frequency, 1, metrics);
            }
            totalFrequency += frequency;
            prevFrequency = frequency;
        }
        assert.gte(metrics.numDocsTotal, totalFrequency, metrics);

        if (metrics.monotonicity.type == "unknown") {
            assert(!metrics.monotonicity.hasOwnProperty("recordIdCorrelationCoefficient"), metrics);
        } else {
            assert(metrics.monotonicity.hasOwnProperty("recordIdCorrelationCoefficient"), metrics);
            const coefficient = metrics.monotonicity.recordIdCorrelationCoefficient;
            assert.gte(Math.abs(coefficient), 0, metrics);
            assert.lte(Math.abs(coefficient), 1, metrics);
        }
    },

    assertNotContainKeyCharacteristicsMetrics(res) {
        assert(!res.hasOwnProperty("keyCharacteristics"), res);
    },

    assertContainKeyCharacteristicsMetrics(res) {
        assert(res.hasOwnProperty("keyCharacteristics"), res);
        const metrics = res.keyCharacteristics;
        assert(metrics.hasOwnProperty("numDocsTotal"), metrics);
        assert(metrics.hasOwnProperty("isUnique"), metrics);
        assert(metrics.hasOwnProperty("numDistinctValues"), metrics);
        assert(metrics.hasOwnProperty("mostCommonValues"), metrics);
        assert(metrics.hasOwnProperty("monotonicity"), metrics);
        assert(metrics.hasOwnProperty("avgDocSizeBytes"), metrics);
        this.validateKeyCharacteristicsMetrics(metrics);
    },

    assertKeyCharacteristicsMetrics(actual, expected) {
        assert.eq(actual.numDocsTotal, expected.numDocs, {actual, expected});
        assert.eq(actual.numDocsSampled, expected.numDocs, {actual, expected});
        assert.eq(actual.isUnique, expected.isUnique, {actual, expected});
        assert.eq(actual.numDistinctValues, expected.numDistinctValues, {actual, expected});

        // Verify the number of most common shard key values returned is less than what
        // 'analyzeShardKeyNumMostCommonValues' is set to.
        assert.gt(actual.mostCommonValues.length, 0);
        assert.lte(actual.mostCommonValues.length, expected.numMostCommonValues, {actual, expected});
        let prevFrequency = Number.MAX_VALUE;
        for (let mostCommonValue of actual.mostCommonValues) {
            // Verify the shard key values are sorted in descending of frequency.
            assert.lte(mostCommonValue.frequency, prevFrequency, {
                mostCommonValue,
                actual: actual.mostCommonValues,
                expected: expected.mostCommonValues,
            });

            // Verify that this shard key value is among the expected ones.
            assert(this.containsBSONObj(expected.mostCommonValues, mostCommonValue), {
                mostCommonValue,
                actual: actual.mostCommonValues,
                expected: expected.mostCommonValues,
            });
            prevFrequency = mostCommonValue.frequency;
        }

        assert(actual.hasOwnProperty("monotonicity"), {actual, expected});
        assert(actual.hasOwnProperty("avgDocSizeBytes"), {actual, expected});
    },

    validateReadDistributionMetrics(metrics) {
        if (metrics.sampleSize.total == 0) {
            assert.eq(
                bsonWoCompare(metrics, {sampleSize: {total: 0, find: 0, aggregate: 0, count: 0, distinct: 0}}),
                0,
                metrics,
            );
        } else {
            assert.eq(
                metrics.sampleSize.find +
                    metrics.sampleSize.aggregate +
                    metrics.sampleSize.count +
                    metrics.sampleSize.distinct,
                metrics.sampleSize.total,
                metrics.sampleSize,
            );

            assert(metrics.hasOwnProperty("percentageOfSingleShardReads"));
            assert(metrics.hasOwnProperty("percentageOfMultiShardReads"));
            assert(metrics.hasOwnProperty("percentageOfScatterGatherReads"));
            assert(metrics.hasOwnProperty("numReadsByRange"));

            for (let fieldName in metrics) {
                if (fieldName.startsWith("percentage")) {
                    assert.gte(metrics[fieldName], 0);
                    assert.lte(metrics[fieldName], 100);
                }
            }
            this.assertApprox(
                metrics.percentageOfSingleShardReads +
                    metrics.percentageOfMultiShardReads +
                    metrics.percentageOfScatterGatherReads,
                100,
                metrics,
            );
            assert.gt(metrics.numReadsByRange.length, 0);
        }
    },

    validateWriteDistributionMetrics(metrics) {
        if (metrics.sampleSize.total == 0) {
            assert.eq(
                bsonWoCompare(metrics, {sampleSize: {total: 0, update: 0, delete: 0, findAndModify: 0}}),
                0,
                metrics,
            );
        } else {
            assert.eq(
                metrics.sampleSize.update + metrics.sampleSize.delete + metrics.sampleSize.findAndModify,
                metrics.sampleSize.total,
                metrics.sampleSize,
            );

            assert(metrics.hasOwnProperty("percentageOfSingleShardWrites"));
            assert(metrics.hasOwnProperty("percentageOfMultiShardWrites"));
            assert(metrics.hasOwnProperty("percentageOfScatterGatherWrites"));
            assert(metrics.hasOwnProperty("percentageOfShardKeyUpdates"));
            assert(metrics.hasOwnProperty("percentageOfSingleWritesWithoutShardKey"));
            assert(metrics.hasOwnProperty("percentageOfMultiWritesWithoutShardKey"));
            assert(metrics.hasOwnProperty("numWritesByRange"));

            for (let fieldName in metrics) {
                if (fieldName.startsWith("percentage")) {
                    assert.gte(metrics[fieldName], 0);
                    assert.lte(metrics[fieldName], 100);
                }
            }
            this.assertApprox(
                metrics.percentageOfSingleShardWrites +
                    metrics.percentageOfMultiShardWrites +
                    metrics.percentageOfScatterGatherWrites,
                100,
                metrics,
            );
            assert.gt(metrics.numWritesByRange.length, 0);
        }
    },

    assertNotContainReadWriteDistributionMetrics(res) {
        assert(!res.hasOwnProperty("readDistribution"));
        assert(!res.hasOwnProperty("writeDistribution"));
    },

    assertContainReadWriteDistributionMetrics(res) {
        assert(res.hasOwnProperty("readDistribution"));
        assert(res.hasOwnProperty("writeDistribution"));
        this.validateReadDistributionMetrics(res.readDistribution);
        this.validateWriteDistributionMetrics(res.writeDistribution);
    },

    validateSampledQueryDocument(doc) {
        const readCmdNames = new Set(["find", "aggregate", "count", "distinct"]);
        assert(doc.hasOwnProperty("ns"), doc);
        assert(doc.hasOwnProperty("collectionUuid"), doc);
        assert(doc.hasOwnProperty("cmdName"), doc);
        assert(doc.hasOwnProperty("cmd"), doc);
        assert(doc.hasOwnProperty("expireAt"), doc);
        if (readCmdNames.has(doc.cmdName)) {
            assert(doc.cmd.hasOwnProperty("filter"));
            assert(doc.cmd.hasOwnProperty("collation"));
        }
    },
};
