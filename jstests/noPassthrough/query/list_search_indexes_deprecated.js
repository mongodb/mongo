/**
 * Tests that listSearchIndexes logs a deprecation message, rate-limited to once per process.
 * No mongot dependency, tests server-side deprecation logging only.
 */
import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("listSearchIndexes deprecation logging", function () {
    let conn;
    let testDB;
    const collName = jsTestName();

    before(function () {
        conn = MongoRunner.runMongod();
        testDB = conn.getDB(jsTestName());
        assert.commandWorked(testDB.createCollection(collName));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    const deprecationMsg =
        "Use of the listSearchIndexes command is deprecated. Instead use the '$listSearchIndexes' aggregation stage.";

    function getMatchingLogLines() {
        const fieldMatcher = {msg: deprecationMsg};
        const globalLogs = testDB.adminCommand({getLog: "global"});
        return [...iterateMatchingLogLines(globalLogs.log, fieldMatcher)];
    }

    // Runs listSearchIndexes, expects failure since search is not configured.
    function runDeprecatedCommand() {
        assert.commandFailedWithCode(testDB.runCommand({"listSearchIndexes": collName}), 31082);
    }

    it("should not log before first invocation", function () {
        const matchingLogLines = getMatchingLogLines();
        assert.eq(matchingLogLines.length, 0, {matchingLogLines});
    });

    it("should log exactly once after first invocation", function () {
        runDeprecatedCommand();
        const matchingLogLines = getMatchingLogLines();
        assert.eq(matchingLogLines.length, 1, {matchingLogLines});
    });

    it("should not log again on repeated invocation", function () {
        runDeprecatedCommand();
        const matchingLogLines = getMatchingLogLines();
        assert.eq(matchingLogLines.length, 1, {matchingLogLines});
    });

    it("should not log for $listSearchIndexes aggregation stage", function () {
        assert.commandFailedWithCode(
            testDB.runCommand({
                "aggregate": collName,
                pipeline: [{"$listSearchIndexes": {}}],
                cursor: {},
            }),
            31082,
        );
        const matchingLogLines = getMatchingLogLines();
        assert.eq(matchingLogLines.length, 1, {matchingLogLines});
    });
});
