/**
 * Test that listSearchIndexes logs a deprecation message. It should log whether or not
 * mongot/search is configured, so don't configure search for this test.
 */

import {iterateMatchingLogLines} from "jstests/libs/log.js";

const dbName = jsTestName();
const collName = jsTestName();
const conn = MongoRunner.runMongod();
const testDB = conn.getDB(dbName);

assert.commandWorked(testDB.createCollection(collName));

function getMatchingLogLines() {
    const fieldMatcher = {
        msg:
            "Use of the listSearchIndexes command is deprecated. Instead use the '$listSearchIndexes' aggregation stage."
    };
    const globalLogs = testDB.adminCommand({getLog: 'global'});
    return [...iterateMatchingLogLines(globalLogs.log, fieldMatcher)];
}

function runDeprecatedCommand() {
    // Run listSearchIndexes. Expect the command to fail as we haven't configured search.
    assert.commandFailedWithCode(testDB.runCommand({'listSearchIndexes': collName}), 31082);
}
// Assert that deprecation msg is not logged before map reduce command is even run.
var matchingLogLines = getMatchingLogLines();
assert.eq(matchingLogLines.length, 0, matchingLogLines);

// Run listSearchIndexes. Expect the command to fail as we haven't configured search.
runDeprecatedCommand();

// Check that we logged a deprecation message.
matchingLogLines = getMatchingLogLines();
assert.eq(matchingLogLines.length, 1, matchingLogLines);

// Check that if we run it again, we don't log every time.
runDeprecatedCommand();
matchingLogLines = getMatchingLogLines();
assert.eq(matchingLogLines.length, 1, matchingLogLines);

// Check that the aggregation stage doesn't log.
assert.commandFailedWithCode(
    testDB.runCommand({'aggregate': collName, pipeline: [{"$listSearchIndexes": {}}], cursor: {}}),
    31082);
matchingLogLines = getMatchingLogLines();
assert.eq(matchingLogLines.length, 1, matchingLogLines);

MongoRunner.stopMongod(conn);