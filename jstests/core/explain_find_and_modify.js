/**
 * Test correctness of explaining findAndModify. Asserts the following:
 *
 * 1. Explaining findAndModify should never create a database.
 * 2. Explaining findAndModify should never create a collection.
 * 3. Explaining findAndModify should not work with an invalid findAndModify command object.
 * 4. Explaining findAndModify should not modify any contents of the collection.
 * 5. The reported stats should reflect how the command would be executed.
 */
(function() {
    "use strict";
    var cName = "explain_find_and_modify";
    var t = db.getCollection(cName);

    // Different types of findAndModify explain requests.
    var explainRemove = {explain: {findAndModify: cName, remove: true, query: {_id: 0}}};
    var explainUpdate = {explain: {findAndModify: cName, update: {$inc: {i: 1}}, query: {_id: 0}}};
    var explainUpsert = {
        explain: {findAndModify: cName, update: {$inc: {i: 1}}, query: {_id: 0}, upsert: true}
    };

    // 1. Explaining findAndModify should never create a database.

    // Make sure this one doesn't exist before we start.
    assert.commandWorked(db.getSiblingDB(cName).runCommand({dropDatabase: 1}));
    var newDB = db.getSiblingDB(cName);

    // Explain the command, ensuring the database is not created.
    var err_msg = "Explaining findAndModify on a non-existent database should return an error.";
    assert.commandFailed(newDB.runCommand(explainRemove), err_msg);
    assertDBDoesNotExist(newDB, "Explaining a remove should not create a database.");

    assert.commandFailed(newDB.runCommand(explainUpsert), err_msg);
    assertDBDoesNotExist(newDB, "Explaining an upsert should not create a database.");

    // 2. Explaining findAndModify should never create a collection.

    // Insert a document to make sure the database exists.
    t.insert({'will': 'be dropped'});
    // Make sure the collection doesn't exist.
    t.drop();

    // Explain the command, ensuring the collection is not created.
    assert.commandWorked(db.runCommand(explainRemove));
    assertCollDoesNotExist(cName, "explaining a remove should not create a new collection.");

    assert.commandWorked(db.runCommand(explainUpsert));
    assertCollDoesNotExist(cName, "explaining an upsert should not create a new collection.");

    assert.commandWorked(db.runCommand(Object.merge(explainUpsert, {fields: {x: 1}})));
    assertCollDoesNotExist(cName, "explaining an upsert should not create a new collection.");

    // 3. Explaining findAndModify should not work with an invalid findAndModify command object.

    // Specifying both remove and new is illegal.
    assert.commandFailed(db.runCommand({remove: true, new: true}));

    // 4. Explaining findAndModify should not modify any contents of the collection.
    var onlyDoc = {_id: 0, i: 1};
    assert.writeOK(t.insert(onlyDoc));

    // Explaining a delete should not delete anything.
    var matchingRemoveCmd = {findAndModify: cName, remove: true, query: {_id: onlyDoc._id}};
    var res = db.runCommand({explain: matchingRemoveCmd});
    assert.commandWorked(res);
    assert.eq(t.find().itcount(), 1, "Explaining a remove should not remove any documents.");

    // Explaining an update should not update anything.
    var matchingUpdateCmd = {findAndModify: cName, update: {x: "x"}, query: {_id: onlyDoc._id}};
    var res = db.runCommand({explain: matchingUpdateCmd});
    assert.commandWorked(res);
    assert.eq(t.findOne(), onlyDoc, "Explaining an update should not update any documents.");

    // Explaining an upsert should not insert anything.
    var matchingUpsertCmd =
        {findAndModify: cName, update: {x: "x"}, query: {_id: "non-match"}, upsert: true};
    var res = db.runCommand({explain: matchingUpsertCmd});
    assert.commandWorked(res);
    assert.eq(t.find().itcount(), 1, "Explaining an upsert should not insert any documents.");

    // 5. The reported stats should reflect how it would execute and what it would modify.
    var isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;

    // List out the command to be explained, and the expected results of that explain.
    var testCases = [
        // -------------------------------------- Removes ----------------------------------------
        {
          // Non-matching remove command.
          cmd: {remove: true, query: {_id: "no-match"}},
          expectedResult: {
              executionStats: {
                  nReturned: 0,
                  executionSuccess: true,
                  executionStages: {stage: "DELETE", nWouldDelete: 0}
              }
          }
        },
        {
          // Matching remove command.
          cmd: {remove: true, query: {_id: onlyDoc._id}},
          expectedResult: {
              executionStats: {
                  nReturned: 1,
                  executionSuccess: true,
                  executionStages: {stage: "DELETE", nWouldDelete: 1}
              }
          }
        },
        // -------------------------------------- Updates ----------------------------------------
        {
          // Non-matching update query.
          cmd: {update: {$inc: {i: 1}}, query: {_id: "no-match"}},
          expectedResult: {
              executionStats: {
                  nReturned: 0,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 0, wouldInsert: false}
              }
          }
        },
        {
          // Non-matching update query, returning new doc.
          cmd: {update: {$inc: {i: 1}}, query: {_id: "no-match"}, new: true},
          expectedResult: {
              executionStats: {
                  nReturned: 0,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 0, wouldInsert: false}
              }
          }
        },
        {
          // Matching update query.
          cmd: {update: {$inc: {i: 1}}, query: {_id: onlyDoc._id}},
          expectedResult: {
              executionStats: {
                  nReturned: 1,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 1, wouldInsert: false}
              }
          }
        },
        {
          // Matching update query, returning new doc.
          cmd: {update: {$inc: {i: 1}}, query: {_id: onlyDoc._id}, new: true},
          expectedResult: {
              executionStats: {
                  nReturned: 1,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 1, wouldInsert: false}
              }
          }
        },
        // -------------------------------------- Upserts ----------------------------------------
        {
          // Non-matching upsert query.
          cmd: {update: {$inc: {i: 1}}, upsert: true, query: {_id: "no-match"}},
          expectedResult: {
              executionStats: {
                  nReturned: 0,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 0, wouldInsert: true}
              }
          }
        },
        {
          // Non-matching upsert query, returning new doc.
          cmd: {update: {$inc: {i: 1}}, upsert: true, query: {_id: "no-match"}, new: true},
          expectedResult: {
              executionStats: {
                  nReturned: 1,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 0, wouldInsert: true}
              }
          }
        },
        {
          // Matching upsert query, returning new doc.
          cmd: {update: {$inc: {i: 1}}, upsert: true, query: {_id: onlyDoc._id}, new: true},
          expectedResult: {
              executionStats: {
                  nReturned: 1,
                  executionSuccess: true,
                  executionStages: {stage: "UPDATE", nWouldModify: 1, wouldInsert: false}
              }
          }
        }
    ];

    // Apply all the same test cases, this time adding a projection stage.
    testCases = testCases.concat(testCases.map(function makeProjection(testCase) {
        return {
            cmd: Object.merge(testCase.cmd, {fields: {i: 0}}),
            expectedResult: {
                executionStats: {
                    // nReturned Shouldn't change.
                    nReturned: testCase.expectedResult.executionStats.nReturned,
                    executionStages: {
                        stage: "PROJECTION",
                        transformBy: {i: 0},
                        // put previous root stage under projection stage.
                        inputStage: testCase.expectedResult.executionStats.executionStages
                    }
                }
            }
        };
    }));
    // Actually assert on the test cases.
    testCases.forEach(function(testCase) {
        assertExplainMatchedAllVerbosities(testCase.cmd, testCase.expectedResult);
    });

    // ----------------------------------------- Helpers -----------------------------------------

    /**
     * Helper to make this test work in the sharding passthrough suite.
     *
     * Transforms the explain output so that if it came from a mongos, it will be modified
     * to have the same format as though it had come from a mongod.
     */
    function transformIfSharded(explainOut) {
        if (!isMongos) {
            return explainOut;
        }

        // Asserts that the explain command ran on a single shard and modifies the given
        // explain output to have a top-level UPDATE or DELETE stage by removing the
        // top-level SINGLE_SHARD stage.
        function replace(outerKey, innerKey) {
            assert(explainOut.hasOwnProperty(outerKey));
            assert(explainOut[outerKey].hasOwnProperty(innerKey));

            var shardStage = explainOut[outerKey][innerKey];
            assert.eq("SINGLE_SHARD", shardStage.stage);
            assert.eq(1, shardStage.shards.length);
            Object.extend(explainOut[outerKey], shardStage.shards[0], false);
        }

        replace("queryPlanner", "winningPlan");
        replace("executionStats", "executionStages");

        return explainOut;
    }

    /**
     * Assert the results from running the explain match the expected results.
     *
     * Since we aren't expecting a perfect match (we only specify a subset of the fields we expect
     * to match), recursively go through the expected results, and make sure each one has a
     * corresponding field on the actual results, and that their values match.
     * Example doc for expectedMatches:
     * {executionStats: {nReturned: 0, executionStages: {isEOF: 1}}}
     */
    function assertExplainResultsMatch(explainOut, expectedMatches, preMsg, currentPath) {
        // This is only used recursively, to keep track of where we are in the document.
        var isRootLevel = typeof currentPath === "undefined";
        Object.keys(expectedMatches).forEach(function(key) {
            var totalFieldName = isRootLevel ? key : currentPath + "." + key;
            assert(explainOut.hasOwnProperty(key),
                   preMsg + "Explain's output does not have a value for " + key);
            if (typeof expectedMatches[key] === "object") {
                // Sub-doc, recurse to match on it's fields
                assertExplainResultsMatch(
                    explainOut[key], expectedMatches[key], preMsg, totalFieldName);
            } else {
                assert.eq(explainOut[key],
                          expectedMatches[key],
                          preMsg + "Explain's " + totalFieldName + " (" + explainOut[key] + ")" +
                              " does not match expected value (" + expectedMatches[key] + ").");
            }
        });
    }

    /**
     * Assert that running explain on the given findAndModify command matches the expected results,
     * on all the different verbosities (but just assert the command worked on the lowest verbosity,
     * since it doesn't have any useful stats).
     */
    function assertExplainMatchedAllVerbosities(findAndModifyArgs, expectedResult) {
        ["queryPlanner", "executionStats", "allPlansExecution"].forEach(function(verbosityMode) {
            var cmd = {
                explain: Object.merge({findAndModify: cName}, findAndModifyArgs),
                verbosity: verbosityMode
            };
            var msg = "Error after running command: " + tojson(cmd) + ": ";
            var explainOut = db.runCommand(cmd);
            assert.commandWorked(explainOut, "command: " + tojson(cmd));
            // Don't check explain results for queryPlanner mode, as that doesn't have any of the
            // interesting stats.
            if (verbosityMode !== "queryPlanner") {
                explainOut = transformIfSharded(explainOut);
                assertExplainResultsMatch(explainOut, expectedResult, msg);
            }
        });
    }

    function assertDBDoesNotExist(db, msg) {
        assert.eq(db.getMongo().getDBNames().indexOf(db.getName()),
                  -1,
                  msg + "db " + db.getName() + " exists.");
    }

    function assertCollDoesNotExist(cName, msg) {
        assert.eq(
            db.getCollectionNames().indexOf(cName), -1, msg + "collection " + cName + " exists.");
    }
})();
