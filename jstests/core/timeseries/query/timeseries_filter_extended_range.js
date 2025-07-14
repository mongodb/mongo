/**
 * Test that find/match type queries work properly on dates outside the 32 bit epoch range,
 *  [1970-01-01 00:00:00 UTC - 2038-01-19 03:14:07 UTC].
 *
 * @tags: [
 *   requires_fcv_62,
 *   # Refusing to run a test that issues an aggregation command with explain because it may
 *   # return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */

const timeFieldName = "time";
const standardDocs = [
    {[timeFieldName]: ISODate("1975-12-01")},
    {[timeFieldName]: ISODate("1980-01-13")},
    {[timeFieldName]: ISODate("2018-07-14")},
    {[timeFieldName]: ISODate("2030-09-30")},
];
const beforeEpochDocs = [
    {[timeFieldName]: ISODate("1969-12-31T23:00:59.001Z")},
    {[timeFieldName]: ISODate("1969-12-31T23:59:59.001Z")} /* one millisecond before the epoch */,
];
const afterEpochDocs = [
    // This date is one millisecond after the maximum (the largest 32 bit integer) number of seconds
    // since the epoch.
    {[timeFieldName]: ISODate("2038-01-19T03:14:07.001Z")},
    {[timeFieldName]: ISODate("2050-01-20T03:14:00.003Z")}
];
const allExtendedRangeDocs = beforeEpochDocs.concat(afterEpochDocs);

/*
 * Creates a collection, populates it using the `standardDocs` and any dates passed in by
 * `extendedRangeDocs`, runs the `query` and ensures that the result set is equal to `results`.
 */
function runTest({query, results, extendedRangeDocs}) {
    // Setup the collection and insert the dates in `standardDocs` and `extendedRangeDocs`.
    const tsColl = db.getCollection(jsTestName());
    tsColl.drop();
    assert.commandWorked(
        db.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.commandWorked(tsColl.insert(standardDocs.concat(extendedRangeDocs)));

    const pipeline = [{$match: query}, {$project: {_id: 0, [timeFieldName]: 1}}];
    const plan = tsColl.explain().aggregate(pipeline);

    // Verify the results.
    const aggActuals = tsColl.aggregate(pipeline).toArray();
    assert.sameMembers(results, aggActuals, JSON.stringify(plan, null, 4));

    // Verify the equivalent find command.
    let findActuals = tsColl.find(query, {_id: 0, [timeFieldName]: 1}).toArray();
    assert.sameMembers(results, findActuals, JSON.stringify(plan, null, 4));
}

// Verify when there are no extended range dates in the collection, we handle querying by extended
// range dates. The code for this is shared by multiple operators, so we don't have to test each
// operator separately.
runTest({
    query: {[timeFieldName]: {$lte: beforeEpochDocs[1].time}},
    results: [],
    extendedRangeDocs: []
});
runTest({
    query: {[timeFieldName]: {$lte: afterEpochDocs[0].time}},
    results: standardDocs,
    extendedRangeDocs: []
});
runTest({
    query: {[timeFieldName]: {$gt: beforeEpochDocs[1].time}},
    results: standardDocs,
    extendedRangeDocs: []
});
runTest(
    {query: {[timeFieldName]: {$gt: afterEpochDocs[0].time}}, results: [], extendedRangeDocs: []});
runTest({query: {[timeFieldName]: afterEpochDocs[0].time}, results: [], extendedRangeDocs: []});

/*
 * Verify $eq queries for dates inside, below and above the epoch ranges.
 */
// Test in the epoch range when there are measurements before and after the epoch.
runTest({
    query: {[timeFieldName]: {$eq: standardDocs[1].time}},
    results: [standardDocs[1]],
    extendedRangeDocs: allExtendedRangeDocs
});

// Test when there is one date above the epoch range. The date is one millisecond after the maximum
// (the largest 32 bit integer) number of seconds since the epoch.
runTest({
    query: {[timeFieldName]: {$eq: afterEpochDocs[0].time}},
    results: [afterEpochDocs[0]],
    extendedRangeDocs: [afterEpochDocs[0]]
});

// Test when there are multiple dates after the epoch range.
runTest({
    query: {[timeFieldName]: {$eq: afterEpochDocs[0].time}},
    results: [afterEpochDocs[0]],
    extendedRangeDocs: afterEpochDocs
});

// Test when there is one date before the epoch. The date is one millisecond before the epoch.
runTest({
    query: {[timeFieldName]: {$eq: beforeEpochDocs[1].time}},
    results: [beforeEpochDocs[1]],
    extendedRangeDocs: [beforeEpochDocs[1]]
});

// Test when there are multiple dates before the epoch.
runTest({
    query: {[timeFieldName]: {$eq: beforeEpochDocs[0].time}},
    results: [beforeEpochDocs[0]],
    extendedRangeDocs: beforeEpochDocs
});

/*
 * $lt queries.
 */
// Test with dates below the epoch that match the predicate.
runTest({
    query: {[timeFieldName]: {$lt: beforeEpochDocs[1].time}},
    results: [beforeEpochDocs[0]],
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates above the epoch that do not match the predicate.
runTest({
    query: {[timeFieldName]: {$lt: standardDocs[2].time}},
    results: [standardDocs[0], standardDocs[1]],
    extendedRangeDocs: afterEpochDocs
});

// Test with dates both above and below the epoch. Only the before dates will match the predicate.
runTest({
    query: {[timeFieldName]: {$lt: standardDocs[1].time}},
    results: [standardDocs[0], beforeEpochDocs[0], beforeEpochDocs[1]],
    extendedRangeDocs: allExtendedRangeDocs
});

// Test with dates both above and below the epoch. Both before and after dates match the predicate.
runTest({
    query: {[timeFieldName]: {$lt: afterEpochDocs[1].time}},
    results: standardDocs.concat(beforeEpochDocs, [afterEpochDocs[0]]),
    extendedRangeDocs: allExtendedRangeDocs
});

/*
 * $gt queries.
 */
// Test with dates below the epoch that do not match the predicate.
runTest({
    query: {[timeFieldName]: {$gt: beforeEpochDocs[1].time}},
    results: standardDocs,
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates above the epoch that do not match the predicate.
runTest({
    query: {[timeFieldName]: {$gt: afterEpochDocs[1].time}},
    results: [],
    extendedRangeDocs: afterEpochDocs
});

// Test with dates both above and below the epoch. Only the after dates will match the predicate.
runTest({
    query: {[timeFieldName]: {$gt: standardDocs[2].time}},
    results: [standardDocs[3], afterEpochDocs[0], afterEpochDocs[1]],
    extendedRangeDocs: allExtendedRangeDocs
});

// Test with dates both above and below the epoch. Both before and after dates match the predicate.
runTest({
    query: {[timeFieldName]: {$gt: beforeEpochDocs[0].time}},
    results: standardDocs.concat([beforeEpochDocs[1]], afterEpochDocs),
    extendedRangeDocs: allExtendedRangeDocs
});

/*
 * $lte queries.
 */

// Test with dates below the epoch that do match the predicate.
runTest({
    query: {[timeFieldName]: {$lte: standardDocs[0].time}},
    results: beforeEpochDocs.concat([standardDocs[0]]),
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates above the epoch that do match the predicate.
runTest({
    query: {[timeFieldName]: {$lte: standardDocs[2].time}},
    results: [standardDocs[0], standardDocs[1], standardDocs[2]],
    extendedRangeDocs: afterEpochDocs
});

// Test with dates both above and below the epoch. Only the before dates will match the predicate.
runTest({
    query: {[timeFieldName]: {$lte: standardDocs[2].time}},
    results: beforeEpochDocs.concat(standardDocs.slice(0, -1)),
    extendedRangeDocs: allExtendedRangeDocs
});

// Test with dates both above and below the epoch. Both before and after dates will match the
// predicate.
runTest({
    query: {[timeFieldName]: {$lte: afterEpochDocs[0].time}},
    results: standardDocs.concat(beforeEpochDocs, [afterEpochDocs[0]]),
    extendedRangeDocs: allExtendedRangeDocs
});

/*
 * $gte queries.
 */
// Test with dates below the epoch that do not match the predicate.
runTest({
    query: {[timeFieldName]: {$gte: standardDocs[1].time}},
    results: standardDocs.slice(1),
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates above the epoch that do match the predicate.
runTest({
    query: {[timeFieldName]: {$gte: standardDocs[2].time}},
    results: afterEpochDocs.concat(standardDocs.slice(2)),
    extendedRangeDocs: afterEpochDocs
});

// Test with dates both above and below the epoch. Only the after dates will match the predicate.
runTest({
    query: {[timeFieldName]: {$gte: standardDocs[1].time}},
    results: afterEpochDocs.concat(standardDocs.slice(1)),
    extendedRangeDocs: allExtendedRangeDocs
});

// Test with dates both above and below the epoch. Both before and after dates will match the
// predicate.
runTest({
    query: {[timeFieldName]: {$gte: beforeEpochDocs[1].time}},
    results: afterEpochDocs.concat(standardDocs, [beforeEpochDocs[1]]),
    extendedRangeDocs: allExtendedRangeDocs
});

/*
 * Compound predicates ($and, $or).
 */

// Test with dates below the epoch that do match the predicate.
runTest({
    query: {[timeFieldName]: {$gt: ISODate("1920-01-01"), $lt: ISODate("1980-01-01")}},
    results: beforeEpochDocs.concat([standardDocs[0]]),
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates below the epoch that do not match the predicate.
runTest({
    query:
        {[timeFieldName]: {$gt: ISODate("1970-01-01T00:00:00.001Z"), $lt: ISODate("1980-01-01")}},
    results: [standardDocs[0]],
    extendedRangeDocs: beforeEpochDocs
});

// Test with dates above the epoch that do match the predicate.
runTest({
    query: {
        [timeFieldName]:
            {$gt: ISODate("2030-09-29T23:59:59.001Z"), $lt: ISODate("2050-01-20T03:14:00.001Z")}
    },
    results: [standardDocs[3], afterEpochDocs[0]],
    extendedRangeDocs: afterEpochDocs
});

// Test with dates above the epoch that do not match the predicate.
runTest({
    query: {
        [timeFieldName]:
            {$gt: ISODate("2030-09-29T23:59:59.001Z"), $lt: ISODate("2038-01-19T03:14:00.000Z")}
    },
    results: [standardDocs[3]],
    extendedRangeDocs: afterEpochDocs
});

// Test with dates both above and below the epoch. Both before and after dates will match the
// predicate.
runTest({
    query: {
        $or: [
            {[timeFieldName]: {$lt: ISODate("1975-12-01T23:59:00.001Z")}},
            {[timeFieldName]: {$gt: ISODate("2038-01-19T03:14:07.002Z")}}
        ]
    },
    results: beforeEpochDocs.concat([standardDocs[0], afterEpochDocs[1]]),
    extendedRangeDocs: allExtendedRangeDocs
});

// Test with dates both above and below the epoch. Neither before nor after dates will match the
// predicate.
runTest({
    query: {[timeFieldName]: {$in: [standardDocs[1].time, standardDocs[3].time]}},
    results: [standardDocs[1], standardDocs[3]],
    extendedRangeDocs: allExtendedRangeDocs
});
