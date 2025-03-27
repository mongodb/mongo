/**
 * Tests getMore on cursors opened by reads that used rawData.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations,
 *   known_query_shape_computation_problem,  # TODO (SERVER-101293): Remove this tag.
 *   requires_timeseries,
 *   requires_getmore,
 * ]
 */

const coll = db[jsTestName()];
const bucketsColl = db["system.buckets." + coll.getName()];

const timeField = "t";
const metaField = "m";
const time = new Date("2024-01-01T00:00:00Z");

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(coll.insert([
    {[timeField]: time, [metaField]: 1, a: "a"},
    {[timeField]: time, [metaField]: 1, a: "b"},
    {[timeField]: time, [metaField]: 2, a: "c"},
    {[timeField]: time, [metaField]: 2, a: "d"},
    {[timeField]: time, [metaField]: 3, a: "e"},
    {[timeField]: time, [metaField]: 3, a: "f"},
]));

const expectedDocs = coll.find({"control.count": 2}).rawData().sort({$natural: 1}).toArray();
const aggCur = coll.aggregate(
    [
        {$match: {"control.count": 2}},
    ],
    {cursor: {batchSize: 0}, rawData: true, hint: {$natural: 1}},
);

let getMoreRes = assert.commandWorked(
    db.runCommand({getMore: aggCur.getId(), collection: coll.getName(), batchSize: 1}));
assert.eq(1, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));
assert.eq(expectedDocs[0], getMoreRes.cursor.nextBatch[0]);
assert.neq(0, getMoreRes.cursor.id, tojson(getMoreRes));

getMoreRes = assert.commandWorked(
    db.runCommand({getMore: getMoreRes.cursor.id, collection: coll.getName()}));
assert.eq(2, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));
assert.eq(expectedDocs[1], getMoreRes.cursor.nextBatch[0]);
assert.eq(expectedDocs[2], getMoreRes.cursor.nextBatch[1]);
assert.eq(0, getMoreRes.cursor.id, tojson(getMoreRes));
