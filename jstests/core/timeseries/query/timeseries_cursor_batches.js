/**
 * This is a regression test to exercise a fix in SBE's BlockToRowStage. Prior to the fix, the below
 * query would trigger a failure due to BlockToRowStage not properly saving its state during
 * a yield initiated by DocumentSourceCursor.
 *
 * @tags: [ requires_timeseries, requires_getmore ]
 */
let coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {"timeseries": {"timeField": "time", "metaField": "tag"}}));

// All of these documents will go in the same bucket.
assert.commandWorked(coll.insertMany([
    {_id: 0, time: new Date("2023-11-03T12:35:19.033Z"), "tag": {t: 1}, "str": "abcdefghijklmnop"},
    {_id: 1, time: new Date("2023-11-03T12:35:19.033Z"), "tag": {t: 1}, "str": ""},
    {_id: 2, time: new Date("2023-11-03T12:35:19.033Z"), "tag": {t: 1}, "str": ""},
]));

// The problematic scenario happens when we are partway through processing a block in
// BlockToRowStage and DocumentSourceCursor initiates a yield when it fills up a batch of results.
// The 'batchSize' setting ensures we will hit this point after the first document is processed and
// a getNext is issued.
const res =
    coll.aggregate([{$match: {"str": {$ne: "hello"}, _id: {$lt: 5}}}, {$project: {_id: 1, str: 1}}],
                   {cursor: {batchSize: 1}})
        .toArray();
assert.eq(res.length, 3, tojson(res));
