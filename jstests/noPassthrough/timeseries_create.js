/**
 * Tests that the create command recognizes the timeseries option and only accepts valid
 * configurations of options in conjunction with and within the timeseries option.
 *
 * @tags: [
 *   requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
let collCount = 0;

const testOptions = function(allowed,
                             createOptions,
                             timeseriesOptions = {
                                 timeField: "time"
                             },
                             errorCode = ErrorCodes.InvalidOptions,
                             fixture = {
                                 // This method is run before creating time-series collection.
                                 setUp: (testDB, collName) => {},
                                 // This method is run at the end of this function after
                                 // passing all the test assertions.
                                 tearDown: (testDB, collName) => {},
                             }) {
    const testDB = conn.getDB(dbName);
    const collName = 'timeseries_' + collCount++;
    const bucketsCollName = 'system.buckets.' + collName;

    fixture.setUp(testDB, collName);
    const res = testDB.runCommand(
        Object.extend({create: collName, timeseries: timeseriesOptions}, createOptions));
    if (allowed) {
        assert.commandWorked(res);
        const collections =
            assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

        const tsColl = collections.find(coll => coll.name == collName);
        assert(tsColl, collections);
        assert.eq(tsColl.type, "timeseries", tsColl);

        const bucketsColl = collections.find(coll => coll.name == bucketsCollName);
        assert(bucketsColl, collections);
        assert.eq(bucketsColl.type, "collection", bucketsColl);
        assert(bucketsColl.options.clusteredIndex, bucketsColl);
        if (createOptions.expireAfterSeconds) {
            assert.eq(bucketsColl.options.expireAfterSeconds,
                      createOptions.expireAfterSeconds,
                      bucketsColl);
        }

        assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
    } else {
        assert.commandFailedWithCode(res, errorCode);
    }

    fixture.tearDown(testDB, collName);

    assert(!testDB.getCollectionNames().includes(collName));
    assert(!testDB.getCollectionNames().includes(bucketsCollName));
};

const testValidTimeseriesOptions = function(timeseriesOptions) {
    testOptions(true, {}, timeseriesOptions);
};

const testInvalidTimeseriesOptions = function(timeseriesOptions, errorCode) {
    testOptions(false, {}, timeseriesOptions, errorCode);
};

const testIncompatibleCreateOptions = function(createOptions, errorCode) {
    testOptions(false, createOptions, {timeField: 'time'}, errorCode);
};

const testCompatibleCreateOptions = function(createOptions) {
    testOptions(true, createOptions);
};

const testTimeseriesNamespaceExists = function(setUp) {
    testOptions(false, {}, {timeField: "time"}, ErrorCodes.NamespaceExists, {
        setUp: setUp,
        tearDown: (testDB, collName) => {
            assert.commandWorked(testDB.dropDatabase());
        }
    });
};

testValidTimeseriesOptions({timeField: "time"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta", granularity: "seconds"});

// A bucketMaxSpanSeconds may be provided, but only if they are the default for the granularity.
testValidTimeseriesOptions(
    {timeField: "time", metaField: "meta", granularity: "seconds", bucketMaxSpanSeconds: 60 * 60});
testValidTimeseriesOptions({
    timeField: "time",
    metaField: "meta",
    granularity: "minutes",
    bucketMaxSpanSeconds: 60 * 60 * 24
});
testValidTimeseriesOptions({
    timeField: "time",
    metaField: "meta",
    granularity: "hours",
    bucketMaxSpanSeconds: 60 * 60 * 24 * 30
});

testValidTimeseriesOptions({timeField: "time", metaField: "meta", granularity: "minutes"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta", granularity: "hours"});

testInvalidTimeseriesOptions("", ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: 100}, ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: "time", metaField: 100}, ErrorCodes.TypeMismatch);

testInvalidTimeseriesOptions({timeField: "time", invalidOption: {}}, 40415);
testInvalidTimeseriesOptions({timeField: "sub.time"}, ErrorCodes.InvalidOptions);
testInvalidTimeseriesOptions({timeField: "time", metaField: "sub.meta"}, ErrorCodes.InvalidOptions);
testInvalidTimeseriesOptions({timeField: "time", metaField: "time"}, ErrorCodes.InvalidOptions);

testInvalidTimeseriesOptions({timeField: "time", metaField: "meta", bucketMaxSpanSeconds: 10},
                             5510500);
testInvalidTimeseriesOptions(
    {timeField: "time", metaField: "meta", granularity: 'minutes', bucketMaxSpanSeconds: 3600},
    5510500);

testCompatibleCreateOptions({expireAfterSeconds: NumberLong(100)});
testCompatibleCreateOptions({storageEngine: {}});
testCompatibleCreateOptions({indexOptionDefaults: {}});
testCompatibleCreateOptions({collation: {locale: "ja"}});
testCompatibleCreateOptions({writeConcern: {}});
testCompatibleCreateOptions({comment: ""});

testIncompatibleCreateOptions({expireAfterSeconds: NumberLong(-10)}, ErrorCodes.InvalidOptions);
testIncompatibleCreateOptions({expireAfterSeconds: NumberLong("4611686018427387904")},
                              ErrorCodes.InvalidOptions);
testIncompatibleCreateOptions({expireAfterSeconds: ""}, ErrorCodes.TypeMismatch);
testIncompatibleCreateOptions({capped: true, size: 100});
testIncompatibleCreateOptions({capped: true, max: 100});
testIncompatibleCreateOptions({autoIndexId: true});
testIncompatibleCreateOptions({idIndex: {key: {_id: 1}, name: "_id_"}});
testIncompatibleCreateOptions({validator: {}});
testIncompatibleCreateOptions({validationLevel: "off"});
testIncompatibleCreateOptions({validationAction: "warn"});
testIncompatibleCreateOptions({viewOn: "coll"});
testIncompatibleCreateOptions({viewOn: "coll", pipeline: []});
testIncompatibleCreateOptions({clusteredIndex: true});
testIncompatibleCreateOptions({clusteredIndex: false});

testTimeseriesNamespaceExists((testDB, collName) => {
    assert.commandWorked(testDB.createCollection(collName));
});
testTimeseriesNamespaceExists((testDB, collName) => {
    assert.commandWorked(testDB.createView(collName, collName + '_source', []));
});

// Tests that schema validation is enabled on the bucket collection.
{
    const testDB = conn.getDB(dbName);
    const coll = testDB.getCollection('timeseries_' + collCount++);
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(bucketsColl.insert(
        {control: {version: 1, min: {time: ISODate()}, max: {time: ISODate()}}, data: {}}));
    assert.commandFailedWithCode(bucketsColl.insert({
        control: {version: 'not a number', min: {time: ISODate()}, max: {time: ISODate()}},
        data: {}
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(
        bucketsColl.insert(
            {control: {version: 1, min: {time: 'not a date'}, max: {time: ISODate()}}, data: {}}),
        ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(
        bucketsColl.insert(
            {control: {version: 1, min: {time: ISODate()}, max: {time: 'not a date'}}, data: {}}),
        ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({
        control: {version: 1, min: {time: ISODate()}, max: {time: ISODate()}},
        data: 'not an object'
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({invalid_bucket_field: 1}),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandWorked(testDB.runCommand({drop: coll.getName(), writeConcern: {w: "majority"}}));
}

MongoRunner.stopMongod(conn);
})();
