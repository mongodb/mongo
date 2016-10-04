// SERVER-22599 Test behavior of in-memory storage engine with full cache.
(function() {
    'use strict';

    if (jsTest.options().storageEngine !== "inMemory") {
        jsTestLog("Skipping test because storageEngine is not inMemory");
        return;
    }

    Random.setRandomSeed();

    // Return array of approximately 1kB worth of random numbers.
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
        return doc;
    }

    // Return an array with random documents totalling about 1Mb.
    function randomBatch(batchSize) {
        var batch = [];
        for (var j = 0; j < batchSize; j++)
            batch[j] = randomDoc();
        return batch;
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
    for (var j = 0; j < 1000; j++) {
        res = t.insert(batch);
        assert.gte(res.nInserted, 0, tojson(res));
        count += res.nInserted;
        if (res.hasErrors())
            break;
        assert.eq(res.nInserted, batchSize, tojson(res));
        print("Inserted " + count + " documents");
    }
    assert.writeError(res, "didn't get ExceededMemoryLimit but should have");
    print("Inserted " + count + " documents");

    // Should have encountered exactly one memory full error.
    assert.eq(res.getWriteErrorCount(), 1, tojson(res));
    assert.eq(res.getWriteErrorAt(0).code, ErrorCodes.ExceededMemoryLimit, tojson(res));

    // Should encounter memory full at between 75% and 150% of total capacity.
    assert.gt(count * docSizeKB, cacheKB * 0.75, "inserted data size is at least 75% of capacity");
    assert.lt(count * docSizeKB, cacheKB * 1.50, "inserted data size is at most 150% of capacity");

    // Indexes are sufficiently large that it should be impossible to add a new one.
    assert.commandFailedWithCode(t.createIndex({a: 1}), ErrorCodes.ExceededMemoryLimit);

    // An aggregate copying all 'a' and 'b' fields should run out of memory.
    // Can't test the specific error code, because it depends on whether the collection
    // creation already fails, or just the writing. Agg wraps the original error code.
    assert.commandFailed(
        t.runCommand("aggregate", {pipeline: [{$project: {a: 1, b: 1}}, {$out: "test.out"}]}));

    // Should still be able to query.
    assert.eq(t.find({}).itcount(), count, "cannot find expected number of documents");
    assert.eq(t.aggregate([{$group: {_id: null, count: {$sum: 1}}}]).next().count,
              count,
              "cannot aggregate expected number of documents");
}());
