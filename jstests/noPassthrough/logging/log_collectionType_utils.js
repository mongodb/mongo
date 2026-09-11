/**
 * Shared utilities for tests that check fields in slow query logs.
 */

import {findMatchingLogLine} from "jstests/libs/log.js";

/**
 * Finds the slow query log entry identified by `comment` and returns it as a parsed object.
 *
 * At least one of `command` or `type` should be provided to disambiguate the entry of interest.
 * Write commands are logged by the execution engine as an "op" entry (e.g. `type: "update"` /
 * `type: "remove"`) that carries the write metrics and queryShapeHash, in addition to the
 * top-level command entry (whose `ns` is `<db>.$cmd`). Matching on `type` selects the op entry.
 */
export function findSlowQueryLogLine({db, comment, command, type}) {
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const searchFields = {msg: "Slow query", comment: comment};
    if (command !== undefined) {
        searchFields.command = command;
    }
    if (type !== undefined) {
        searchFields.type = type;
    }
    const line = findMatchingLogLine(globalLog.log, searchFields);
    assert(line, `Failed to find slow query log with fields=${tojson(searchFields)}`);
    return JSON.parse(line);
}

/**
 * Asserts that the slow query log entry identified by `comment` has the expected `collectionType`.
 * Pass `expectedCollType: undefined` to assert that the field is absent.
 */
export function checkCollectionType({db, comment, command, type, expectedCollType}) {
    const parsed = findSlowQueryLogLine({db, comment, command, type});
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

/**
 * Asserts fields of a write command slow query log entry identified by `comment`.
 *
 * Options:
 *   - `expectedNs`: the full namespace string.
 *   - `expectedNReturned`: if provided, the exact `nreturned` value. Otherwise, asserts that
 *     `nreturned` is absent from the log entry.
 *   - `expectQueryShapeHash`: if true, asserts a well-formed hexadecimal `queryShapeHash` is
 *     present. If false, asserts that `queryShapeHash` is absent.
 *   - `expectedMetrics`: an object mapping write metric field names to their expected values (e.g.
 *     `{ninserted: 3, nModified: 2}`). Each field is asserted present with the given value.
 */
export function checkWriteFields({
    db,
    comment,
    command,
    type,
    expectedNs,
    expectedNReturned,
    expectQueryShapeHash,
    expectedMetrics,
}) {
    const parsed = findSlowQueryLogLine({db, comment, command, type});
    assert.eq(
        expectedNs,
        parsed.attr.ns,
        `Expected ns=${expectedNs} for comment=${comment}: ${tojson(parsed)}`,
    );
    if (expectedNReturned !== undefined) {
        assert.eq(
            expectedNReturned,
            parsed.attr.nreturned,
            `Expected nreturned=${expectedNReturned} for comment=${comment}: ${tojson(parsed)}`,
        );
    } else {
        assert(
            !parsed.attr.hasOwnProperty("nreturned"),
            `Expected no nreturned for comment=${comment}: ${tojson(parsed)}`,
        );
    }
    if (expectQueryShapeHash) {
        assert(
            parsed.attr.queryShapeHash && /^[0-9a-fA-F]{64}$/.test(parsed.attr.queryShapeHash),
            `Expected a 64-character hex queryShapeHash for comment=${comment}: ${tojson(parsed)}`,
        );
    } else {
        assert(
            !parsed.attr.hasOwnProperty("queryShapeHash"),
            `Expected no queryShapeHash for comment=${comment}: ${tojson(parsed)}`,
        );
    }
    if (expectedMetrics) {
        for (const [field, value] of Object.entries(expectedMetrics)) {
            assert.eq(
                value,
                parsed.attr[field],
                `Expected ${field}=${value} for comment=${comment}: ${tojson(parsed)}`,
            );
        }
    }
}
