/**
 * Tests that the server correctly handles when the OperationContext of the DBDirectClient used by
 * the $lookup stage changes as it unwinds the results.
 *
 * This test was designed to reproduce SERVER-22537.
 */
(function() {
    'use strict';

    // We use a batch size of 2 to ensure that the mongo shell issues a getMore when unwinding the
    // results from the 'dest' collection for the same document in the 'source' collection under a
    // different OperationContext.
    const batchSize = 2;

    const conn =
        MongoRunner.runMongod({setParameter: "internalAggregationLookupBatchSize=" + batchSize});
    assert.neq(null, conn, "mongod failed to start up");
    const testDB = conn.getDB("test");

    testDB.source.drop();
    testDB.dest.drop();

    assert.writeOK(testDB.source.insert({local: 1}));

    // The cursor batching logic actually requests one more document than it needs to fill the
    // first batch, so if we want to leave the $lookup stage paused with a cursor open we'll
    // need two more matching documents than the batch size.
    const numMatches = batchSize + 2;
    for (var i = 0; i < numMatches; ++i) {
        assert.writeOK(testDB.dest.insert({foreign: 1}));
    }

    var res = testDB.runCommand({
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
    });
    assert.commandWorked(res);

    var cursor = new DBCommandCursor(conn, res, batchSize);
    assert.eq(numMatches, cursor.itcount());

    assert.eq(0, MongoRunner.stopMongod(conn), "expected mongod to shutdown cleanly");
})();
