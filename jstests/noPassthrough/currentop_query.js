/**
 * Confirms inclusion of query, command object and planSummary in currentOp() for CRUD operations.
 * This test should not be run in the parallel suite as it sets fail points.
 */
(function() {
    "use strict";

    /**
     * @param {Object} params - Configuration options for the test.
     * @param {string} params.readMode - The read mode to use for the parallel shell. This allows
     * testing currentOp() output for both OP_QUERY and OP_GET_MORE queries, as well as "find" and
     * "getMore" commands.
     */
    function runTest(params) {
        var conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
        assert.neq(null, conn, "mongod was unable to start up");

        var testDB = conn.getDB("test");
        assert.commandWorked(testDB.dropDatabase());

        var coll = testDB.currentop_query;

        // Force yield to occur on every PlanExecutor iteration.
        assert.commandWorked(
            testDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

        /**
         * Captures currentOp() for a given test command/operation and confirms that namespace,
         * operation type and planSummary are correct.
         *
         *  @param {Object} testObj - Contains test arguments.
         *  @param {function} testObj.test - A function that runs the desired test op/cmd.
         *  @param {string} testObj.planSummary - A string containing the expected planSummary.
         *  @param {Object} testObj.currentOpFilter - A filter to be used to narrow currentOp()
         *  output to only the relevant operation or command.
         *  @param {string} [testObj.command] - The command to test against. Will look for this to
         *  be a key in the currentOp().query object.
         *  @param {string} [testObj.operation] - The operation to test against. Will look for this
         *  to be the value of the currentOp().op field.
         */
        function confirmCurrentOpContents(testObj) {
            // Force queries to hang on yield to allow for currentOp capture.
            assert.commandWorked(testDB.adminCommand(
                {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));

            // Set shell read mode for the parallel shell test.
            TestData.shellReadMode = params.readMode;
            TestData.currentOpTest = testObj.test;
            testObj.test = function() {
                db.getMongo().forceReadMode(TestData.shellReadMode);
                TestData.currentOpTest();
            };

            // Run query.
            var awaitShell = startParallelShell(testObj.test, conn.port);

            // Capture currentOp record for the query and confirm that the 'query' and 'planSummary'
            // fields contain the content expected. We are indirectly testing the 'ns' field as well
            // with the currentOp query argument.
            assert.soon(
                function() {
                    testObj.currentOpFilter.ns = coll.getFullName();
                    testObj.currentOpFilter.planSummary = testObj.planSummary;
                    if (testObj.hasOwnProperty("command")) {
                        testObj.currentOpFilter["query." + testObj.command] = {$exists: true};
                    } else if (testObj.hasOwnProperty("operation")) {
                        testObj.currentOpFilter.op = testObj.operation;
                    }

                    var result = testDB.currentOp(testObj.currentOpFilter);
                    assert.commandWorked(result);

                    if (result.inprog.length === 1) {
                        assert.eq(result.inprog[0].appName, "MongoDB Shell", tojson(result));

                        return true;
                    }

                    return false;
                },
                function() {
                    return "Failed to find operation from " + tojson(testObj) +
                        " in currentOp() output: " + tojson(testDB.currentOp());
                });

            // Allow the query to complete.
            assert.commandWorked(
                testDB.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

            awaitShell();
            delete TestData.currentOpTest;
            delete TestData.shellReadMode;
        }

        //
        // Confirm currentOp content for commands defined in 'testList'.
        //
        var testList = [
            {
              test: function() {
                  assert.eq(db.currentop_query
                                .aggregate([{$match: {a: 1, $comment: "currentop_query"}}], {
                                    collation: {locale: "fr"},
                                    hint: {_id: 1},
                                    comment: "currentop_query_2"
                                })
                                .itcount(),
                            1);
              },
              command: "aggregate",
              planSummary: "IXSCAN { _id: 1 }",
              currentOpFilter: {
                  "query.pipeline.0.$match.$comment": "currentop_query",
                  "query.comment": "currentop_query_2",
                  "query.collation": {locale: "fr"},
                  "query.hint": {_id: 1}
              }
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.find({a: 1, $comment: "currentop_query"})
                                .collation({locale: "fr"})
                                .count(),
                            1);
              },
              command: "count",
              planSummary: "COLLSCAN",
              currentOpFilter:
                  {"query.query.$comment": "currentop_query", "query.collation": {locale: "fr"}}
            },
            {
              test: function() {
                  assert.eq(
                      db.currentop_query.distinct(
                          "a", {a: 1, $comment: "currentop_query"}, {collation: {locale: "fr"}}),
                      [1]);
              },
              command: "distinct",
              planSummary: "COLLSCAN",
              currentOpFilter:
                  {"query.query.$comment": "currentop_query", "query.collation": {locale: "fr"}}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.find({a: 1}).comment("currentop_query").itcount(),
                            1);
              },
              command: "find",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.findAndModify({
                      query: {a: 1, $comment: "currentop_query"},
                      update: {$inc: {b: 1}},
                      collation: {locale: "fr"}
                  }),
                            {"_id": 1, "a": 1});
              },
              command: "findandmodify",
              planSummary: "COLLSCAN",
              currentOpFilter:
                  {"query.query.$comment": "currentop_query", "query.collation": {locale: "fr"}}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.group({
                      key: {a: 1},
                      cond: {a: 1, $comment: "currentop_query"},
                      reduce: function() {},
                      initial: {},
                      collation: {locale: "fr"}
                  }),
                            [{"a": 1}]);
              },
              command: "group",
              planSummary: "COLLSCAN",
              currentOpFilter: {
                  "query.group.cond.$comment": "currentop_query",
                  "query.group.collation": {locale: "fr"}
              }
            },
            {
              test: function() {
                  assert.commandWorked(db.currentop_query.mapReduce(
                      function() {
                          emit(this.a, this.b);
                      },
                      function(a, b) {
                          return Array.sum(b);
                      },
                      {
                        query: {a: 1, $comment: "currentop_query"},
                        out: {inline: 1},
                        collation: {locale: "fr"}
                      }));
              },
              command: "mapreduce",
              planSummary: "COLLSCAN",
              currentOpFilter:
                  {"query.query.$comment": "currentop_query", "query.collation": {locale: "fr"}}
            },
            {
              test: function() {
                  assert.writeOK(db.currentop_query.remove({a: 2, $comment: "currentop_query"},
                                                           {collation: {locale: "fr"}}));
              },
              operation: "remove",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.$comment": "currentop_query", "collation": {locale: "fr"}}
            },
            {
              test: function() {
                  assert.writeOK(db.currentop_query.update({a: 1, $comment: "currentop_query"},
                                                           {$inc: {b: 1}},
                                                           {collation: {locale: "fr"}}));
              },
              operation: "update",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.$comment": "currentop_query", "collation": {locale: "fr"}}
            }
        ];

        coll.drop();
        var i;
        for (i = 0; i < 5; ++i) {
            assert.writeOK(coll.insert({_id: i, a: i}));
        }

        testList.forEach(confirmCurrentOpContents);

        //
        // Confirm currentOp contains collation for find command.
        //
        if (params.readMode === "commands") {
            confirmCurrentOpContents({
                test: function() {
                    assert.eq(db.currentop_query.find({a: 1})
                                  .comment("currentop_query")
                                  .collation({locale: "fr"})
                                  .itcount(),
                              1);
                },
                command: "find",
                planSummary: "COLLSCAN",
                currentOpFilter:
                    {"query.comment": "currentop_query", "query.collation": {locale: "fr"}}
            });
        }

        //
        // Confirm currentOp content for geoNear.
        //
        coll.drop();
        for (i = 0; i < 10; ++i) {
            assert.writeOK(coll.insert({a: i, loc: {type: "Point", coordinates: [i, i]}}));
        }
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

        confirmCurrentOpContents({
            test: function() {
                assert.commandWorked(db.runCommand({
                    geoNear: "currentop_query",
                    near: {type: "Point", coordinates: [1, 1]},
                    spherical: true,
                    query: {$comment: "currentop_query"},
                    collation: {locale: "fr"}
                }));
            },
            command: "geoNear",
            planSummary: "GEO_NEAR_2DSPHERE { loc: \"2dsphere\" }",
            currentOpFilter:
                {"query.query.$comment": "currentop_query", "query.collation": {locale: "fr"}}
        });

        //
        // Confirm currentOp content for getMore. This case tests command and legacy getMore with an
        // originating find command.
        //
        coll.drop();
        for (i = 0; i < 10; ++i) {
            assert.writeOK(coll.insert({a: i}));
        }

        var cmdRes = testDB.runCommand(
            {find: "currentop_query", filter: {$comment: "currentop_query"}, batchSize: 0});
        assert.commandWorked(cmdRes);

        TestData.commandResult = cmdRes;

        var filter = {
            "query.getMore": TestData.commandResult.cursor.id,
            "originatingCommand.filter.$comment": "currentop_query"
        };

        confirmCurrentOpContents({
            test: function() {
                var cursor = new DBCommandCursor(db.getMongo(), TestData.commandResult, 5);
                assert.eq(cursor.itcount(), 10);
            },
            planSummary: "COLLSCAN",
            currentOpFilter: filter
        });

        delete TestData.commandResult;

        //
        // Confirm that currentOp displays upconverted getMore and originatingCommand in the case of
        // a legacy query.
        //
        if (params.readMode === "legacy") {
            let filter = {
                "query.getMore": {$gt: 0},
                "query.collection": "currentop_query",
                "query.batchSize": 2,
                originatingCommand: {
                    find: "currentop_query",
                    filter: {},
                    ntoreturn: 2,
                    comment: "currentop_query"
                }
            };

            confirmCurrentOpContents({
                test: function() {
                    // Temporarily disable hanging yields so that we can iterate the first batch.
                    assert.commandWorked(
                        db.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

                    let cursor =
                        db.currentop_query.find({}).comment("currentop_query").batchSize(2);

                    // Exhaust the current batch so that the next request will force a getMore.
                    while (cursor.objsLeftInBatch() > 0) {
                        cursor.next();
                    }

                    // Set yields to hang so that we can check currentOp output.
                    assert.commandWorked(db.adminCommand(
                        {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}));

                    assert.eq(cursor.itcount(), 8);
                },
                operation: "getmore",
                planSummary: "COLLSCAN",
                currentOpFilter: filter
            });
        }

        //
        // Confirm ~1000 byte size limit for currentOp query field.
        //
        coll.drop();
        assert.writeOK(coll.insert({a: 1}));

        // When the currentOp command serializes the query object as a string, individual string
        // values inside it are truncated at 150 characters. To test "total length" truncation we
        // need to pass multiple values, each smaller than 150 bytes.
        TestData.queryFilter = {
            "1": "1".repeat(149),
            "2": "2".repeat(149),
            "3": "3".repeat(149),
            "4": "4".repeat(149),
            "5": "5".repeat(149),
            "6": "6".repeat(149),
            "7": "7".repeat(149),
        };

        var truncatedQueryString = "^\\{ find: \"currentop_query\", filter: \\{ " +
            "1: \"1{149}\", 2: \"2{149}\", 3: \"3{149}\", 4: \"4{149}\", 5: \"5{149}\", " +
            "6: \"6{149}\", 7: \"7+\\.\\.\\.";

        confirmCurrentOpContents({
            test: function() {
                assert.eq(db.currentop_query.find(TestData.queryFilter)
                              .comment("currentop_query")
                              .itcount(),
                          0);
            },
            planSummary: "COLLSCAN",
            currentOpFilter: {
                "query.$truncated": {$regex: truncatedQueryString},
                "query.comment": "currentop_query"
            }
        });

        // Verify that an originatingCommand truncated by currentOp appears as { $truncated:
        // <string>, comment: <string> }.
        cmdRes = testDB.runCommand({
            find: "currentop_query",
            filter: TestData.queryFilter,
            comment: "currentop_query",
            batchSize: 0
        });
        assert.commandWorked(cmdRes);

        TestData.commandResult = cmdRes;

        filter = {
            "query.getMore": TestData.commandResult.cursor.id,
            "originatingCommand.$truncated": {$regex: truncatedQueryString},
            "originatingCommand.comment": "currentop_query"
        };

        confirmCurrentOpContents({
            test: function() {
                var cursor = new DBCommandCursor(db.getMongo(), TestData.commandResult, 5);
                assert.eq(cursor.itcount(), 0);
            },
            planSummary: "COLLSCAN",
            currentOpFilter: filter
        });

        delete TestData.commandResult;

        // Verify that an aggregation truncated by currentOp appears as { $truncated: <string>,
        // comment: <string> } when a comment parameter is present.
        truncatedQueryString =
            "^\\{ aggregate: \"currentop_query\", pipeline: \\[ \\{ \\$match: \\{ " +
            "1: \"1{149}\", 2: \"2{149}\", 3: \"3{149}\", 4: \"4{149}\", 5: \"5{149}\", " +
            "6: \"6{149}\", 7: \"7+\\.\\.\\.";

        confirmCurrentOpContents({
            test: function() {
                assert.eq(
                    db.currentop_query
                        .aggregate([{$match: TestData.queryFilter}], {comment: "currentop_query"})
                        .itcount(),
                    0);
            },
            planSummary: "COLLSCAN",
            currentOpFilter: {
                "query.$truncated": {$regex: truncatedQueryString},
                "query.comment": "currentop_query"
            }
        });

        delete TestData.queryFilter;
    }

    runTest({readMode: "commands"});
    runTest({readMode: "legacy"});
})();
