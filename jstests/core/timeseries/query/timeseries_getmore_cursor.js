/**
 * Verifies that cursor.ns in both initial and getMore responses for rawData operations on
 * timeseries collections returns the user-addressed namespace.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_getmore,
 *   assumes_no_implicit_cursor_exhaustion,
 *   # The cursor.ns fix for rawData operations was introduced in 8.3 (SERVER-120476).
 *   multiversion_incompatible,
 * ]
 */
import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";

const coll = db[jsTestName()];
const timeField = "t";
const metaField = "m";

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

const baseTime = new Date("2026-01-01T00:00:00Z");
let docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({
        [timeField]: new Date(baseTime.getTime() + i * 60000),
        [metaField]: i % 4,
        value: i,
    });
}
assert.commandWorked(coll.insertMany(docs));

const rawColl = getTimeseriesCollForRawOps(coll);
const expectedNs = rawColl.getFullName();

function assertCursorNs(response, label) {
    assert.eq(
        response.cursor.ns,
        expectedNs,
        `${label}: expected cursor.ns '${expectedNs}', got '${response.cursor.ns}'`,
    );
}

function assertRawBucketDoc(doc) {
    assert.hasFields(doc, ["control", "meta"]);
    assert(!doc.hasOwnProperty(timeField), `Expected raw bucket doc without '${timeField}' field, got: ${tojson(doc)}`);
    assert(!doc.hasOwnProperty(metaField), `Expected raw bucket doc without '${metaField}' field, got: ${tojson(doc)}`);
}

/**
 * Tests that cursor.ns is correct for both the initial command response and getMore,
 * and that raw bucket documents are returned.
 */
function testQueryCursorNs(cmdObj, label) {
    jsTest.log.info(`Testing ${label} cursor.ns and data format on ${rawColl.getName()}`);

    const res = assert.commandWorked(db.runCommand(cmdObj));
    assertCursorNs(res, `${label} initial`);
    assert.gt(res.cursor.firstBatch.length, 0);
    assertRawBucketDoc(res.cursor.firstBatch[0]);

    assert.neq(res.cursor.id, 0, `Expected open cursor from ${label}`);
    const getMoreRes = assert.commandWorked(
        db.runCommand({getMore: res.cursor.id, collection: rawColl.getName(), batchSize: 1}),
    );
    assertCursorNs(getMoreRes, `${label} getMore`);
    assert.gt(getMoreRes.cursor.nextBatch.length, 0);
    assertRawBucketDoc(getMoreRes.cursor.nextBatch[0]);

    if (getMoreRes.cursor.id != 0) {
        assert.commandWorked(db.runCommand({killCursors: rawColl.getName(), cursors: [getMoreRes.cursor.id]}));
    }
}

// Test find + getMore cursor.ns with rawData.
testQueryCursorNs({find: rawColl.getName(), batchSize: 1, ...kRawOperationSpec}, "find");

// Test aggregate + getMore cursor.ns with rawData.
testQueryCursorNs(
    {aggregate: rawColl.getName(), pipeline: [], cursor: {batchSize: 1}, ...kRawOperationSpec},
    "aggregate",
);

// Test listIndexes + getMore cursor.ns with rawData.
{
    assert.commandWorked(coll.createIndex({[timeField]: 1}));
    assert.commandWorked(coll.createIndex({value: 1}));
    assert.commandWorked(coll.createIndex({[metaField]: 1, value: -1}));

    jsTest.log.info(`Testing listIndexes cursor.ns and data format on ${rawColl.getName()}`);

    const listRes = assert.commandWorked(
        db.runCommand({listIndexes: rawColl.getName(), cursor: {batchSize: 1}, ...kRawOperationSpec}),
    );
    assertCursorNs(listRes, "listIndexes initial");

    // Collect indexes from firstBatch and getMore separately so we can verify getMore
    // actually returned data.
    const firstBatchIndexes = [...listRes.cursor.firstBatch];
    assert.eq(firstBatchIndexes.length, 1, "Expected exactly 1 index in firstBatch with batchSize: 1");
    assert.neq(listRes.cursor.id, 0, "Expected open cursor from listIndexes with batchSize: 1");

    const getMoreRes = assert.commandWorked(
        db.runCommand({getMore: listRes.cursor.id, collection: rawColl.getName(), batchSize: 100}),
    );
    assertCursorNs(getMoreRes, "listIndexes getMore");
    assert.gt(getMoreRes.cursor.nextBatch.length, 0, "Expected getMore to return remaining indexes");

    const allIndexes = firstBatchIndexes.concat(getMoreRes.cursor.nextBatch);

    // Verify raw bucket index key patterns are returned
    const allKeys = allIndexes.map((idx) => idx.key);
    const expectedKeys = [
        {"meta": 1, "control.min.t": 1, "control.max.t": 1},
        {"control.min.t": 1, "control.max.t": 1},
        {"control.min.value": 1, "control.max.value": 1},
        {"meta": 1, "control.max.value": -1, "control.min.value": -1},
    ];

    assert.sameMembers(expectedKeys, allKeys, `listIndexes with rawData returned unexpected index keys`);
}

// Test exhausted with rawData, verify system.buckets is not leaked in cursor.ns
{
    const findRes = assert.commandWorked(
        db.runCommand({find: rawColl.getName(), batchSize: 100, ...kRawOperationSpec}),
    );
    assertCursorNs(findRes);
    assert.eq(findRes.cursor.id, 0);
    const numBuckets = findRes.cursor.firstBatch.length;
    assert.gt(numBuckets, 0);

    const aggRes = assert.commandWorked(
        db.runCommand({aggregate: rawColl.getName(), pipeline: [], cursor: {batchSize: 100}, ...kRawOperationSpec}),
    );
    assertCursorNs(aggRes);
    assert.eq(aggRes.cursor.id, 0);
    assert.eq(aggRes.cursor.firstBatch.length, numBuckets);

    const listRes = assert.commandWorked(
        db.runCommand({listIndexes: rawColl.getName(), cursor: {batchSize: 100}, ...kRawOperationSpec}),
    );
    assertCursorNs(listRes);
    assert.eq(listRes.cursor.id, 0);
    assert.eq(listRes.cursor.firstBatch.length, 4);
}

// Test exhausted cursors without rawData: verify system.buckets is not leaked in cursor.ns
{
    const userNs = coll.getFullName();

    const findRes = assert.commandWorked(db.runCommand({find: coll.getName(), batchSize: 100}));
    assert.eq(findRes.cursor.ns, userNs);
    assert.eq(findRes.cursor.id, 0);
    assert.eq(findRes.cursor.firstBatch.length, 10);

    const aggRes = assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 100}}),
    );
    assert.eq(aggRes.cursor.ns, userNs);
    assert.eq(aggRes.cursor.id, 0);
    assert.eq(aggRes.cursor.firstBatch.length, 10);
}
