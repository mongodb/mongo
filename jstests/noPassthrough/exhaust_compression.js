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

        const preRes =
            assert.commandWorked(db.adminCommand({configureFailPoint: fp, mode: "alwaysOn"}));

        db.exhaustCollection.find({}).batchSize(2).addOption(DBQuery.Option.exhaust).toArray();

        const postRes =
            assert.commandWorked(db.adminCommand({configureFailPoint: fp, mode: "off"}));

        assert.eq(preRes.count + 1, postRes.count, "Exhaust messages are not compressed");
    }, mongo.port, false, "--networkMessageCompressors", compressor);

    shell();

    MongoRunner.stopMongod(mongo);
};

runTest("snappy");
}());
