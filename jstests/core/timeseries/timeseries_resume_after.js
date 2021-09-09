/**
 * Test that time-series bucket collections support $_requestResumeToken and a subsequent
 * $_resumeAfter.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const timeFieldName = "time";

    const coll = db.timeseries_resume_after;
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    const bucketsColl = db.getCollection("system.buckets." + coll.getName());

    Random.setRandomSeed();

    const numHosts = 10;
    const hosts = TimeseriesTest.generateHosts(numHosts);

    for (let i = 0; i < 100; i++) {
        const host = TimeseriesTest.getRandomElem(hosts);
        TimeseriesTest.updateUsages(host.fields);

        assert.commandWorked(insert(coll, {
            measurement: "cpu",
            time: ISODate(),
            fields: host.fields,
            tags: host.tags,
        }));
    }

    // Run the initial query and request to return a resume token. We're interested only in a single
    // document, so 'batchSize' is set to 1.
    let res = assert.commandWorked(db.runCommand({
        find: bucketsColl.getName(),
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true
    }));

    // Make sure the query returned a resume token which will be used to resume the query from.
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    let resumeToken = res.cursor.postBatchResumeToken;

    jsTestLog("Got resume token " + tojson(resumeToken));
    assert.neq(null, resumeToken.$recordId);

    // Kill the cursor before attempting to resume.
    assert.commandWorked(
        db.runCommand({killCursors: bucketsColl.getName(), cursors: [res.cursor.id]}));

    // Try to resume the query from the saved resume token.
    res = assert.commandWorked(db.runCommand({
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
    res = assert.commandWorked(db.runCommand({
        find: bucketsColl.getName(),
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));

    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    resumeToken = res.cursor.postBatchResumeToken;

    jsTestLog("Got resume token " + tojson(resumeToken));
});
})();
