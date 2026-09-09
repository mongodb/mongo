/**
 * Shared utilities for tests that check fields in slow query logs.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

/**
 * Finds the slow query log entry identified by `comment` and returns it as a parsed object.
 */
export function findSlowQueryLogLine({db, comment, command}) {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const searchFields = {msg: "Slow query", command: command, comment: comment};
    const line = findMatchingLogLine(globalLog.log, searchFields);
    assert(line, `Failed to find slow query log with fields=${tojson(searchFields)}`);
    return JSON.parse(line);
}

/**
 * Asserts that the slow query log entry identified by `comment` has the expected `collectionType`.
 * Pass `expectedCollType: undefined` to assert that the field is absent.
 */
export function checkCollectionType({db, comment, command, expectedCollType}) {
    const parsed = findSlowQueryLogLine({db, comment, command});
    assert.eq(
        expectedCollType,
        parsed.attr.collectionType,
        `Expected ${expectedCollType === undefined ? "no " : ""}collectionType for comment=${comment}: ${tojson(parsed)}`,
    );
}

/**
 * Asserts that the slow query log entry identified by `comment` has the expected `ns` and
 * `nreturned`, and that it includes a well-formed hexadecimal `queryShapeHash`.
 */
export function checkBasicFields({db, comment, command, expectedNs, expectedNReturned}) {
    const parsed = findSlowQueryLogLine({db, comment, command});
    assert.eq(
        expectedNs,
        parsed.attr.ns,
        `Expected ns=${expectedNs} for comment=${comment}: ${tojson(parsed)}`,
    );
    assert.eq(
        expectedNReturned,
        parsed.attr.nreturned,
        `Expected nreturned=${expectedNReturned} for comment=${comment}: ${tojson(parsed)}`,
    );
    assert(
        parsed.attr.queryShapeHash && /^[0-9a-fA-F]{64}$/.test(parsed.attr.queryShapeHash),
        `Expected a 64-character hex queryShapeHash for comment=${comment}: ${tojson(parsed)}`,
    );
}
