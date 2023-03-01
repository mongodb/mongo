/**
 * Utilities for testing the analyzeShardKey command.
 */
var AnalyzeShardKeyUtil = (function() {
    /**
     * Returns true if the given key pattern contains a hashed key.
     */
    function isHashedKeyPattern(keyPattern) {
        for (let fieldName in keyPattern) {
            if (keyPattern[fieldName] == "hashed") {
                return true;
            }
        }
        return false;
    }

    /**
     * Returns true if the given key pattern is {_id: 1}.
     */
    function isIdKeyPattern(keyPattern) {
        return bsonWoCompare(keyPattern, {_id: 1}) == 0;
    }

    /**
     * Returns a set of current shard key field names, candidate shard key field names and
     * index key field names combined.
     */
    function getCombinedFieldNames(currentShardKey, candidateShardKey, indexKey) {
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
    }

    /**
     * Returns the value for the given field.
     */
    function getDottedField(doc, fieldName) {
        let val = doc;
        const fieldNames = fieldName.split(".");
        for (let i = 0; i < fieldNames.length; i++) {
            val = val[fieldNames[i]];
        }
        return val;
    }

    /**
     * Sets the given field to the given value. The field name can be dotted.
     */
    function setDottedField(doc, fieldName, val) {
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
    }

    /**
     * Extracts the shard key value from the given document.
     */
    function extractShardKeyValueFromDocument(doc, shardKey, indexKey) {
        const shardKeyValue = {};
        for (let fieldName in shardKey) {
            const isHashed = indexKey[fieldName] == "hashed";
            const value = AnalyzeShardKeyUtil.getDottedField(doc, fieldName);
            // TODO (SERVER-70994): After SERVER-72814, make sure that the analyzeShardKey command
            // doesn't return hash values in the case where the supporting index is hashed.
            shardKeyValue[fieldName] = isHashed ? convertShardKeyToHashed(value) : value;
        }
        return shardKeyValue;
    }

    /**
     * Returns a random integer between the given range (inclusive).
     */
    function getRandInteger(min, max) {
        return Math.floor(Math.random() * (max - min + 1)) + min;
    }

    /**
     * Returns a random element in the given array.
     */
    function getRandomElement(arr) {
        return arr[getRandInteger(0, arr.length - 1)];
    }

    /**
     * Returns the field name "<prefix>", "<prefix>.x" or "<prefix>.x.y" with roughly equal
     * probability.
     */
    function getRandomFieldName(prefix) {
        const prob = Math.random();
        if (prob < 0.3) {
            return prefix;
        } else if (prob < 0.6) {
            return prefix + ".x";
        }
        return prefix + ".x.y";
    }

    /**
     * Enables profiling of the given database on all the given mongods.
     */
    function enableProfiler(mongodConns, dbName) {
        mongodConns.forEach(conn => {
            assert.commandWorked(conn.getDB(dbName).setProfilingLevel(2));
        });
    }

    /**
     * Disables profiling of the given database on all the given mongods.
     */
    function disableProfiler(mongodConns, dbName) {
        mongodConns.forEach(conn => {
            assert.commandWorked(conn.getDB(dbName).setProfilingLevel(0));
        });
    }

    function calculatePercentage(part, whole) {
        assert.gte(part, 0);
        assert.gt(whole, 0);
        assert.lte(part, whole);
        return (part * 100.0 / whole);
    }

    /**
     * Returns true if 'objs' contains 'obj'.
     */
    function containsBSONObj(objs, obj) {
        for (let tmpObj of objs) {
            if (bsonWoCompare(obj, tmpObj) == 0) {
                return true;
            }
        }
        return false;
    }

    // The analyzeShardKey command rounds the percentages 10 decimal places. The epsilon is chosen
    // to account for that.
    function assertApprox(actual, expected, msg, epsilon = 1e-9) {
        return assert.lte(Math.abs(actual - expected), epsilon, {actual, expected, msg});
    }

    function assertNotContainKeyCharacteristicsMetrics(actual) {
        assert(!actual.hasOwnProperty("numDocs"), actual);
        assert(!actual.hasOwnProperty("isUnique"), actual);
        assert(!actual.hasOwnProperty("numDistinctValues"), actual);
        assert(!actual.hasOwnProperty("mostCommonValues"), actual);
        assert(!actual.hasOwnProperty("monotonicity"), actual);
        assert(!actual.hasOwnProperty("avgDocSizeBytes"), actual);
    }

    function assertContainKeyCharacteristicsMetrics(actual) {
        assert(actual.hasOwnProperty("numDocs"), actual);
        assert(actual.hasOwnProperty("isUnique"), actual);
        assert(actual.hasOwnProperty("numDistinctValues"), actual);
        assert(actual.hasOwnProperty("mostCommonValues"), actual);
        assert(actual.hasOwnProperty("monotonicity"), actual);
        assert(actual.hasOwnProperty("avgDocSizeBytes"), actual);
    }

    function assertKeyCharacteristicsMetrics(actual, expected) {
        assert.eq(actual.numDocs, expected.numDocs, {actual, expected});
        assert.eq(actual.isUnique, expected.isUnique, {actual, expected});
        assert.eq(actual.numDistinctValues, expected.numDistinctValues, {actual, expected});

        // Verify the number of most common shard key values returned is less than what
        // 'analyzeShardKeyNumMostCommonValues' is set to.
        assert.gt(actual.mostCommonValues.length, 0);
        assert.lte(
            actual.mostCommonValues.length, expected.numMostCommonValues, {actual, expected});
        let prevFrequency = Number.MAX_VALUE;
        for (let mostCommonValue of actual.mostCommonValues) {
            // Verify the shard key values are sorted in descending of frequency.
            assert.lte(mostCommonValue.frequency, prevFrequency, {
                mostCommonValue,
                actual: actual.mostCommonValues,
                expected: expected.mostCommonValues
            });

            // Verify that this shard key value is among the expected ones.
            assert(containsBSONObj(expected.mostCommonValues, mostCommonValue), {
                mostCommonValue,
                actual: actual.mostCommonValues,
                expected: expected.mostCommonValues
            });
            prevFrequency = mostCommonValue.frequency;
        }

        assert(actual.hasOwnProperty("monotonicity"), {actual, expected});
        assert(actual.hasOwnProperty("avgDocSizeBytes"), {actual, expected});
    }

    return {
        isHashedKeyPattern,
        isIdKeyPattern,
        getCombinedFieldNames,
        getDottedField,
        setDottedField,
        extractShardKeyValueFromDocument,
        getRandInteger,
        getRandomElement,
        getRandomFieldName,
        enableProfiler,
        disableProfiler,
        calculatePercentage,
        assertApprox,
        assertNotContainKeyCharacteristicsMetrics,
        assertContainKeyCharacteristicsMetrics,
        assertKeyCharacteristicsMetrics
    };
})();
