/**
 * Test that time-series bucket collections support $_requestResumeToken and a subsequent
 * $_resumeAfter.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = "time";

const coll = testDB.getCollection("a");
coll.drop();

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const bucketsColl = testDB.getCollection("system.buckets." + coll.getName());

Random.setRandomSeed();

const numHosts = 10;
const hosts = TimeseriesTest.generateHosts(numHosts);

for (let i = 0; i < 100; i++) {
    const host = TimeseriesTest.getRandomElem(hosts);
    TimeseriesTest.updateUsages(host.fields);

    assert.commandWorked(coll.insert({
        measurement: "cpu",
        time: ISODate(),
        fields: host.fields,
        tags: host.tags,
    },
                                     {ordered: false}));
}

// Run the initial query and request to return a resume token. We're interested only in a single
// document, so 'batchSize' is set to 1.
let res = assert.commandWorked(testDB.runCommand(
    {find: bucketsColl.getName(), hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}));

// Make sure the query returned a resume token which will be used to resume the query from.
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
let resumeToken = res.cursor.postBatchResumeToken;

jsTestLog("Got resume token " + tojson(resumeToken));
assert.neq(null, resumeToken.$recordId);

// Kill the cursor before attempting to resume.
assert.commandWorked(
    testDB.runCommand({killCursors: bucketsColl.getName(), cursors: [res.cursor.id]}));

// Try to resume the query from the saved resume token.
res = assert.commandWorked(testDB.runCommand({
    find: bucketsColl.getName(),
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}));

assert.hasFields(res.cursor, ["postBatchResumeToken"]);
resumeToken = res.cursor.postBatchResumeToken;

jsTestLog("Got resume token " + tojson(resumeToken));
assert.eq(null, resumeToken.$recordId);

// Try to resume from a null '$recordId'.
res = assert.commandWorked(testDB.runCommand({
    find: bucketsColl.getName(),
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}));

assert.hasFields(res.cursor, ["postBatchResumeToken"]);
resumeToken = res.cursor.postBatchResumeToken;

jsTestLog("Got resume token " + tojson(resumeToken));
})();
