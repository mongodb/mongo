/**
 * Tests getMore on cursors opened by reads that used rawData.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";

const coll = db[jsTestName()];

const timeField = "t";
const metaField = "m";
const time = new Date("2024-01-01T00:00:00Z");

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(
    coll.insert([
        {[timeField]: time, [metaField]: 1, a: "a"},
        {[timeField]: time, [metaField]: 1, a: "b"},
        {[timeField]: time, [metaField]: 2, a: "c"},
        {[timeField]: time, [metaField]: 2, a: "d"},
        {[timeField]: time, [metaField]: 3, a: "e"},
        {[timeField]: time, [metaField]: 3, a: "f"},
    ]),
);

const expectedDocs = getTimeseriesCollForRawOps(coll)
    .find({"control.count": 2})
    .rawData()
    .sort({$natural: 1})
    .toArray();
const aggCur = getTimeseriesCollForRawOps(coll).aggregate([{$match: {"control.count": 2}}], {
    cursor: {batchSize: 0},
    ...kRawOperationSpec,
    hint: {$natural: 1},
});

let getMoreRes = assert.commandWorked(
    db.runCommand({getMore: aggCur.getId(), collection: getTimeseriesCollForRawOps(coll).getName(), batchSize: 1}),
);
assert.eq(1, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));
assert.eq(expectedDocs[0], getMoreRes.cursor.nextBatch[0]);
assert.neq(0, getMoreRes.cursor.id, tojson(getMoreRes));

getMoreRes = assert.commandWorked(
    db.runCommand({getMore: getMoreRes.cursor.id, collection: getTimeseriesCollForRawOps(coll).getName()}),
);
assert.eq(2, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));
assert.eq(expectedDocs[1], getMoreRes.cursor.nextBatch[0]);
assert.eq(expectedDocs[2], getMoreRes.cursor.nextBatch[1]);
assert.eq(0, getMoreRes.cursor.id, tojson(getMoreRes));
