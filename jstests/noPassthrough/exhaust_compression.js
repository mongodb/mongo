(function() {
'use strict';

var runTest = function(compressor) {
    var mongo = MongoRunner.runMongod({networkMessageCompressors: compressor});

    let shell = startParallelShell(function() {
        var collName = 'exhaustCollection';
        var fp = 'beforeCompressingExhaustResponse';
        db[collName].drop();

        const kDocumentCount = 10;
        for (var i = 0; i < kDocumentCount; i++) {
            assert.commandWorked(db.runCommand({insert: collName, documents: [{a: i}]}));
        }

        const kBatchSize = 2;
        const preRes =
            assert.commandWorked(db.adminCommand({configureFailPoint: fp, mode: "alwaysOn"}));

        db.exhaustCollection.find({})
            .batchSize(kBatchSize)
            .addOption(DBQuery.Option.exhaust)
            .toArray();

        const postRes =
            assert.commandWorked(db.adminCommand({configureFailPoint: fp, mode: "off"}));

        // The initial response for find command has kBatchSize docs and the remaining docs comes
        // in batches of kBatchSize in response to the getMore command with the exhaustAllowed bit
        // set.
        const kExpectedDelta = Math.floor((kDocumentCount - kBatchSize) / kBatchSize);
        assert.eq(
            postRes.count - preRes.count, kExpectedDelta, "Exhaust messages are not compressed");
    }, mongo.port, false, "--networkMessageCompressors", compressor);

    shell();

    MongoRunner.stopMongod(mongo);
};

runTest("snappy");
}());
