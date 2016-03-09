/**
 * Tests that the server correctly handles when the OperationContext of the DBDirectClient used by
 * the $lookup stage changes as it unwinds the results.
 *
 * This test was designed to reproduce SERVER-22537.
 */
(function() {
    'use strict';

    // We use a batch size of 1 to ensure that the mongo shell issues a getMore when unwinding the
    // results from the 'dest' collection for the same document in the 'source' collection under a
    // different OperationContext.
    const batchSize = 1;

    db.source.drop();
    db.dest.drop();

    assert.writeOK(db.source.insert({local: 1}));

    // We insert documents in the 'dest' collection such that their combined size is greater than
    // 16MB in order to ensure that the DBDirectClient used by the $lookup stage issues a getMore
    // under a different OperationContext.
    const numMatches = 3;
    const largeStr = new Array(6 * 1024 * 1024 + 1).join('x');

    for (var i = 0; i < numMatches; ++i) {
        assert.writeOK(db.dest.insert({foreign: 1, largeStr: largeStr}));
    }

    var res = db.runCommand({
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

    var cursor = new DBCommandCursor(db.getMongo(), res, batchSize);
    assert.eq(numMatches, cursor.itcount());
})();
