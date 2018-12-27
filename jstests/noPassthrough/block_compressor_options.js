/**
 * Tests using different combinations of --wiredTigerCollectionBlockCompressor and
 * --wiredTigerJournalCompressor.
 *
 * Using the collection block compressor option will result in all new collections made during
 * that process lifetime to use that compression setting. WiredTiger perfectly supports different
 * tables using different block compressors. This test will start up MongoDB once for each block
 * compressor setting and a create a new collection. Then after all collections are created, check
 * creation string passed to WT via the collStats command.
 *
 * WiredTiger also supports changing the compression setting for writes to the journal. This tests
 * that the setting can be changed between clean restarts, but otherwise does not verify the
 * journal compression behavior.
 *
 * @tags: [requires_persistence,requires_wiredtiger]
 */
(function() {
    'use strict';

    // On the first iteration, start a mongod. Subsequent iterations will close and restart on the
    // same dbpath.
    let firstIteration = true;
    let compressors = ['none', 'snappy', 'zlib', 'zstd'];
    let mongo;
    for (let compressor of compressors) {
        jsTestLog({"Starting with compressor": compressor});
        if (firstIteration) {
            mongo = MongoRunner.runMongod({
                wiredTigerCollectionBlockCompressor: compressor,
                wiredTigerJournalCompressor: compressor
            });
            firstIteration = false;
        } else {
            MongoRunner.stopMongod(mongo);
            mongo = MongoRunner.runMongod({
                restart: true,
                dbpath: mongo.dbpath,
                cleanData: false,
                wiredTigerCollectionBlockCompressor: compressor
            });
        }
        mongo.getDB('db')[compressor].insert({});
    }

    for (let compressor of compressors) {
        jsTestLog({"Asserting collection compressor": compressor});
        let stats = mongo.getDB('db')[compressor].stats();
        assert(stats['wiredTiger']['creationString'].search('block_compressor=' + compressor) > -1);
    }

    MongoRunner.stopMongod(mongo);
}());
