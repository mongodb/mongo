/**
 * Tests that the server correctly handles when the OperationContext used by the $lookup stage
 * changes as it unwinds the results.
 *
 * This test was designed to reproduce SERVER-22537.
 */
(function() {
    'use strict';

    const options = {setParameter: 'internalDocumentSourceCursorBatchSizeBytes=1'};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

    const testDB = conn.getDB('test');

    // We use a batch size of 2 to ensure that the mongo shell issues a getMore when unwinding the
    // results from the 'dest' collection for the same document in the 'source' collection under a
    // different OperationContext.
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
    assert.eq(numMatches, cursor.itcount());

    MongoRunner.stopMongod(conn);
})();
