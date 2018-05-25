/**
 * Confirms inclusion of query, command object and planSummary in currentOp() for CRUD operations.
 * This test should not be run in the parallel suite as it sets fail points.
 * @tags: [requires_replication, requires_sharding]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

    // Set up a 2-shard cluster. Configure 'internalQueryExecYieldIterations' on both shards such
    // that operations will yield on each PlanExecuter iteration.
    const st = new ShardingTest({
        name: jsTestName(),
        shards: 2,
        rs: {nodes: 1, setParameter: {internalQueryExecYieldIterations: 1}}
    });

    // Obtain one mongoS connection and a second direct to the shard.
    const rsConn = st.rs0.getPrimary();
    const mongosConn = st.s;

    const mongosDB = mongosConn.getDB("currentop_query");
    const mongosColl = mongosDB.currentop_query;

    // Enable sharding on the the test database and ensure that the primary is on shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), rsConn.name);

    // On a sharded cluster, aggregations which are dispatched to multiple shards first establish
    // zero-batch cursors and only hit the failpoints on the following getMore. This helper takes a
    // generic command object and creates an appropriate filter given the use-case.
    function commandOrOriginatingCommand(cmdObj, isRemoteShardCurOp) {
        const cmdFieldName = (isRemoteShardCurOp ? "originatingCommand" : "command");
        const cmdFilter = {};
        for (let subFieldName in cmdObj) {
            cmdFilter[`${cmdFieldName}.${subFieldName}`] = cmdObj[subFieldName];
        }
        return cmdFilter;
    }

    // Drops and re-creates the sharded test collection.
    function dropAndRecreateTestCollection() {
        assert(mongosColl.drop());
        assert.commandWorked(mongosDB.adminCommand(
            {shardCollection: mongosColl.getFullName(), key: {_id: "hashed"}}));
    }

    /**
     * @param {connection} conn - The connection through which to run the test suite.
     * @param {string} readMode - The read mode to use for the parallel shell. This allows
     * testing currentOp() output for both OP_QUERY and OP_GET_MORE queries, as well as "find" and
     * "getMore" commands.
     * @params {function} currentOp - Function which takes a database object and a filter, and
     * returns an array of matching current operations. This allows us to test output for both the
     * currentOp command and the $currentOp aggregation stage.
     * @params {boolean} truncatedOps - if true, we expect operations that exceed the maximum
     * currentOp size to be truncated in the output 'command' field, and we run only a subset of
     * tests designed to exercise that scenario. If false, we expect the entire operation to be
     * returned.
     * @params {boolean} localOps - if true, we expect currentOp to return operations running on a
     * mongoS itself rather than on the shards.
     */
    function runTests({conn, readMode, currentOp, truncatedOps, localOps}) {
        const testDB = conn.getDB("currentop_query");
        const coll = testDB.currentop_query;
        dropAndRecreateTestCollection();

        for (let i = 0; i < 5; ++i) {
            assert.writeOK(coll.insert({_id: i, a: i}));
        }

        const isLocalMongosCurOp = (FixtureHelpers.isMongos(testDB) && localOps);
        const isRemoteShardCurOp = (FixtureHelpers.isMongos(testDB) && !localOps);

        // If 'truncatedOps' is true, run only the subset of tests designed to validate the
        // truncation behaviour. Otherwise, run the standard set of tests which assume that
        // truncation will not occur.
        if (truncatedOps) {
            runTruncationTests();
        } else {
            runStandardTests();
        }

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
            FixtureHelpers.runCommandOnEachPrimary({
                db: conn.getDB("admin"),
                cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}
            });

            // Set the test configuration in TestData for the parallel shell test.
            TestData.shellReadMode = readMode;
            TestData.currentOpTest = testObj.test;
            TestData.currentOpCollName = "currentop_query";

            // Wrapper function which sets the readMode and DB before running the test function
            // found at TestData.currentOpTest.
            function doTest() {
                const testDB = db.getSiblingDB(TestData.currentOpCollName);
                testDB.getMongo().forceReadMode(TestData.shellReadMode);
                TestData.currentOpTest(testDB);
            }

            // Run the operation in the background.
            var awaitShell = startParallelShell(doTest, testDB.getMongo().port);

            // Augment the currentOpFilter with additional known predicates.
            if (!testObj.currentOpFilter.ns) {
                testObj.currentOpFilter.ns = coll.getFullName();
            }
            if (!isLocalMongosCurOp) {
                testObj.currentOpFilter.planSummary = testObj.planSummary;
            }
            if (testObj.hasOwnProperty("command")) {
                testObj.currentOpFilter["command." + testObj.command] = {$exists: true};
            } else if (testObj.hasOwnProperty("operation")) {
                testObj.currentOpFilter.op = testObj.operation;
            }

            // Capture currentOp record for the query and confirm that the 'query' and 'planSummary'
            // fields contain the content expected. We are indirectly testing the 'ns' field as well
            // with the currentOp query argument.
            assert.soon(
                function() {
                    var result = currentOp(testDB, testObj.currentOpFilter, truncatedOps, localOps);
                    assert.commandWorked(result);

                    if (result.inprog.length > 0) {
                        result.inprog.forEach((op) => {
                            assert.eq(op.appName, "MongoDB Shell", tojson(result));
                            assert.eq(op.clientMetadata.application.name,
                                      "MongoDB Shell",
                                      tojson(result));
                        });
                        return true;
                    }

                    return false;
                },
                function() {
                    return "Failed to find operation from " + tojson(testObj.currentOpFilter) +
                        " in currentOp() output: " +
                        tojson(currentOp(testDB, {}, truncatedOps, localOps)) +
                        (isLocalMongosCurOp
                             ? ", with localOps=false: " +
                                 tojson(currentOp(testDB, {}, truncatedOps, false))
                             : "");
                });

            // Allow the query to complete.
            FixtureHelpers.runCommandOnEachPrimary({
                db: conn.getDB("admin"),
                cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "off"}
            });

            awaitShell();
            delete TestData.currentOpCollName;
            delete TestData.currentOpTest;
            delete TestData.shellReadMode;
        }

        /**
         * Runs a set of tests to verify that the currentOp output appears as expected. These tests
         * assume that the 'truncateOps' parameter is false, so no command objects in the currentOp
         * output will be truncated to string.
         */
        function runStandardTests() {
            //
            // Confirm currentOp content for commands defined in 'testList'.
            //
            var testList = [
                {
                  test: function(db) {
                      assert.eq(db.currentop_query
                                    .aggregate([{$match: {a: 1, $comment: "currentop_query"}}], {
                                        collation: {locale: "fr"},
                                        hint: {_id: 1},
                                        comment: "currentop_query_2"
                                    })
                                    .itcount(),
                                1);
                  },
                  planSummary: "IXSCAN { _id: 1 }",
                  currentOpFilter: commandOrOriginatingCommand({
                      "aggregate": {$exists: true},
                      "pipeline.0.$match.$comment": "currentop_query",
                      "comment": "currentop_query_2",
                      "collation": {locale: "fr"},
                      "hint": {_id: 1}
                  },
                                                               isRemoteShardCurOp)
                },
                {
                  test: function(db) {
                      assert.eq(db.currentop_query.find({a: 1, $comment: "currentop_query"})
                                    .collation({locale: "fr"})
                                    .count(),
                                1);
                  },
                  command: "count",
                  planSummary: "COLLSCAN",
                  currentOpFilter: {
                      "command.query.$comment": "currentop_query",
                      "command.collation": {locale: "fr"}
                  }
                },
                {
                  test: function(db) {
                      assert.eq(db.currentop_query.distinct("a",
                                                            {a: 1, $comment: "currentop_query"},
                                                            {collation: {locale: "fr"}}),
                                [1]);
                  },
                  command: "distinct",
                  planSummary: "COLLSCAN",
                  currentOpFilter: {
                      "command.query.$comment": "currentop_query",
                      "command.collation": {locale: "fr"}
                  }
                },
                {
                  test: function(db) {
                      assert.eq(
                          db.currentop_query.find({a: 1}).comment("currentop_query").itcount(), 1);
                  },
                  command: "find",
                  planSummary: "COLLSCAN",
                  currentOpFilter: {"command.comment": "currentop_query"}
                },
                {
                  test: function(db) {
                      assert.eq(db.currentop_query.findAndModify({
                          query: {_id: 1, a: 1, $comment: "currentop_query"},
                          update: {$inc: {b: 1}},
                          collation: {locale: "fr"}
                      }),
                                {"_id": 1, "a": 1});
                  },
                  command: "findandmodify",
                  planSummary: "IXSCAN { _id: 1 }",
                  currentOpFilter: {
                      "command.query.$comment": "currentop_query",
                      "command.collation": {locale: "fr"}
                  }
                },
                {
                  test: function(db) {
                      assert.commandWorked(
                          db.currentop_query.mapReduce(() => {},
                                                       (a, b) => {},
                                                       {
                                                         query: {$comment: "currentop_query"},
                                                         out: {inline: 1},
                                                       }));
                  },
                  command: "mapreduce",
                  planSummary: "COLLSCAN",
                  currentOpFilter: {
                      "command.query.$comment": "currentop_query",
                      "ns": /^currentop_query.*currentop_query/
                  }
                },
                {
                  test: function(db) {
                      assert.writeOK(db.currentop_query.remove({a: 2, $comment: "currentop_query"},
                                                               {collation: {locale: "fr"}}));
                  },
                  operation: "remove",
                  planSummary: "COLLSCAN",
                  currentOpFilter:
                      (isLocalMongosCurOp
                           ? {"command.delete": coll.getName(), "command.ordered": true}
                           : {
                               "command.q.$comment": "currentop_query",
                               "command.collation": {locale: "fr"}
                             })
                },
                {
                  test: function(db) {
                      assert.writeOK(
                          db.currentop_query.update({a: 1, $comment: "currentop_query"},
                                                    {$inc: {b: 1}},
                                                    {collation: {locale: "fr"}, multi: true}));
                  },
                  operation: "update",
                  planSummary: "COLLSCAN",
                  currentOpFilter:
                      (isLocalMongosCurOp
                           ? {"command.update": coll.getName(), "command.ordered": true}
                           : {
                               "command.q.$comment": "currentop_query",
                               "command.collation": {locale: "fr"}
                             })
                }
            ];

            testList.forEach(confirmCurrentOpContents);

            //
            // Confirm currentOp contains collation for find command.
            //
            if (readMode === "commands") {
                confirmCurrentOpContents({
                    test: function(db) {
                        assert.eq(db.currentop_query.find({a: 1})
                                      .comment("currentop_query")
                                      .collation({locale: "fr"})
                                      .itcount(),
                                  1);
                    },
                    command: "find",
                    planSummary: "COLLSCAN",
                    currentOpFilter: {
                        "command.comment": "currentop_query",
                        "command.collation": {locale: "fr"}
                    }
                });
            }

            //
            // Confirm currentOp content for geoNear.
            //
            dropAndRecreateTestCollection();
            for (let i = 0; i < 10; ++i) {
                assert.writeOK(coll.insert({a: i, loc: {type: "Point", coordinates: [i, i]}}));
            }
            assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

            confirmCurrentOpContents({
                test: function(db) {
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
                currentOpFilter: {
                    "command.query.$comment": "currentop_query",
                    "command.collation": {locale: "fr"}
                }
            });

            //
            // Confirm currentOp content for getMore. This case tests command and legacy getMore
            // with originating find and aggregate commands.
            //
            dropAndRecreateTestCollection();
            for (let i = 0; i < 10; ++i) {
                assert.writeOK(coll.insert({a: i}));
            }

            const originatingCommands = {
                find:
                    {find: "currentop_query", filter: {}, comment: "currentop_query", batchSize: 0},
                aggregate: {
                    aggregate: "currentop_query",
                    pipeline: [{$match: {}}],
                    comment: "currentop_query",
                    cursor: {batchSize: 0}
                }
            };

            for (let cmdName in originatingCommands) {
                const cmdObj = originatingCommands[cmdName];
                const cmdRes = testDB.runCommand(cmdObj);
                assert.commandWorked(cmdRes);

                TestData.commandResult = cmdRes;

                // If this is a non-localOps test running via mongoS, then the cursorID we obtained
                // above is the ID of the mongoS cursor, and will not match the IDs of any of the
                // individual shard cursors in the currentOp output. We therefore don't perform an
                // exact match on 'command.getMore', but only verify that the cursor ID is non-zero.
                const filter = {
                    "command.getMore":
                        (isRemoteShardCurOp ? {$gt: 0} : TestData.commandResult.cursor.id),
                    [`originatingCommand.${cmdName}`]:
                        {$exists: true}, "originatingCommand.comment": "currentop_query"
                };

                confirmCurrentOpContents({
                    test: function(db) {
                        const cursor = new DBCommandCursor(db, TestData.commandResult, 5);
                        assert.eq(cursor.itcount(), 10);
                    },
                    command: "getMore",
                    planSummary: "COLLSCAN",
                    currentOpFilter: filter
                });

                delete TestData.commandResult;
            }

            //
            // Confirm that currentOp displays upconverted getMore and originatingCommand in the
            // case of a legacy query.
            //
            if (readMode === "legacy") {
                let filter = {
                    "command.getMore": {$gt: 0},
                    "command.collection": "currentop_query",
                    "command.batchSize": 2,
                    "originatingCommand.find": "currentop_query",
                    "originatingCommand.ntoreturn": 2,
                    "originatingCommand.comment": "currentop_query"
                };

                confirmCurrentOpContents({
                    test: function(db) {
                        load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

                        // Temporarily disable hanging yields so that we can iterate the first
                        // batch.
                        FixtureHelpers.runCommandOnEachPrimary({
                            db: db.getSiblingDB("admin"),
                            cmdObj: {configureFailPoint: "setYieldAllLocksHang", mode: "off"}
                        });

                        let cursor =
                            db.currentop_query.find({}).comment("currentop_query").batchSize(2);

                        // Exhaust the current batch so that the next request will force a getMore.
                        while (cursor.objsLeftInBatch() > 0) {
                            cursor.next();
                        }

                        // Set yields to hang so that we can check currentOp output.
                        FixtureHelpers.runCommandOnEachPrimary({
                            db: db.getSiblingDB("admin"),
                            cmdObj:
                                {configureFailPoint: "setYieldAllLocksHang", mode: "alwaysOn"}
                        });

                        assert.eq(cursor.itcount(), 8);
                    },
                    operation: "getmore",
                    planSummary: "COLLSCAN",
                    currentOpFilter: filter
                });
            }

            //
            // Confirm that a legacy query whose filter contains a field named 'query' appears as
            // expected in currentOp. This test ensures that upconverting a legacy query correctly
            // identifies this as a user field rather than a wrapped filter spec.
            //
            if (readMode === "legacy") {
                confirmCurrentOpContents({
                    test: function(db) {
                        assert.eq(
                            db.currentop_query.find({query: "foo", $comment: "currentop_query"})
                                .itcount(),
                            0);
                    },
                    command: "find",
                    planSummary: "COLLSCAN",
                    currentOpFilter: {
                        "command.filter.$comment": "currentop_query",
                        "command.filter.query": "foo"
                    }
                });
            }
        }

        /**
         * Runs a set of tests to verify that currentOp will serialize objects exceeding ~1000 bytes
         * to string when the 'truncateOps' parameter is set.
         */
        function runTruncationTests() {
            dropAndRecreateTestCollection();
            assert.writeOK(coll.insert({a: 1}));

            // When the currentOp command serializes the query object as a string, individual string
            // values inside it are truncated at 150 characters. To test "total length" truncation
            // we need to pass multiple values, each smaller than 150 bytes.
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

            let currentOpFilter;

            currentOpFilter = {
                "command.$truncated": {$regex: truncatedQueryString},
                "command.comment": "currentop_query"
            };

            confirmCurrentOpContents({
                test: function(db) {
                    assert.eq(db.currentop_query.find(TestData.queryFilter)
                                  .comment("currentop_query")
                                  .itcount(),
                              0);
                },
                planSummary: "COLLSCAN",
                currentOpFilter: currentOpFilter
            });

            // Verify that an originatingCommand truncated by currentOp appears as { $truncated:
            // <string>, comment: <string> }.
            const cmdRes = testDB.runCommand({
                find: "currentop_query",
                filter: TestData.queryFilter,
                comment: "currentop_query",
                batchSize: 0
            });
            assert.commandWorked(cmdRes);

            TestData.commandResult = cmdRes;

            currentOpFilter = {
                "command.getMore":
                    (isRemoteShardCurOp ? {$gt: 0} : TestData.commandResult.cursor.id),
                "originatingCommand.$truncated": {$regex: truncatedQueryString},
                "originatingCommand.comment": "currentop_query"
            };

            confirmCurrentOpContents({
                test: function(db) {
                    var cursor = new DBCommandCursor(db, TestData.commandResult, 5);
                    assert.eq(cursor.itcount(), 0);
                },
                planSummary: "COLLSCAN",
                currentOpFilter: currentOpFilter
            });

            delete TestData.commandResult;

            // Verify that an aggregation truncated by currentOp appears as { $truncated: <string>,
            // comment: <string> } when a comment parameter is present.
            truncatedQueryString =
                "^\\{ aggregate: \"currentop_query\", pipeline: \\[ \\{ \\$match: \\{ " +
                "1: \"1{149}\", 2: \"2{149}\", 3: \"3{149}\", 4: \"4{149}\", 5: \"5{149}\", " +
                "6: \"6{149}\", 7: \"7+\\.\\.\\.";

            currentOpFilter = commandOrOriginatingCommand(
                {"$truncated": {$regex: truncatedQueryString}, "comment": "currentop_query"},
                isRemoteShardCurOp);

            confirmCurrentOpContents({
                test: function(db) {
                    assert.eq(db.currentop_query
                                  .aggregate([{$match: TestData.queryFilter}],
                                             {comment: "currentop_query"})
                                  .itcount(),
                              0);
                },
                planSummary: "COLLSCAN",
                currentOpFilter: currentOpFilter
            });

            delete TestData.queryFilter;
        }
    }

    function currentOpCommand(inputDB, filter, truncatedOps, localOps) {
        return inputDB.currentOp(Object.assign(filter, {$truncateOps: truncatedOps}));
    }

    function currentOpAgg(inputDB, filter, truncatedOps, localOps) {
        return {
            inprog: inputDB.getSiblingDB("admin")
                        .aggregate([
                            {
                              $currentOp: {
                                  localOps: (localOps || false),
                                  truncateOps: (truncatedOps || false)
                              }
                            },
                            {$match: filter}
                        ])
                        .toArray(),
            ok: 1
        };
    }

    for (let connType of[rsConn, mongosConn]) {
        for (let readMode of["commands", "legacy"]) {
            for (let truncatedOps of[false, true]) {
                for (let localOps of[false, true]) {
                    // Run all tests using the $currentOp aggregation stage.
                    runTests({
                        conn: connType,
                        readMode: readMode,
                        currentOp: currentOpAgg,
                        localOps: localOps,
                        truncatedOps: truncatedOps
                    });
                }
                // Run tests using the currentOp command. The 'localOps' parameter is not supported.
                runTests({
                    conn: connType,
                    readMode: readMode,
                    currentOp: currentOpCommand,
                    localOps: false,
                    truncatedOps: truncatedOps
                });
            }
        }
    }

    st.stop();
})();
