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

    function runTest(pipeline) {
        // We use a batch size of 2 to ensure that the mongo shell does not exhaust the cursor on
        // its first batch.
        const batchSize = 2;

        testDB.source.drop();
        assert.writeOK(testDB.source.insert({x: 1}));

        testDB.dest.drop();
        for (let i = 0; i < 5; ++i) {
            assert.writeOK(testDB.dest.insert({x: 1}));
        }

        const res = assert.commandWorked(testDB.runCommand({
            aggregate: 'source',
            pipeline: pipeline,
            cursor: {
                batchSize: batchSize,
            },
        }));

        const cursor = new DBCommandCursor(testDB, res, batchSize);
        cursor.close();  // Closing the cursor will issue the "killCursors" command.

        const serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
        assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus.metrics.cursor));
    }

    runTest([
        {
          $lookup: {
              from: 'dest',
              localField: 'x',
              foreignField: 'x',
              as: 'matches',
          }
        },
        {
          $unwind: {
              path: '$matches',
          },
        },
    ]);

    runTest([
        {
          $lookup: {
              from: 'dest',
              let : {x1: "$x"},
              pipeline: [
                  {$match: {$expr: {$eq: ["$$x1", "$x"]}}},
                  {
                    $lookup: {
                        from: "dest",
                        as: "matches2",
                        let : {x2: "$x"},
                        pipeline: [{$match: {$expr: {$eq: ["$$x2", "$x"]}}}]
                    }
                  },
                  {
                    $unwind: {
                        path: '$matches2',
                    },
                  },
              ],
              as: 'matches1',
          }
        },
        {
          $unwind: {
              path: '$matches1',
          },
        },
    ]);

    MongoRunner.stopMongod(conn);
})();
