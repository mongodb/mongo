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

if (!db.adminCommand({getParameter: 1, featureFlagTimeSeriesCollection: 1})
         .featureFlagTimeSeriesCollection.value) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

let collCount = 0;

const testOptions = function(allowed,
                             createOptions,
                             timeseriesOptions = {
                                 timeField: "time"
                             },
                             errorCode = ErrorCodes.InvalidOptions) {
    const collName = jsTestName() + "_" + collCount++;
    const res = db.runCommand(
        Object.extend({create: collName, timeseries: timeseriesOptions}, createOptions));
    if (allowed) {
        assert.commandWorked(res);
        assert(db[collName].drop());
    } else {
        assert.commandFailedWithCode(res, errorCode);
    }
};

const testValidTimeseriesOptions = function(timeseriesOptions) {
    testOptions(true, {}, timeseriesOptions);
};

const testInvalidTimeseriesOptions = function(timeseriesOptions, errorCode) {
    testOptions(false, {}, timeseriesOptions, errorCode);
};

const testIncompatibleCreateOptions = function(createOptions) {
    testOptions(false, createOptions);
};

const testCompatibleCreateOptions = function(createOptions) {
    testOptions(true, createOptions);
};

testValidTimeseriesOptions({timeField: "time"});
testValidTimeseriesOptions({timeField: "time", metaField: "meta"});
testValidTimeseriesOptions({timeField: "time", expireAfterSeconds: NumberLong(100)});
testValidTimeseriesOptions(
    {timeField: "time", metaField: "meta", expireAfterSeconds: NumberLong(100)});

testInvalidTimeseriesOptions("", ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: 100}, ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: "time", metaField: 100}, ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: "time", expireAfterSeconds: ""}, ErrorCodes.TypeMismatch);
testInvalidTimeseriesOptions({timeField: "time", invalidOption: {}}, 40415);

testCompatibleCreateOptions({storageEngine: {}});
testCompatibleCreateOptions({indexOptionDefaults: {}});
testCompatibleCreateOptions({collation: {locale: "ja"}});
testCompatibleCreateOptions({writeConcern: {}});
testCompatibleCreateOptions({comment: ""});

testIncompatibleCreateOptions({capped: true, size: 100});
testIncompatibleCreateOptions({capped: true, max: 100});
testIncompatibleCreateOptions({autoIndexId: true});
testIncompatibleCreateOptions({idIndex: {key: {_id: 1}, name: "_id_"}});
testIncompatibleCreateOptions({validator: {}});
testIncompatibleCreateOptions({validationLevel: "off"});
testIncompatibleCreateOptions({validationAction: "warn"});
testIncompatibleCreateOptions({viewOn: "coll"});
testIncompatibleCreateOptions({viewOn: "coll", pipeline: []});
})();