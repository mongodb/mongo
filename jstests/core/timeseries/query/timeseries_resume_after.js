/**
 * Test that time-series collections support $_requestResumeToken with both
 * $_resumeAfter and $_startAt tokens over the raw bucket data.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: killCursors.
 *   not_allowed_with_signed_security_token,
 *   # Queries on mongoS may not request or provide a resume token.
 *   assumes_against_mongod_not_mongos,
 *   # Resuming may not work properly with stepdowns.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   cannot_run_during_upgrade_downgrade,
 *   requires_fcv_81,
 *   # cannot be run if fuzzer config is set
 *   does_not_support_config_fuzzer
 * ]
 */
import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const timeFieldName = "time";

    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    Random.setRandomSeed();

    const numHosts = 10;
    const hosts = TimeseriesTest.generateHosts(numHosts);

    // Insert some data so that the backing system collection has a single bucket.
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
    assert.eq(1,
              getTimeseriesCollForRawOps(coll).count({}, kRawOperationSpec),
              "The following tests rely on having a single bucket");

    function testResumeToken(tokenType) {
        // Run the initial query and request to return a resume token.
        let res = assert.commandWorked(db.runCommand({
            find: getTimeseriesCollForRawOps(coll).getName(),
            hint: {$natural: 1},
            batchSize: 1,
            $_requestResumeToken: true,
            ...kRawOperationSpec,
        }));
        assert.neq([], res.cursor.firstBatch, "Expect some data to be returned");

        // Make sure the query returned a resume token.
        assert.hasFields(res.cursor, ["postBatchResumeToken"]);
        let resumeToken = res.cursor.postBatchResumeToken;
        assert.neq(null, resumeToken.$recordId, "Got resume token " + tojson(resumeToken));

        // Kill the cursor before attempting to resume.
        assert.commandWorked(db.runCommand(
            {killCursors: getTimeseriesCollForRawOps(coll).getName(), cursors: [res.cursor.id]}));

        // Try to resume the query from the saved resume token.
        let resumeCmd = {
            find: getTimeseriesCollForRawOps(coll).getName(),
            hint: {$natural: 1},
            batchSize: 1,
            $_requestResumeToken: true,
            ...kRawOperationSpec,
        };
        resumeCmd[tokenType] = resumeToken;

        res = assert.commandWorked(db.runCommand(resumeCmd));
        assert.eq([], res.cursor.firstBatch, "Expect no more data returned");
        assert.hasFields(res.cursor, ["postBatchResumeToken"]);
        resumeToken = res.cursor.postBatchResumeToken;

        // After a collection is exhausted, the record id in the resume token is set to null.
        assert.eq(null, resumeToken.$recordId, "Got resume token " + tojson(resumeToken));

        // Try to resume from a null '$recordId'
        resumeCmd[tokenType] = resumeToken;
        assert.commandFailedWithCode(db.runCommand(resumeCmd), ErrorCodes.KeyNotFound);

        // Test that resuming fails if the recordId is Long.
        resumeCmd[tokenType] = {'$recordId': NumberLong(10)};
        assert.commandFailedWithCode(db.runCommand(resumeCmd), 7738600);

        // Test that resuming fails if querying the time-series collection without rawData.
        let viewCmd =
            {find: coll.getName(), filter: {}, $_requestResumeToken: true, hint: {$natural: 1}};
        viewCmd[tokenType] = {'$recordId': BinData(5, '1234')};
        assert.commandFailedWithCode(db.runCommand(viewCmd), ErrorCodes.InvalidPipelineOperator);
    }

    testResumeToken('$_resumeAfter');
    testResumeToken('$_startAt');
});
