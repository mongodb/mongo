/**
 * Test that find/match type queries work properly on dates ouside the 32 bit epoch range,
 *  [1970-01-01 00:00:00 UTC - 2038-01-29 03:13:07 UTC].
 *
 * @tags: [
 *   requires_fcv_62,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {
"use strict";
const timeFieldName = "time";

/*
 * Creates a collection, populates it, runs the `query` and ensures that the result set
 * is equal to `results`.
 *
 * If overflow is set we create a document with dates above the 32 bit range (year 2040)
 * If underflow is set, we create a document with dates below the 32 bit range (year 1965)
 */
function runTest(underflow, overflow, query, results) {
    // Setup our DB & our collections.
    const tsColl = db.getCollection(jsTestName());
    tsColl.drop();

    assert.commandWorked(
        db.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));

    const dates = [
        // If underflow, we want to insert a date that would fall below the epoch
        // i.e. 1970-01-01 00:00:00 UTC. Otherwise we use a date within the epoch.
        {[timeFieldName]: underflow ? new Date("1965-01-01") : new Date("1971-01-01")},
        {[timeFieldName]: new Date("1975-01-01")},
        {[timeFieldName]: new Date("1980-01-01")},
        {[timeFieldName]: new Date("1995-01-01")},
        // If overflow, we want to insert a date that would use more than 32 bit milliseconds after
        // the epoch. This overflow will occur 2038-01-29 03:13:07 UTC. Otherwise we go slightly
        // before the end of the 32 bit epoch.
        {[timeFieldName]: overflow ? new Date("2040-01-01") : new Date("2030-01-01")}
    ];
    assert.commandWorked(tsColl.insert(dates));

    // Make sure the expected results are in the correct order for comparison below.
    function cmpTimeFields(a, b) {
        return (b[timeFieldName].getTime() - a[timeFieldName].getTime());
    }
    results.sort(cmpTimeFields);

    const pipeline = [{$match: query}, {$project: {_id: 0, [timeFieldName]: 1}}];

    // Verify agg pipeline. We don't want to go through a plan that encourages a sort order to
    // avoid BUS and index selection, so we sort after gathering the results.
    const aggActuals = tsColl.aggregate(pipeline).toArray();
    aggActuals.sort(cmpTimeFields);
    assert.docEq(results, aggActuals);

    // Verify the equivalent find command. We again don't want to go through a plan that
    // encourages a sort order to avoid BUS and index selection, so we sort after gathering the
    // results.
    let findActuals = tsColl.find(query, {_id: 0, [timeFieldName]: 1}).toArray();
    findActuals.sort(cmpTimeFields);
    assert.docEq(results, findActuals);
}

runTest(false,
        false,
        {[timeFieldName]: {$eq: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1980-01-01")}]);
runTest(false,
        true,
        {[timeFieldName]: {$eq: new Date("2040-01-01")}},
        [{[timeFieldName]: new Date("2040-01-01")}]);
runTest(true,
        false,
        {[timeFieldName]: {$eq: new Date("1965-01-01")}},
        [{[timeFieldName]: new Date("1965-01-01")}]);

runTest(false,
        false,
        {[timeFieldName]: {$lt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1971-01-01")}, {[timeFieldName]: new Date("1975-01-01")}]);
runTest(false,
        true,
        {[timeFieldName]: {$lt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1971-01-01")}, {[timeFieldName]: new Date("1975-01-01")}]);
runTest(true,
        false,
        {[timeFieldName]: {$lt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1965-01-01")}, {[timeFieldName]: new Date("1975-01-01")}]);
runTest(true,
        true,
        {[timeFieldName]: {$lt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1965-01-01")}, {[timeFieldName]: new Date("1975-01-01")}]);

runTest(false,
        false,
        {[timeFieldName]: {$gt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1995-01-01")}, {[timeFieldName]: new Date("2030-01-01")}]);
runTest(false,
        true,
        {[timeFieldName]: {$gt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1995-01-01")}, {[timeFieldName]: new Date("2040-01-01")}]);
runTest(true,
        false,
        {[timeFieldName]: {$gt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1995-01-01")}, {[timeFieldName]: new Date("2030-01-01")}]);
runTest(true,
        true,
        {[timeFieldName]: {$gt: new Date("1980-01-01")}},
        [{[timeFieldName]: new Date("1995-01-01")}, {[timeFieldName]: new Date("2040-01-01")}]);

runTest(false, false, {[timeFieldName]: {$lte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1971-01-01")},
    {[timeFieldName]: new Date("1975-01-01")},
    {[timeFieldName]: new Date("1980-01-01")}
]);
runTest(false, true, {[timeFieldName]: {$lte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1971-01-01")},
    {[timeFieldName]: new Date("1975-01-01")},
    {[timeFieldName]: new Date("1980-01-01")}
]);
runTest(true, false, {[timeFieldName]: {$lte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1965-01-01")},
    {[timeFieldName]: new Date("1975-01-01")},
    {[timeFieldName]: new Date("1980-01-01")}
]);
runTest(true, true, {[timeFieldName]: {$lte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1965-01-01")},
    {[timeFieldName]: new Date("1975-01-01")},
    {[timeFieldName]: new Date("1980-01-01")}
]);

runTest(false, false, {[timeFieldName]: {$gte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1980-01-01")},
    {[timeFieldName]: new Date("1995-01-01")},
    {[timeFieldName]: new Date("2030-01-01")}
]);
runTest(false, true, {[timeFieldName]: {$gte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1980-01-01")},
    {[timeFieldName]: new Date("1995-01-01")},
    {[timeFieldName]: new Date("2040-01-01")}
]);
runTest(true, false, {[timeFieldName]: {$gte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1980-01-01")},
    {[timeFieldName]: new Date("1995-01-01")},
    {[timeFieldName]: new Date("2030-01-01")}
]);
runTest(true, true, {[timeFieldName]: {$gte: new Date("1980-01-01")}}, [
    {[timeFieldName]: new Date("1980-01-01")},
    {[timeFieldName]: new Date("1995-01-01")},
    {[timeFieldName]: new Date("2040-01-01")}
]);

// Verify ranges that straddle the lower epoch work properly
runTest(
    true, false, {[timeFieldName]: {$gt: new Date("1920-01-01"), $lt: new Date("1980-01-01")}}, [
        {[timeFieldName]: new Date("1965-01-01")},
        {[timeFieldName]: new Date("1975-01-01")},
    ]);

runTest(
    false, true, {[timeFieldName]: {$gt: new Date("1980-01-01"), $lt: new Date("2050-01-01")}}, [
        {[timeFieldName]: new Date("1995-01-01")},
        {[timeFieldName]: new Date("2040-01-01")},
    ]);

// TODO: SERVER-69952 Literals outside the epoch are currently compared to _id, generally,
// so we cannot match against them. This will have to be fixed in a similar manner by determining
// whether the compared dates can be outside the epoch range and not relying on _id in that case.
//
// The following scenarios fail:
// runTest(
//    false, false, {[timeFieldName]: {$gt: new Date("1920-01-01"), $lt: new Date("1980-01-01")}}, [
//         {[timeFieldName]: new Date("1971-01-01")},
//         {[timeFieldName]: new Date("1975-01-01")},
//     ]);
// runTest(
//     false, false, {[timeFieldName]: {$gt: new Date("1980-01-01"), $lt: new Date("2050-01-01")}},
//     [
//         {[timeFieldName]: new Date("1995-01-01")},
//         {[timeFieldName]: new Date("2030-01-01")},
//     ]);
})();