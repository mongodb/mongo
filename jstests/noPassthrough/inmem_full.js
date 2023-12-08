// SERVER-22599 Test behavior of in-memory storage engine with full cache.
if (jsTest.options().storageEngine !== "inMemory") {
    jsTestLog("Skipping test because storageEngine is not inMemory");
    quit();
}

Random.setRandomSeed();

// Return array of approximately 680 bytes worth of random numbers.
// This size was chosen to make `randomDoc()` ~10kB.
function randomArray() {
    var arr = [];
    for (var j = 0; j < 85; j++)
        arr[j] = Random.rand();
    return arr;
}

// Return a document of approximately 10kB in size with arrays of random numbers.
function randomDoc() {
    var doc = {};
    for (var c of "abcdefghij")
        doc[c] = randomArray();
    const docSizeKB = Object.bsonsize(doc) / 1024;
    assert.lte(9.9, docSizeKB, "randomDoc() should be at least 9.9kB");
    assert.gte(10, docSizeKB, "randomDoc() should be no more than 10kB");
    return doc;
}

// Return an array with random documents totalling about 1Mb.
function randomBatch(batchSize) {
    var batch = [];
    for (var j = 0; j < batchSize; j++)
        batch[j] = randomDoc();
    return batch;
}

function randomBlob(sizeInKB) {
    var s = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    var b = Array(sizeInKB * 1024)
                .join()
                .split(',')
                .map(function() {
                    return s.charAt(Math.floor(Random.rand() * s.length));
                })
                .join('');
    return b;
}

const cacheMB = 128;
const cacheKB = 1024 * cacheMB;
const docSizeKB = Object.bsonsize(randomDoc()) / 1024;
const batchSize = 100;
const batch = randomBatch(batchSize);

var mongod = MongoRunner.runMongod({
    storageEngine: 'inMemory',
    inMemoryEngineConfigString: 'cache_size=' + cacheMB + "M,",
});
assert.neq(null, mongod, "mongod failed to started up with --inMemoryEngineConfigString");
var db = mongod.getDB("test");
var t = db.large;

// Insert documents until full.
var res;
var count = 0;
for (var j = 0; j < 2 /* The size should never be off by more than a factor of 2. */ * cacheKB /
         (docSizeKB * batchSize);
     j++) {
    res = t.insert(batch);
    assert.gte(res.nInserted, 0, tojson(res));
    count += res.nInserted;
    if (res.hasErrors())
        break;
    assert.eq(res.nInserted, batchSize, tojson(res));
    print("Inserted " + count + " documents");
}
assert.writeError(res, "didn't get ExceededMemoryLimit but should have");
print(`Inserted ${count} documents`);

// Should have encountered exactly one memory full error.
assert.eq(res.getWriteErrorCount(), 1, tojson(res));
assert.eq(res.getWriteErrorAt(0).code, ErrorCodes.ExceededMemoryLimit, tojson(res));

// Should encounter memory full at between 75% and 150% of total capacity.
assert.gt(count * docSizeKB, cacheKB * 0.75, "inserted data size is at least 75% of capacity");
assert.lt(count * docSizeKB, cacheKB * 1.50, "inserted data size is at most 150% of capacity");

// Adding a large field to all existing documents should run out of memory.
// This is the repro reported in SERVER-22599.
const blob = randomBlob(docSizeKB);
assert.commandFailedWithCode(t.update({}, {$set: {blob: blob}}, false, true),
                             ErrorCodes.ExceededMemoryLimit);

// Indexes are sufficiently large that it should be impossible to add a new one.
assert.commandFailedWithCode(t.createIndex({a: 1}), ErrorCodes.ExceededMemoryLimit);

// An aggregate copying all 'a' and 'b' fields should run out of memory.
// Can't test the specific error code, because it depends on whether the collection
// creation already fails, or just the writing. Agg wraps the original error code.
// The `cursor` option became required in 3.6 unless you use `explain`; see
// https://www.mongodb.com/docs/manual/reference/command/aggregate/#dbcmd.aggregate
assert.commandFailedWithCode(
    t.runCommand("aggregate",
                 {pipeline: [{$project: {a: 1, b: 1}}, {$out: "test.out"}], cursor: {}}),
    ErrorCodes.ExceededMemoryLimit);

// Should still be able to query.
assert.eq(t.find({}).itcount(), count, "cannot find expected number of documents");
assert.eq(t.aggregate([{$group: {_id: null, count: {$sum: 1}}}]).next().count,
          count,
          "cannot aggregate expected number of documents");

MongoRunner.stopMongod(mongod);
