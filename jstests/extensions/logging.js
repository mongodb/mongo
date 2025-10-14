/**
 * Tests that the $log extension stage works end-to-end, populating Info, Warning, and Error log lines.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

const coll = db[jsTestName()];
coll.drop();
const testData = [
    {_id: 0, x: 1},
    {_id: 1, x: 2},
    {_id: 2, x: 3},
];
assert.commandWorked(coll.insertMany(testData));

function testLogStage({
    pipeline,
    expectedInfoLogCount,
    expectedOtherLog: {expectedLogCode, expectedLogMessage, severity} = {},
}) {
    // Clear the logs before running the aggregation.
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    // Run the aggregation with the $log stage.
    assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));

    // First check the correct number of info log lines were printed.
    let infoLogs = checkLog.getFilteredLogMessages(db, 11134004, {}, "I");
    assert.eq(infoLogs.length, expectedInfoLogCount, tojson(infoLogs));
    for (let log of infoLogs) {
        assert.eq(log.msg, "Logging info line for $log");
        assert.eq(log.c, "EXTENSION-MONGOT");
    }

    // Then check any expected non-info log message appear as expected.
    if (expectedLogCode) {
        let otherLogs = checkLog.getFilteredLogMessages(db, expectedLogCode, {}, severity);
        for (let log of otherLogs) {
            assert.eq(log.msg, expectedLogMessage);
            assert.eq(log.c, "EXTENSION-MONGOT");
        }
    }
}

// Parse() is called twice - once when the LiteParsed stage is created, and once when the full
// DocumentSource stage is created. Log lines are printed in both cases.
const parseCallCount = 2;

// Test that the $log stage logs info log lines as expected.
testLogStage({
    pipeline: [{$log: {numInfoLogLines: 1}}],
    expectedInfoLogCount: 1 * parseCallCount,
});
testLogStage({
    pipeline: [{$log: {numInfoLogLines: 3}}],
    expectedInfoLogCount: 3 * parseCallCount,
});
testLogStage({
    pipeline: [{$log: {numInfoLogLines: 5}}],
    expectedInfoLogCount: 5 * parseCallCount,
});

// Test that the $log stage with an empty spec or non-object spec logs an error but no info log.
testLogStage({
    pipeline: [{$log: {}}],
    expectedInfoLogCount: 0,
    expectedOtherLog: {
        expectedLogCode: 11134000,
        expectedLogMessage: "$log stage spec is empty or not an object.",
        expectedLogCount: 1 * parseCallCount,
        severity: "E",
    },
});

// Test that the $log stage with missing numInfoLogLines logs a warning.
testLogStage({
    pipeline: [{$log: {otherField: 1}}],
    expectedInfoLogCount: 0,
    expectedOtherLog: {
        expectedLogCode: 11134001,
        expectedLogMessage: "$log stage missing or invalid numInfoLogLines field.",
        severity: "W",
    },
});

// Test that the $log stage with an numInfoLogLines < 0 logs a warning.
testLogStage({
    pipeline: [{$log: {numInfoLogLines: -1}}],
    expectedInfoLogCount: 0,
    expectedOtherLog: {
        expectedLogCode: 11134002,
        expectedLogMessage: "$log stage must have non-negative value for numInfoLogLines.",
        severity: "W",
    },
});

// Test that the $log stage with an numInfoLogLines > 5 logs a warning and 5 info log lines per
// parse call.
testLogStage({
    pipeline: [{$log: {numInfoLogLines: 99}}],
    expectedInfoLogCount: 5 * parseCallCount,
    expectedOtherLog: {
        expectedLogCode: 11134003,
        expectedLogMessage: "$log stage will not print more than 5 log lines.",
        severity: "W",
    },
});
