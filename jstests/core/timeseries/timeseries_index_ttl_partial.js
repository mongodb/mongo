/**
 * Tests the creation of partial, TTL indexes on a time-series collection.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    return;
}

const collName = "timeseries_index_ttl_partial";
const indexName = "partialTTLIndex";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = "tm";
const metaFieldName = "mm";
const timeSpec = {
    [timeFieldName]: 1
};
const metaSpec = {
    [metaFieldName]: 1
};

const expireAfterSeconds = NumberLong(400);

const resetTsColl = function(extraOptions = {}) {
    coll.drop();

    let options = {timeseries: {timeField: timeFieldName, metaField: metaFieldName}};
    assert.commandWorked(db.createCollection(coll.getName(), Object.merge(options, extraOptions)));
};

(function invalidTTLIndexes() {
    resetTsColl();

    let options = {name: indexName, expireAfterSeconds: 3600};
    // TTL indexes on the time field are only allowed in conjunction with partialFilterExpressions
    // on the metafield.
    assert.commandFailedWithCode(coll.createIndex(timeSpec, options), ErrorCodes.InvalidOptions);

    // TTL indexes on the metafield are not allowed.
    assert.commandFailedWithCode(coll.createIndex(metaSpec, options), ErrorCodes.InvalidOptions);
}());

(function partialTTLIndexesShouldSucceed() {
    resetTsColl();
    const options = {
        name: indexName,
        partialFilterExpression: {[metaFieldName]: {$gt: 5}},
        expireAfterSeconds: expireAfterSeconds
    };

    // Creating a TTL index on time, with a partial filter expression on the metaField should
    // succeed.
    assert.commandWorked(coll.createIndex(
        timeSpec, Object.merge(options, {expireAfterSeconds: expireAfterSeconds})));
    let indexes = coll.getIndexes().filter(ix => ix.name === indexName);
    assert.eq(1, indexes.length, tojson(indexes));

    let partialTTLIndex = indexes[0];
    assert.eq(indexName, partialTTLIndex.name, tojson(partialTTLIndex));
    assert.eq(timeSpec, partialTTLIndex.key, tojson(partialTTLIndex));
    assert.eq(expireAfterSeconds, partialTTLIndex.expireAfterSeconds, tojson(partialTTLIndex));

    resetTsColl({expireAfterSeconds: 3600});

    // Creating an index on time (on a time-series collection created with the expireAfterSeconds
    // parameter) with a partial filter expression on the metaField should succeed.
    assert.commandWorked(coll.createIndex(timeSpec, options));
    indexes = coll.getIndexes().filter(ix => ix.name === indexName);
    assert.eq(1, indexes.length, tojson(indexes));

    partialTTLIndex = indexes[0];
    assert.eq(indexName, partialTTLIndex.name, tojson(partialTTLIndex));
    assert.eq(timeSpec, partialTTLIndex.key, tojson(partialTTLIndex));
    assert.eq(expireAfterSeconds, partialTTLIndex.expireAfterSeconds, tojson(partialTTLIndex));
}());

(function invalidPartialTTLIndexesShouldFail() {
    resetTsColl();

    const currentData = ISODate();
    const filterOnData = {
        name: indexName,
        partialFilterExpression: {"data": {$gt: 5}},
        expireAfterSeconds: expireAfterSeconds
    };
    const filterOnMeta = {
        name: indexName,
        partialFilterExpression: {[metaFieldName]: {$gt: 5}},
        expireAfterSeconds: expireAfterSeconds
    };
    const filterOnMetaAndData = {
        name: indexName,
        partialFilterExpression: {[metaFieldName]: {$gt: 5}, "data": {$gt: 5}},
        expireAfterSeconds: expireAfterSeconds
    };
    const filterOnTime = {
        name: indexName,
        partialFilterExpression: {[timeFieldName]: {$gt: currentData}},
        expireAfterSeconds: expireAfterSeconds
    };
    const dataSpec = {"data": 1};

    // These cases have a valid index specs on the time field but invalid partialFilterExpressions.
    {
        // A TTL index on time requires partial indexes to be on the metadata field.
        assert.commandFailedWithCode(coll.createIndex(timeSpec, filterOnData),
                                     ErrorCodes.InvalidOptions);

        // A TTL index on time requires partial indexes on the metadata field only, no compound
        // expressions.
        assert.commandFailedWithCode(coll.createIndex(timeSpec, filterOnMetaAndData),
                                     ErrorCodes.InvalidOptions);

        // Partial indexes are not allowed to be on the timeField.
        assert.commandFailedWithCode(coll.createIndex(timeSpec, filterOnTime),
                                     ErrorCodes.InvalidOptions);
    }

    const timeAndMetaSpec = Object.merge(timeSpec, metaSpec);
    const timeAndDataSpec = Object.merge(timeSpec, dataSpec);
    // These cases have valid partialFilterExpressions but invalid index specs.
    {
        // TTL indexes are only allowed on the time field.
        assert.commandFailedWithCode(coll.createIndex(metaSpec, filterOnMeta),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(coll.createIndex(dataSpec, filterOnMeta),
                                     ErrorCodes.InvalidOptions);

        // TTL indexes are not allowed on compound indexes (even if a time field exists in the
        // spec).
        assert.commandFailedWithCode(coll.createIndex(timeAndMetaSpec, filterOnMeta),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.createIndex(timeAndDataSpec, filterOnMeta),
                                     ErrorCodes.CannotCreateIndex);
    }
}());
})();
