// Tests that timeseries is rejected in mapReduce.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   assumes_no_implicit_collection_creation_after_drop,
//   does_not_support_stepdowns,
//   requires_scripting,
//   # Time-series collections aren't supported in mapReduce.
//   exclude_from_timeseries_crud_passthrough,
//   requires_timeseries,
// ]

const baseName = jsTestName();

const timeseriesSource = db[`${baseName}_source`];
timeseriesSource.drop();
assert.commandWorked(db.createCollection(timeseriesSource.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(timeseriesSource.insert({t: ISODate(), m: {tag: 1}, value: 100}));

const targetName = `${jsTestName()}_out`;

function mapFunc() {
    emit(this.x, 1);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

// TODO SERVER-101595 Remove ErrorCodes.CommandNotSupportedOnView from error codes.
// Test that mapReduce fails when run against timeseries.
assert.commandFailedWithCode(
    db.runCommand({mapReduce: timeseriesSource.getName(), map: mapFunc, reduce: reduceFunc, out: targetName}),
    [ErrorCodes.CommandNotSupportedOnView, 11574100, 11574101],
);
