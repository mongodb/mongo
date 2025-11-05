/**
 * Test the behavior of subpipelines with rawData = true. Ensures that subpipelines operate on raw buckets directly when this flag is true.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This wasn't fixed until 8.3.
 *   requires_fcv_83,
 *   # Getmores may be needed to exhaust the result sets.
 *   requires_getmore,
 * ]
 */

/**
 * Setup
 */
const testDB = db;
const tsCollName = jsTestName() + "_ts";
const localCollName = jsTestName();
const timeField = "t";
const metaField = "shard";

// Create a few different buckets.
const tsColl = testDB[tsCollName];
tsColl.drop();
assert.commandWorked(testDB.createCollection(tsCollName, {timeseries: {timeField, metaField}}));
const data = [
    {[timeField]: new Date(), [metaField]: "shard1"},
    {[timeField]: new Date("2020-01-01"), [metaField]: "shard1"},
    {[timeField]: new Date(), [metaField]: "shard1"},
    {[timeField]: new Date("2017-01-01"), [metaField]: "shard1"},
    {[timeField]: new Date(), [metaField]: "shard1"},
    {[timeField]: new Date("2016-01-01"), [metaField]: "shard1"},
];
assert.commandWorked(tsColl.insertMany(data));

const localColl = testDB[localCollName];
assert.commandWorked(localColl.insert({[timeField]: new Date(), join: "shard1"}));

/**
 * Lookup
 */
// Note we must lookup on "meta" because the foreign collection is raw.
const results = localColl
    .aggregate([{$lookup: {from: tsCollName, localField: "join", foreignField: "meta", as: "things"}}], {
        rawData: true,
    })
    .toArray();
// Joined/looked-up results are in the "things" field.
// Sample and ensure that it has the control field. Indicating the results is still bucketed/raw.
assert.hasFields(results[0]["things"][0], ["control"], tojson({erroneousResults: results}));

/**
 * Union With
 */
const resultsUnion = localColl
    .aggregate(
        [
            // This eliminates all local results.
            {$match: {[metaField]: "shard1"}},
            {$unionWith: {coll: tsCollName, pipeline: []}},
        ],
        {rawData: true},
    )
    .toArray();
// We filter out all local results before unioning so all results should be foreign.
// Sample and ensure that it has the control field. Indicating the results is still bucketed/raw.
assert.hasFields(resultsUnion[0], ["control"], tojson({erroneousResults: resultsUnion}));

/**
 * Graph Lookup
 */
const resultsGraphLookup = localColl
    .aggregate(
        [
            {
                $graphLookup: {
                    from: tsCollName,
                    startWith: "$join",
                    connectFromField: "join",
                    // Note we must lookup on "meta" because the foreign collection is raw.
                    connectToField: "meta",
                    as: "things",
                },
            },
        ],
        {rawData: true},
    )
    .toArray();
// Joined/looked-up results are in the "things" field.
// Sample and ensure that it has the control field. Indicating the results is still bucketed/raw.
assert.hasFields(resultsGraphLookup[0]["things"][0], ["control"], tojson({erroneousResults: resultsGraphLookup}));
