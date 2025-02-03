/**
 * server-5932 Cursor-based aggregation
 *
 * @tags: [
 *   # The result set produced by this test is large, so when wrapped in a $facet, the maximum
 *   # intermediate document size would be exceeded.
 *   do_not_wrap_aggregations_in_facets,
 *   # This test will not work with causal consistency because an aggregate and its subsequent
 *   # getMores act as one operation, which means that there are no guarantees that future cursor
 *   # commands will read any writes which occur in between cursor commands.
 *   does_not_support_causal_consistency,
 * ]
 */

let t = db[jsTestName()];
t.drop();

//
// define helpers
//

// batch size is optional
function buildAggCmd(pipeline, batchSize) {
    return {
        aggregate: t.getName(),
        pipeline: pipeline,
        cursor: (batchSize === undefined ? {} : {batchSize: batchSize}),
    };
}

// batch size is optional
function makeCursor(cmdOut, followupBatchSize) {
    assert.commandWorked(cmdOut);
    assert.neq(cmdOut.cursor.id, undefined);
    assert(cmdOut.cursor.id instanceof NumberLong);
    assert(cmdOut.cursor.firstBatch instanceof Array);
    return new DBCommandCursor(db, cmdOut, followupBatchSize);
}

// both batch sizes are optional
function aggCursor(pipeline, firstBatchSize, followupBatchSize) {
    const cmdOut = db.runCommand(buildAggCmd(pipeline, firstBatchSize));
    assert.commandWorked(cmdOut);

    if (firstBatchSize !== undefined)
        assert.lte(cmdOut.cursor.firstBatch.length, firstBatchSize);

    return makeCursor(cmdOut, followupBatchSize);
}

//
// insert data
//

let bigArray = [];
for (let i = 0; i < 1000; i++)
    bigArray.push(i);

let bigStr = Array(1001).toString();  // 1000 bytes of ','

for (let i = 0; i < 100; i++)
    t.insert({_id: i, bigArray: bigArray, bigStr: bigStr});

//
// do testing
//

// successfully handles results > 16MB (bigArray.length * bytes in bigStr * t.count() == 100MB)
let cursor = aggCursor([{$unwind: '$bigArray'}]);  // default settings
assert.eq(cursor.itcount(), bigArray.length * t.count());
cursor = aggCursor([{$unwind: '$bigArray'}], 0);  // empty first batch
assert.eq(cursor.itcount(), bigArray.length * t.count());
cursor = aggCursor([{$unwind: '$bigArray'}], 5, 5);  // many small batches
assert.eq(cursor.itcount(), bigArray.length * t.count());

// empty result set results in cursor.id == 0 unless batchSize is 0;
let res = t.runCommand(buildAggCmd([{$match: {noSuchField: {$exists: true}}}]));
assert.eq(res.cursor.firstBatch, []);
assert.eq(res.cursor.id, 0);
res = t.runCommand(buildAggCmd([{$match: {noSuchField: {$exists: true}}}], 0));
assert.eq(res.cursor.firstBatch, []);
assert.neq(res.cursor.id, 0);
assert.eq(makeCursor(res).itcount(), 0);

// parse errors are caught before first batch, regardless of size
res = t.runCommand(buildAggCmd([{$noSuchStage: 1}], 0));
assert.commandFailed(res);

// data dependent errors can get ok:1 but fail in getMore if they don't fail in first batch
res = t.runCommand(buildAggCmd([{$project: {cantAddString: {$add: [1, '$bigStr']}}}], 1));
assert.commandFailed(res);

// Setting batchSize 0 doesn't guarantee that command will succeed: it may fail during plan
// selection.
res = t.runCommand(buildAggCmd([{$project: {cantAddString: {$add: [1, '$bigStr']}}}], 0));
if (res.ok) {
    assert.throws(function() {
        makeCursor(res).itcount();
    });
}

// error if collection dropped after first batch
cursor = aggCursor([{$unwind: '$bigArray'}], 0);
t.drop();
assert.throws(function() {
    cursor.itcount();
});
