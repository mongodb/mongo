/**
 * Shared utilities for tests that check the "collectionType" field in slow query logs.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

/**
 * Asserts that the slow query log entry identified by `comment` has the expected `collectionType`.
 * Pass `expectedCollType: undefined` to assert that the field is absent.
 */
export function checkCollectionType({db, comment, command, expectedCollType}) {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const searchFields = {msg: "Slow query", command: command, comment: comment};
    const line = findMatchingLogLine(globalLog.log, searchFields);
    assert(line, `Failed to find slow query log with fields=${tojson(searchFields)}`);

    const parsed = JSON.parse(line);
    assert.eq(
        expectedCollType,
        parsed.attr.collectionType,
        `Expected ${expectedCollType === undefined ? "no " : ""}collectionType for comment=${comment}: ${line}`,
    );
}
