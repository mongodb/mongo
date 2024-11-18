/**
 * Tests that the create command recognizes the timeseries option and only accepts valid
 * configurations of options in conjunction with and within the timeseries option.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const testDB = db.getSiblingDB(jsTestName());
let collCount = 0;

const bucketGranularityError = ErrorCodes.InvalidOptions;
const bucketMaxSpanSecondsError = ErrorCodes.InvalidOptions;

const testOptions = function({
    errorCode,
    createOptions = {},
    timeseriesOptions = {
        timeField: "time"
    },
    optionsAffectStorage = true,
    fixture = {
        // This method is run before creating time-series collection.
        setUp: (testDB, collName) => {},
        // This method is run at the end of this function after
        // passing all the test assertions.
        tearDown: (testDB, collName) => {},
    },
}) {
    const collName = 'timeseries_' + collCount++;
    const bucketsCollName = 'system.buckets.' + collName;

    assert.commandWorked(testDB.runCommand({drop: collName}));
    fixture.setUp(testDB, collName);

    const create = function() {
        return testDB.createCollection(
            collName, Object.extend({timeseries: timeseriesOptions}, createOptions));
    };
    const res = create();

    if (!errorCode) {
        assert.commandWorked(res);

        // Test that the creation is idempotent.
        assert.commandWorked(create());

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

        // If there are more options than only the time field, test that we get NamespaceExists if a
        // collection already exists with the same name but without those additional options.
        if (optionsAffectStorage &&
            (Object.entries(createOptions).length > 0 ||
             Object.entries(timeseriesOptions).length > 1)) {
            assert.commandWorked(testDB.createCollection(
                collName, {timeseries: {timeField: timeseriesOptions["timeField"]}}));
            assert.commandFailedWithCode(create(), ErrorCodes.NamespaceExists);

            assert.commandWorked(testDB.runCommand({drop: collName}));
        }
    } else {
        assert.commandFailedWithCode(res, errorCode);
    }

    fixture.tearDown(testDB, collName);

    assert(!testDB.getCollectionNames().includes(collName));
    assert(!testDB.getCollectionNames().includes(bucketsCollName));
};

const testValidTimeseriesOptions = function(timeseriesOptions) {
    testOptions({errorCode: null, timeseriesOptions: timeseriesOptions});
};

const testInvalidTimeseriesOptions = function(timeseriesOptions,
                                              errorCode = ErrorCodes.InvalidOptions) {
    testOptions({
        errorCode: errorCode,
        timeseriesOptions: timeseriesOptions,
    });
};

const testIncompatibleCreateOptions = function(createOptions,
                                               errorCode = ErrorCodes.InvalidOptions) {
    testOptions({
        errorCode: errorCode,
        createOptions: createOptions,
    });
};

const testCompatibleCreateOptions = function(createOptions, optionsAffectStorage = true) {
    testOptions({
        errorCode: null,
        createOptions: createOptions,
        optionsAffectStorage: optionsAffectStorage,
    });
};

const testTimeseriesNamespaceExists = function(setUp) {
    testOptions({
        errorCode: ErrorCodes.NamespaceExists,
        fixture: {
            setUp: setUp,
            tearDown: (testDB, collName) => {
                assert.commandWorked(testDB.dropDatabase());
            }
        },
    });
};

testValidTimeseriesOptions({timeField: "time"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta"});
testValidTimeseriesOptions({timeField: "time", granularity: "minutes"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta", granularity: "minutes"});

// Granularity can include a corresponding bucketMaxSpanSeconds value, but not a
// bucketRoundingSeconds value (even if the value corresponds to the granularity).
testInvalidTimeseriesOptions({
    timeField: "time",
    metaField: "meta",
    granularity: "seconds",
    bucketMaxSpanSeconds: 60 * 60,
    bucketRoundingSeconds: 60
},
                             ErrorCodes.InvalidOptions);

// Granularity may be provided with bucketMaxSpanSeconds as long as it corresponds to the
// granularity.
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

testInvalidTimeseriesOptions({timeField: "time", invalidOption: {}}, ErrorCodes.IDLUnknownField);
testInvalidTimeseriesOptions({timeField: "sub.time"}, ErrorCodes.InvalidOptions);
testInvalidTimeseriesOptions({timeField: "time", metaField: "sub.meta"}, ErrorCodes.InvalidOptions);
testInvalidTimeseriesOptions({timeField: "time", metaField: "time"}, ErrorCodes.InvalidOptions);

testInvalidTimeseriesOptions({timeField: "time", metaField: "meta", bucketMaxSpanSeconds: 10},
                             bucketMaxSpanSecondsError);
testInvalidTimeseriesOptions(
    {timeField: "time", metaField: "meta", granularity: 'minutes', bucketMaxSpanSeconds: 3600},
    bucketMaxSpanSecondsError);

testCompatibleCreateOptions({expireAfterSeconds: NumberLong(100)});
testCompatibleCreateOptions({storageEngine: {}}, false);
testCompatibleCreateOptions({storageEngine: {[TestData.storageEngine]: {}}});
testCompatibleCreateOptions({indexOptionDefaults: {}}, false);
testCompatibleCreateOptions({indexOptionDefaults: {storageEngine: {[TestData.storageEngine]: {}}}});
testCompatibleCreateOptions({collation: {locale: "ja"}});
testCompatibleCreateOptions({writeConcern: {}}, false);
testCompatibleCreateOptions({comment: ""}, false);

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
    const coll = testDB.getCollection('timeseries_' + collCount++);
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    const oid = ObjectId("65F9971847423af45aeafc67");
    const timestamp = ISODate("2024-03-19T13:46:00Z");
    assert.commandWorked(bucketsColl.insert({
        _id: oid,
        control: {
            version: TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)
                ? TimeseriesTest.BucketVersion.kCompressedSorted
                : TimeseriesTest.BucketVersion.kUncompressed,
            min: {time: timestamp, "_id": oid, "a": 1},
            max: {time: timestamp, "_id": oid, "a": 1},
            count: 1
        },
        data: {
            "time": TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)
                ? BinData(7, "CQDANfZWjgEAAAA=")
                : {"0": timestamp},
            "_id": TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)
                ? BinData(7, "BwBl+ZcgR0I69Frq/GcA")
                : {"0": oid},
            "a": TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)
                ? BinData(7, "AQAAAAAAAADwPwA=")
                : {"0": 1},
        }
    }));

    assert.commandFailedWithCode(bucketsColl.insert({
        control: {version: 'not a number', min: {time: ISODate()}, max: {time: ISODate()}},
        data: {}
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({
        control: {
            version: TimeseriesTest.BucketVersion.kUncompressed,
            min: {time: 'not a date'},
            max: {time: ISODate()}
        },
        data: {}
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({
        control: {
            version: TimeseriesTest.BucketVersion.kUncompressed,
            min: {time: ISODate()},
            max: {time: 'not a date'}
        },
        data: {}
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({
        control: {
            version: TimeseriesTest.BucketVersion.kUncompressed,
            min: {time: ISODate()},
            max: {time: ISODate()}
        },
        data: 'not an object'
    }),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandFailedWithCode(bucketsColl.insert({invalid_bucket_field: 1}),
                                 ErrorCodes.DocumentValidationFailure);
    assert.commandWorked(testDB.runCommand({drop: coll.getName(), writeConcern: {w: "majority"}}));
}
