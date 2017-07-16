/**
 * This test confirms that after sharding a collection with some pre-existing data,
 * the resulting chunks aren't auto-split too aggressively.
 */
(function() {
    'use strict';

    var s = new ShardingTest({
        name: "shard_existing_coll_chunk_count",
        shards: 1,
        mongos: 1,
        other: {enableAutoSplit: true},
    });

    assert.commandWorked(s.s.adminCommand({enablesharding: "test"}));

    var collNum = 0;
    var overhead = Object.bsonsize({_id: ObjectId(), i: 1, pad: ""});

    var getNumberChunks = function(ns) {
        return s.configRS.getPrimary().getDB("config").getCollection("chunks").count({ns});
    };

    var runCase = function(opts) {
        // Expected options.
        assert.gte(opts.docSize, 0);
        assert.gte(opts.stages.length, 2);

        // Compute padding.
        if (opts.docSize < overhead) {
            var pad = "";
        } else {
            var pad = (new Array(opts.docSize - overhead + 1)).join(' ');
        }

        collNum++;
        var db = s.getDB("test");
        var collName = "coll" + collNum;
        var coll = db.getCollection(collName);
        var i = 0;
        var limit = 0;
        var stageNum = 0;
        var stage = opts.stages[stageNum];

        // Insert initial docs.
        var bulk = coll.initializeUnorderedBulkOp();
        limit += stage.numDocsToInsert;
        for (; i < limit; i++) {
            bulk.insert({i, pad});
        }
        assert.writeOK(bulk.execute());

        // Create shard key index.
        assert.commandWorked(coll.createIndex({i: 1}));

        // Shard collection.
        assert.commandWorked(s.s.adminCommand({shardcollection: coll.getFullName(), key: {i: 1}}));

        // Confirm initial number of chunks.
        var numChunks = getNumberChunks(coll.getFullName());
        assert.eq(numChunks,
                  stage.expectedNumChunks,
                  'in ' + coll.getFullName() + ' expected ' + stage.expectedNumChunks +
                      ' initial chunks, but found ' + numChunks + '\nopts: ' + tojson(opts) +
                      '\nchunks:\n' + s.getChunksString(coll.getFullName()));

        // Do the rest of the stages.
        for (stageNum = 1; stageNum < opts.stages.length; stageNum++) {
            stage = opts.stages[stageNum];

            // Insert the later docs (one at a time, to maximise the autosplit effects).
            limit += stage.numDocsToInsert;
            for (; i < limit; i++) {
                coll.insert({i, pad});
            }

            // Confirm number of chunks for this stage.
            var numChunks = getNumberChunks(coll.getFullName());
            assert.eq(numChunks,
                      stage.expectedNumChunks,
                      'in ' + coll.getFullName() + ' expected ' + stage.expectedNumChunks +
                          ' chunks for stage ' + stageNum + ', but found ' + numChunks +
                          '\nopts: ' + tojson(opts) + '\nchunks:\n' +
                          s.getChunksString(coll.getFullName()));
        }
    };

    // Original problematic case.
    runCase({
        docSize: 0,
        stages: [
            {numDocsToInsert: 20000, expectedNumChunks: 1},
            {numDocsToInsert: 7, expectedNumChunks: 1},
            {numDocsToInsert: 1000, expectedNumChunks: 1},
        ],
    });

    // Original problematic case (worse).
    runCase({
        docSize: 0,
        stages: [
            {numDocsToInsert: 90000, expectedNumChunks: 1},
            {numDocsToInsert: 7, expectedNumChunks: 1},
            {numDocsToInsert: 1000, expectedNumChunks: 1},
        ],
    });

    // Pathological case #1.
    runCase({
        docSize: 522,
        stages: [
            {numDocsToInsert: 8191, expectedNumChunks: 1},
            {numDocsToInsert: 2, expectedNumChunks: 1},
            {numDocsToInsert: 1000, expectedNumChunks: 1},
        ],
    });

    // Pathological case #2.
    runCase({
        docSize: 522,
        stages: [
            {numDocsToInsert: 8192, expectedNumChunks: 1},
            {numDocsToInsert: 8192, expectedNumChunks: 1},
        ],
    });

    // Lower chunksize to 1MB, and restart the mongos for it to take.
    assert.writeOK(
        s.getDB("config").getCollection("settings").update({_id: "chunksize"}, {$set: {value: 1}}, {
            upsert: true
        }));
    s.restartMongos(0);

    // Original problematic case, scaled down to smaller chunksize.
    runCase({
        docSize: 0,
        stages: [
            {numDocsToInsert: 10000, expectedNumChunks: 1},
            {numDocsToInsert: 10, expectedNumChunks: 1},
            {numDocsToInsert: 20, expectedNumChunks: 1},
            {numDocsToInsert: 40, expectedNumChunks: 1},
            {numDocsToInsert: 1000, expectedNumChunks: 1},
        ],
    });

    // Docs just smaller than half chunk size.
    runCase({
        docSize: 510 * 1024,
        stages: [
            {numDocsToInsert: 10, expectedNumChunks: 6},
            {numDocsToInsert: 10, expectedNumChunks: 12},
        ],
    });

    // Docs just larger than half chunk size.
    runCase({
        docSize: 514 * 1024,
        stages: [
            {numDocsToInsert: 10, expectedNumChunks: 10},
            {numDocsToInsert: 10, expectedNumChunks: 20},
        ],
    });

    s.stop();
})();
