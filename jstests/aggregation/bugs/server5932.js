// server-5932 Cursor-based aggregation

var t = db.server5932;
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
    return new DBCommandCursor(db.getMongo(), cmdOut, followupBatchSize);
}

// both batch sizes are optional
function aggCursor(pipeline, firstBatchSize, followupBatchSize) {
    var cmdOut = db.runCommand(buildAggCmd(pipeline, firstBatchSize));
    assert.commandWorked(cmdOut);

    if (firstBatchSize !== undefined)
        assert.lte(cmdOut.cursor.firstBatch.length, firstBatchSize);

    return makeCursor(cmdOut, followupBatchSize);
}

//
// insert data
//

var bigArray = [];
for (var i = 0; i < 1000; i++)
    bigArray.push(i);

var bigStr = Array(1001).toString();  // 1000 bytes of ','

for (var i = 0; i < 100; i++)
    t.insert({_id: i, bigArray: bigArray, bigStr: bigStr});

//
// do testing
//

// successfully handles results > 16MB (bigArray.length * bytes in bigStr * t.count() == 100MB)
var cursor = aggCursor([{$unwind: '$bigArray'}]);  // default settings
assert.eq(cursor.itcount(), bigArray.length * t.count());
var cursor = aggCursor([{$unwind: '$bigArray'}], 0);  // empty first batch
assert.eq(cursor.itcount(), bigArray.length * t.count());
var cursor = aggCursor([{$unwind: '$bigArray'}], 5, 5);  // many small batches
assert.eq(cursor.itcount(), bigArray.length * t.count());

// empty result set results in cursor.id == 0 unless batchSize is 0;
var res = t.runCommand(buildAggCmd([{$match: {noSuchField: {$exists: true}}}]));
assert.eq(res.cursor.firstBatch, []);
assert.eq(res.cursor.id, 0);
var res = t.runCommand(buildAggCmd([{$match: {noSuchField: {$exists: true}}}], 0));
assert.eq(res.cursor.firstBatch, []);
assert.neq(res.cursor.id, 0);
assert.eq(makeCursor(res).itcount(), 0);

// parse errors are caught before first batch, regardless of size
var res = t.runCommand(buildAggCmd([{$noSuchStage: 1}], 0));
assert.commandFailed(res);

// data dependent errors can get ok:1 but fail in getMore if they don't fail in first batch
var res = t.runCommand(buildAggCmd([{$project: {cantAddString: {$add: [1, '$bigStr']}}}], 1));
assert.commandFailed(res);
var res = t.runCommand(buildAggCmd([{$project: {cantAddString: {$add: [1, '$bigStr']}}}], 0));
assert.commandWorked(res);
assert.throws(function() {
    makeCursor(res).itcount();
});

// error if collection dropped after first batch
var cursor = aggCursor([{$unwind: '$bigArray'}], 0);
t.drop();
assert.throws(function() {
    cursor.itcount();
});
// DON'T ADD NEW TEST TO THIS FILE AFTER THIS ONE (unless you reseed the data)
