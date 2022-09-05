/**
 * Confirms that log output for each operation adheres to the expected, consistent format, including
 * query/write metrics where applicable, on both mongoD and mongoS.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

// This test looks for exact matches in log output, which does not account for implicit
// sessions.
TestData.disableImplicitSessions = true;

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
load("jstests/libs/log.js");              // For findMatchingLogLine.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.

// Prevent the mongo shell from gossiping its cluster time, since this will increase the amount
// of data logged for each op. For some of the testcases below, including the cluster time would
// cause them to be truncated at the 512-byte RamLog limit, and some of the fields we need to
// check would be lost.
TestData.skipGossipingClusterTime = true;

// Set up a 2-shard single-node replicaset cluster.
const stParams = {
    name: jsTestName(),
    shards: 2,
    rs: {nodes: 1}
};
const st = new ShardingTest(stParams);

// Obtain one mongoS connection and a second direct to the shard.
const shardConn = st.rs0.getPrimary();
const mongosConn = st.s;

const dbName = "logtest";

const mongosDB = mongosConn.getDB(dbName);
const shardDB = shardConn.getDB(dbName);

// Enable sharding on the the test database and ensure that the primary is on shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), shardConn.name);

// Drops and re-shards the test collection, then splits at {_id: 0} and moves the upper chunk to
// the second shard.
function dropAndRecreateTestCollection() {
    assert(mongosDB.test.drop());
    st.shardColl(mongosDB.test, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName(), true);
}

// Configures logging parameters on the target environment, constructs a list of test operations
// depending on the deployment type, runs each of these in turn, and searches the logs for the
// corresponding output. Returns a pair of arrays [testsRun, logLines]; the former is the set of
// test cases that were run, while the latter contains the logline for each test, or null if no
// such logline was found.
function runLoggingTests({db, slowMs, logLevel, sampleRate}) {
    dropAndRecreateTestCollection();

    const coll = db.test;

    for (let i = 1; i <= 5; ++i) {
        assert.commandWorked(
            coll.insert({_id: i, a: i, loc: {type: "Point", coordinates: [i, i]}}));
        assert.commandWorked(
            coll.insert({_id: -i, a: -i, loc: {type: "Point", coordinates: [-i, -i]}}));
    }
    assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

    const isMongos = FixtureHelpers.isMongos(db);

    // Build a string that identifies the parameters of this test run. Individual ops will
    // use this string as their comment where applicable, and we also print it to the logs.
    const logFormatTestComment = (isMongos ? 'mongos' : 'mongod') + "_slowms:" + slowMs +
        "_logLevel:" + logLevel + "_sampleRate:" + sampleRate;
    jsTestLog(logFormatTestComment);

    // Set all logging parameters. If slowMs is null, we set a high threshold here so that
    // logLevel can be tested in cases where operations should not otherwise be logged.
    assert.commandWorked(db.adminCommand(
        {profile: 0, slowms: (slowMs == null) ? 1000000 : slowMs, sampleRate: sampleRate}));
    assert.commandWorked(db.setLogLevel(logLevel, "command"));
    assert.commandWorked(db.setLogLevel(logLevel, "write"));

    const isSBEEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"]);

    // Certain fields in the log lines on mongoD are not applicable in their counterparts on
    // mongoS, and vice-versa. Ignore these fields when examining the logs of an instance on
    // which we do not expect them to appear.
    const ignoreFields =
            (isMongos
                 ? ["docsExamined", "keysExamined", "keysInserted", "keysDeleted", "planSummary",
					 "usedDisk", "hasSortStage"]
                 : ["nShards"]);

    function confirmLogContents(db, {test, logFields}, testIndex) {
        // Clear the log before running the test, to guarantee that we do not match against any
        // similar tests which may have run previously.
        assert.commandWorked(db.adminCommand({clearLog: "global"}));

        // Run the given command in order to generate a log line. If slowMs is non-null and
        // greater than 0, apply that slowMs to every second test.
        if (slowMs != null && slowMs > 0) {
            db.adminCommand({profile: 0, slowms: (testIndex % 2 ? slowMs : -1)});
        }
        assert.doesNotThrow(() => test(db));

        // Confirm whether the operation was logged or not.
        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        return findMatchingLogLine(globalLog.log, logFields, ignoreFields);
    }

    //
    // Defines the set of test operations and associated log output fields.
    //
    const testList = [
        {
            test: function(db) {
                assert.eq(db.test
                              .aggregate([{$match: {a: 1}}], {
                                  comment: logFormatTestComment,
                                  collation: {locale: "fr"},
                                  hint: {_id: 1},
                              })
                              .itcount(),
                          1);
            },
            logFields: {
                command: "aggregate",
                aggregate: coll.getName(),
                pipeline: [{$match: {a: 1}}],
                comment: logFormatTestComment,
                collation: {locale: "fr"},
                hint: {_id: 1},
                planSummary: "IXSCAN { _id: 1 }",
                cursorExhausted: 1,
                docsExamined: 10,
                keysExamined: 10,
                nreturned: 1,
                nShards: stParams.shards
            }
        },
        {
            test: function(db) {
                assert.eq(db.test.find({a: 1, $comment: logFormatTestComment})
                              .collation({locale: "fr"})
                              .count(),
                          1);
            },
            logFields: {
                command: "count",
                count: coll.getName(),
                query: {a: 1, $comment: logFormatTestComment},
                collation: {locale: "fr"},
                planSummary: "COLLSCAN"
            }
        },
        {
            test: function(db) {
                assert.eq(
                    db.test.distinct(
                        "a", {a: 1, $comment: logFormatTestComment}, {collation: {locale: "fr"}}),
                    [1]);
            },
            logFields: {
                command: "distinct",
                distinct: coll.getName(),
                query: {a: 1, $comment: logFormatTestComment},
                planSummary: "COLLSCAN",
                $comment: logFormatTestComment,
                collation: {locale: "fr"}
            }
        },
        {
            test: function(db) {
                assert.eq(db.test.find({_id: 1}).comment(logFormatTestComment).itcount(), 1);
            },
            logFields: {
                command: "find",
                find: coll.getName(),
                comment: logFormatTestComment,
                planSummary: isSBEEnabled ? "IXSCAN { _id: 1 }" : "IDHACK",
                cursorExhausted: 1,
                keysExamined: 1,
                docsExamined: 1,
                nreturned: 1,
                nShards: 1
            }
        },
        {
            test: function(db) {
                assert.eq(db.test.findAndModify({
                    query: {_id: 1, a: 1, $comment: logFormatTestComment},
                    update: {$inc: {b: 1}},
                    collation: {locale: "fr"}
                }),
                          {_id: 1, a: 1, loc: {type: "Point", coordinates: [1, 1]}});
            },
            // TODO SERVER-34208: display FAM update metrics in mongoS logs.
            logFields: Object.assign((isMongos ? {} : {nMatched: 1, nModified: 1}), {
                command: "findAndModify",
                findandmodify: coll.getName(),
                planSummary: "IXSCAN { _id: 1 }",
                keysExamined: 1,
                docsExamined: 1,
                $comment: logFormatTestComment,
                collation: {locale: "fr"}
            })
        },
        {
            test: function(db) {
                assert.commandWorked(db.test.mapReduce(() => {}, (a, b) => {}, {
                    query: {$comment: logFormatTestComment},
                    out: {inline: 1},
                }));
            },
            logFields: {
                command: "mapReduce",
                mapreduce: coll.getName(),
                planSummary: "COLLSCAN",
                keysExamined: 0,
                docsExamined: 10,
                $comment: logFormatTestComment,
                out: {inline: 1}
            }
        },
        {
            test: function(db) {
                assert.commandWorked(db.test.update(
                    {a: 1, $comment: logFormatTestComment}, {$inc: {b: 1}}, {multi: true}));
            },
            logFields: (isMongos ? {
                command: "update",
                update: coll.getName(),
                ordered: true,
                nMatched: 1,
                nModified: 1,
                nShards: stParams.shards
            }
                                 : {
                                       q: {a: 1, $comment: logFormatTestComment},
                                       u: {$inc: {b: 1}},
                                       multi: true,
                                       planSummary: "COLLSCAN",
                                       keysExamined: 0,
                                       docsExamined: 10,
                                       nMatched: 1,
                                       nModified: 1
                                   })
        },
        {
            test: function(db) {
                assert.commandWorked(db.test.update({_id: 100, $comment: logFormatTestComment},
                                                    {$inc: {b: 1}},
                                                    {multi: true, upsert: true}));
            },
            logFields: (isMongos ? {
                command: "update",
                update: coll.getName(),
                ordered: true,
                nMatched: 0,
                nModified: 0,
                nUpserted: 1,
                nShards: 1
            }
                                 : {
                                       q: {_id: 100, $comment: logFormatTestComment},
                                       u: {$inc: {b: 1}},
                                       multi: true,
                                       planSummary: "IXSCAN { _id: 1 }",
                                       keysExamined: 0,
                                       docsExamined: 0,
                                       nMatched: 0,
                                       nModified: 0,
                                       nUpserted: 1
                                   })
        },
        {
            test: function(db) {
                assert.commandWorked(db.test.insert({z: 1, comment: logFormatTestComment}));
            },
            logFields: {
                command: "insert",
                insert: `${coll.getName()}|${coll.getFullName()}`,
                keysInserted: 1,
                ninserted: 1,
                nShards: 1
            }
        },
        {
            test: function(db) {
                assert.commandWorked(db.test.remove({z: 1, $comment: logFormatTestComment}));
            },
            logFields: (isMongos ? {
                command: "delete",
                delete: coll.getName(),
                ordered: true,
                ndeleted: 1,
                nShards: stParams.shards
            }
                                 : {
                                       q: {z: 1, $comment: logFormatTestComment},
                                       limit: 0,
                                       planSummary: "COLLSCAN",
                                       keysExamined: 0,
                                       docsExamined: 12,
                                       ndeleted: 1,
                                       keysDeleted: 1
                                   })
        },
        {
            test: function(db) {
                const originalSortBytes = db.adminCommand(
                    {getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1});
                assert.commandWorked(originalSortBytes);
                assert.commandWorked(db.adminCommand(
                    {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 10}));
                assert.eq(coll.aggregate([{$match: {a: 1}}, {$sort: {a: 1}}], {allowDiskUse: true})
                              .itcount(),
                          1);
                assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes:
                        originalSortBytes.internalQueryMaxBlockingSortMemoryUsageBytes
                }));
            },
            logFields:
                {command: "aggregate", aggregate: coll.getName(), hasSortStage: 1, usedDisk: 1}
        },
        {
            test: function(db) {
                assert.commandWorked(db.runCommand({
                    update: "test",
                    updates: [
                        {q: {_id: 200}, u: {$inc: {c: 1}}, upsert: true},
                        {q: {_id: 201}, u: {$inc: {c: 1}}, upsert: true},
                        {q: {_id: 202}, u: {$inc: {c: 1}}, upsert: true}
                    ],
                    ordered: true
                }));
            },
            logFields: (isMongos ? {
                command: "update",
                update: coll.getName(),
                ordered: true,
                nMatched: 0,
                nModified: 0,
                nUpserted: 3,
                nShards: 1
            }
                                 : {command: "update", ns: `${db.getName()}.$cmd`})
        },
        {
            test: function(db) {
                assert.eq(db.test.find({_id: {$in: [1, 5]}})
                              .comment(logFormatTestComment)
                              .collation({locale: "fr"})
                              .itcount(),
                          2);
            },
            logFields: {
                command: "find",
                find: coll.getName(),
                planSummary: "IXSCAN { _id: 1 }",
                comment: logFormatTestComment,
                collation: {locale: "fr"},
                cursorExhausted: 1,
                docsExamined: 2,
                nreturned: 2,
                nShards: 1
            }
        }
    ];

    // Confirm log content for getMore on both find and aggregate cursors.
    const originatingCommands = {
        find: {find: coll.getName(), batchSize: 0},
        aggregate: {aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 0}}
    };

    for (let cmdName in originatingCommands) {
        const cmdObj = originatingCommands[cmdName];
        const cmdRes = assert.commandWorked(db.runCommand(cmdObj));
        const expectedCountOfDocuments = 14;
        // Make sure queryHash field is present of getMore logs following a find command.
        const extra = cmdName === "find" ? {queryHash: ""} : {};

        testList.push({
            test: function(db) {
                const cursor = new DBCommandCursor(db, cmdRes);
                assert.eq(cursor.itcount(), expectedCountOfDocuments);
            },
            logFields: Object.assign({getMore: cmdRes.cursor.id}, cmdObj, extra, {
                cursorid: cmdRes.cursor.id,
                planSummary: "COLLSCAN",
                cursorExhausted: 1,
                docsExamined: expectedCountOfDocuments,
                keysExamined: 0,
                nreturned: expectedCountOfDocuments,
                nShards: stParams.shards
            })
        });
    }

    // Run each of the test in the array, recording the log line found for each.
    const logLines =
        testList.map((testCase, arrIndex) => confirmLogContents(db, testCase, arrIndex));

    return [testList, logLines];
}

//
// Helper functions.
//

// In cases where some tests were not logged, this helper will identify and return them.
function getUnloggedTests(testsRun, logLines) {
    return testsRun.filter((testCase, arrIndex) => !logLines[arrIndex]);
}

//
// Test cases for varying values of logLevel, slowms, and sampleRate.
//

for (let testDB of [shardDB, mongosDB]) {
    // Test that all operations are logged when slowMs is < 0 and sampleRate is 1 at the
    // default logLevel.
    let [testsRun, logLines] =
        runLoggingTests({db: testDB, slowMs: -1, logLevel: 0, sampleRate: 1.0});
    let unlogged = getUnloggedTests(testsRun, logLines);
    assert.eq(unlogged.length, 0, () => tojson(unlogged));

    // Test that only some operations are logged when sampleRate is < 1 at the default
    // logLevel, even when slowMs is < 0. The actual sample rate is probabilistic, and may
    // therefore vary quite significantly from 0.5. However, we have already established
    // that with sampleRate 1 *all* ops are logged, so here it is sufficient to confirm that
    // some ops are not. We repeat the test 5 times to minimize the odds of failure.
    let sampleRateTestsRun = 0, sampleRateTestsLogged = 0;
    for (let i = 0; i < 5; i++) {
        [testsRun, logLines] =
            runLoggingTests({db: testDB, slowMs: -1, logLevel: 0, sampleRate: 0.5});
        unlogged = getUnloggedTests(testsRun, logLines);
        sampleRateTestsLogged += (testsRun.length - unlogged.length);
        sampleRateTestsRun += testsRun.length;
    }
    assert.betweenEx(0, sampleRateTestsLogged, sampleRateTestsRun);

    // Test that only operations which exceed slowMs are logged when slowMs > 0 and
    // sampleRate is 1, at the default logLevel. The given value of slowMs will be applied
    // to every second op in the test, so only half of the ops should be logged.
    [testsRun, logLines] =
        runLoggingTests({db: testDB, slowMs: 1000000, logLevel: 0, sampleRate: 1.0});
    unlogged = getUnloggedTests(testsRun, logLines);
    assert.eq(unlogged.length, Math.floor(testsRun.length / 2), () => tojson(unlogged));

    // Test that all operations are logged when logLevel is 1, regardless of sampleRate and
    // slowMs. We pass 'null' for slowMs to signify that a high threshold should be set
    // (such that, at logLevel 0, no operations would be logged) and that this value should
    // be applied for all operations, rather than for every second op as in the case of the
    // slowMs test.
    [testsRun, logLines] =
        runLoggingTests({db: testDB, slowMs: null, logLevel: 1, sampleRate: 0.5});
    unlogged = getUnloggedTests(testsRun, logLines);
    assert.eq(unlogged.length, 0, () => tojson(unlogged));
}
st.stop();
})();
