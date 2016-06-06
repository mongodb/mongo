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
                  assert.eq(
                      db.currentop_query.aggregate([{$match: {a: 1, $comment: "currentop_query"}}])
                          .itcount(),
                      1);
              },
              command: "aggregate",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.pipeline.0.$match.$comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.find({a: 1, $comment: "currentop_query"}).count(),
                            1);
              },
              command: "count",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.query.$comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.distinct("a", {a: 1, $comment: "currentop_query"}),
                            [1]);
              },
              command: "distinct",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.query.$comment": "currentop_query"}
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
                  assert.eq(
                      db.currentop_query.findAndModify(
                          {query: {a: 1, $comment: "currentop_query"}, update: {$inc: {b: 1}}}),
                      {"_id": 1, "a": 1});
              },
              command: "findandmodify",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.query.$comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.eq(db.currentop_query.group({
                      key: {a: 1},
                      cond: {a: 1, $comment: "currentop_query"},
                      reduce: function() {},
                      initial: {}
                  }),
                            [{"a": 1}]);
              },
              command: "group",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.group.cond.$comment": "currentop_query"}
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
                      {query: {a: 1, $comment: "currentop_query"}, out: {inline: 1}}));
              },
              command: "mapreduce",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.query.$comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.writeOK(db.currentop_query.remove({a: 2, $comment: "currentop_query"}));
              },
              operation: "remove",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.$comment": "currentop_query"}
            },
            {
              test: function() {
                  assert.writeOK(db.currentop_query.update({a: 1, $comment: "currentop_query"},
                                                           {$inc: {b: 1}}));
              },
              operation: "update",
              planSummary: "COLLSCAN",
              currentOpFilter: {"query.$comment": "currentop_query"}
            }
        ];

        coll.drop();
        var i;
        for (i = 0; i < 5; ++i) {
            assert.writeOK(coll.insert({_id: i, a: i}));
        }

        testList.forEach(confirmCurrentOpContents);

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
                    query: {$comment: "currentop_query"}
                }));
            },
            command: "geoNear",
            planSummary: "GEO_NEAR_2DSPHERE { loc: \"2dsphere\" }",
            currentOpFilter: {"query.query.$comment": "currentop_query"}
        });

        //
        // Confirm currentOp content for getMore.
        //
        coll.drop();
        for (i = 0; i < 10; ++i) {
            assert.writeOK(coll.insert({a: i}));
        }

        var cmdRes = testDB.runCommand(
            {find: "currentop_query", filter: {$comment: "currentop_query"}, batchSize: 0});
        assert.commandWorked(cmdRes);

        TestData.commandResult = cmdRes;

        var filter;
        if (params.readMode === "legacy") {
            filter = {"op": "getmore", "query.filter.$comment": "currentop_query"};
        } else {
            filter = {
                "query.getMore": TestData.commandResult.cursor.id,
                "originatingCommand.filter.$comment": "currentop_query"
            };
        }

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
        // Confirm 512 byte size limit for currentOp query field.
        //
        coll.drop();
        assert.writeOK(coll.insert({a: 1}));

        // When the currentOp command serializes the query object as a string, individual string
        // values inside it are truncated at 150 characters. To test "total length" truncation we
        // need to pass multiple values, each smaller than 150 bytes.
        TestData.queryFilter = {
            "1": "1".repeat(100),
            "2": "2".repeat(100),
            "3": "3".repeat(100),
            "4": "4".repeat(100),
            "5": "5".repeat(100),
            "6": "6".repeat(100),
        };
        var truncatedQueryString = "{ find: \"currentop_query\", filter: { " +
            "1: \"1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111\", " +
            "2: \"2222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222\", " +
            "3: \"3333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333333\", " +
            "4: \"4444444444444444444444444444444444444444444444444444444444444444444444444444444444444444444444444444\", " +
            "5: \"5555555555555555555555555555555555555555...";

        confirmCurrentOpContents({
            test: function() {
                assert.eq(db.currentop_query.find(TestData.queryFilter).itcount(), 0);
            },
            planSummary: "COLLSCAN",
            currentOpFilter: {"query": truncatedQueryString}
        });

        delete TestData.queryFilter;
    }

    runTest({readMode: "commands"});
    runTest({readMode: "legacy"});

})();
