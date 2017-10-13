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

    /**
     * Executes an aggregrate with 'options.pipeline' and confirms that 'options.numResults' were
     * returned.
     */
    function runTest(options) {
        // The batchSize must be smaller than the number of documents returned by the $lookup. This
        // ensures that the mongo shell will issue a getMore when unwinding the $lookup results for
        // the same document in the 'source' collection, under a different OperationContext.
        const batchSize = 2;

        testDB.source.drop();
        assert.writeOK(testDB.source.insert({x: 1}));

        testDB.dest.drop();
        for (let i = 0; i < 5; ++i) {
            assert.writeOK(testDB.dest.insert({x: 1}));
        }

        const res = assert.commandWorked(testDB.runCommand({
            aggregate: 'source',
            pipeline: options.pipeline,
            cursor: {
                batchSize: batchSize,
            },
        }));

        const cursor = new DBCommandCursor(testDB, res, batchSize);
        assert.eq(options.numResults, cursor.itcount());
    }

    runTest({
        pipeline: [
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
        ],
        numResults: 5
    });

    runTest({
        pipeline: [
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
        ],
        numResults: 25
    });

    MongoRunner.stopMongod(conn);
})();
