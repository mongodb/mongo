/**
 * Tests that the cursor underlying the $lookup stage is killed when the cursor returned to the
 * client for the aggregation pipeline is killed.
 *
 * This test was designed to reproduce SERVER-24386.
 */
(function() {
    'use strict';

    const options = {setParameter: 'internalDocumentSourceCursorBatchSizeBytes=1'};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

    const testDB = conn.getDB('test');

    // We use a batch size of 2 to ensure that the mongo shell does not exhaust the cursor on its
    // first batch.
    const batchSize = 2;
    const numMatches = 5;

    assert.writeOK(testDB.source.insert({local: 1}));
    for (let i = 0; i < numMatches; ++i) {
        assert.writeOK(testDB.dest.insert({foreign: 1}));
    }

    const res = assert.commandWorked(testDB.runCommand({
        aggregate: 'source',
        pipeline: [
            {
              $lookup: {
                  from: 'dest',
                  localField: 'local',
                  foreignField: 'foreign',
                  as: 'matches',
              }
            },
            {
              $unwind: {
                  path: '$matches',
              },
            },
        ],
        cursor: {
            batchSize: batchSize,
        },
    }));

    const cursor = new DBCommandCursor(conn, res, batchSize);
    cursor.close();  // Closing the cursor will issue the "killCursors" command.

    const serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));

    MongoRunner.stopMongod(conn);
})();
