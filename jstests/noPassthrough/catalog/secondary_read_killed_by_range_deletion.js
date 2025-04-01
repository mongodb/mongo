/**
 * Tests that queries targeting a secondary node are killed if a range deletion was completed
 * during the query execution.
 *
 * @tags: [
 *   requires_fcv_82
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 2},
    other: {
        enableBalancer: false,
        rsOptions: {
            setParameter: {
                // Reduce the amount of time waiting for a range deletion completion.
                orphanCleanupDelaySecs: 0,
            }
        }
    }
});

const db = st.s.getDB(jsTestName());
const coll = db.getCollection("coll");
const collAux = db.getCollection("collAux");
const numDocs = 100;

// Create a sharded collection with one chunk on 'shard0'.
CreateShardedCollectionUtil.shardCollectionWithChunks(
    coll, {x: 1}, [{min: {x: MinKey}, max: {x: MaxKey}, shard: st.shard0.shardName}]);

const insertions = Array.from({length: numDocs}, (_, i) => {
    return {x: i, y: i, fix: "f"};
});
coll.insertMany(insertions);

let nextDonorShard = st.shard0;
let nextTargetShard = st.shard1;

// Create an auxiliar collection to perform a $lookup later.
assert.commandWorked(db.createCollection(collAux.getName()));
collAux.insertMany([{y: 1}, {y: 2}, {y: 3}, {y: 4}, {y: 5}]);

const commandsToTest = [
    {
        // Find command.
        cmd: {
            find: coll.getName(),
            filter: {},
            $readPreference: {mode: "secondary"},
            batchSize: 5,
        },
        returnsCursor: true,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "local"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            {
                // The query should never be killed when readConcern is `snapshot`. It should not
                // miss documents either since we're reading from a snapshot taken before the
                // migration.
                cmdExtension: {readConcern: {level: "snapshot"}},
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.eq(100 /* numDocs */,
                              docs.length,
                              "Failed to read the expected documents from the secondary.");
                },
            },
            {
                // The query should never be killed when readConcern is `available`. However, we'll
                // miss documents since the storage engine snapshot advances anyway.
                cmdExtension: {readConcern: {level: "available"}},
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
                skipAfterMigrationQueryCheck: true,
            },
            {
                // The query should not be killed when a rangeDeletion document is processed and it
                // doesn’t contain the `preMigrationShardVersion` field. This test case is to
                // simulate a rangeDeletion document created on an older version.
                cmdExtension: {readConcern: {level: "local"}},
                removeShardVersionFieldFromRangeDeletionDoc: true,
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
            },
            {
                // The query should not be killed when the server parameter
                // `terminateSecondaryReadsOnOrphanCleanup` is disabled, but it will miss documents.
                cmdExtension: {readConcern: {level: "local"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
            },
        ]
    },
    {
        // Aggregate command with a $match stage.
        cmd: {
            aggregate: coll.getName(),
            pipeline: [{$match: {y: {$gte: 0}}}],
            $readPreference: {mode: "secondary"},
            cursor: {batchSize: 5},
        },
        returnsCursor: true,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "local"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            {
                // The query should never be killed when readConcern is `snapshot`. It should not
                // miss documents either since we're reading from a snapshot taken before the
                // migration.
                cmdExtension: {readConcern: {level: "snapshot"}},
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.eq(docs.length,
                              100 /* numDocs */,
                              "Failed to read the expected documents from the secondary.");
                },
            },
            {
                // The query should never be killed when readConcern is `available`. However, we'll
                // miss documents since the storage engine snapshot advances anyway.
                cmdExtension: {readConcern: {level: "available"}},
                shouldWork: true,
                skipAfterMigrationQueryCheck: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
            },
            {
                // The query should not be killed when a rangeDeletion document is processed and it
                // doesn’t contain the `preMigrationShardVersion` field. This test case is to
                // simulate a rangeDeletion document created on an older version.
                cmdExtension: {readConcern: {level: "local"}},
                removeShardVersionFieldFromRangeDeletionDoc: true,
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
            },
            {
                // The query should not be killed when the server parameter
                // `terminateSecondaryReadsOnOrphanCleanup` is disabled, but it will miss documents.
                cmdExtension: {readConcern: {level: "local"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCursorCmdDocsFunc: (docs) => {
                    assert.lt(docs.length,
                              100 /* numDocs */,
                              "Expected to miss documents due to a range deletion.");
                },
            },
        ]
    },
    {
        // Aggregate command with a $out stage.
        cmd: {
            aggregate: coll.getName(),
            pipeline: [{$match: {}}, {$out: "collOut"}],
            $readPreference: {mode: "secondary"},
            cursor: {batchSize: 100},
        },
        returnsCursor: false,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "local"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            // // TODO (SERVER-102966) The following test cases fail with error 15959: 'the match
            // // filter must be an expression in an object'
            // {
            //     cmdExtension: {readConcern: {level: "snapshot"}},
            //     shouldWork: true,
            //     checkCmdResponseFunc: (res, db) => {
            //         const collOut = db.getCollection("collOut");
            //         assert.eq(100 /*numDocs*/, collOut.countDocuments());
            //         collOut.drop();
            //     }
            // },
            // {
            //     cmdExtension: {readConcern: {level: "local"}},
            //     terminateSecondaryReadsOnOrphanCleanup: false,
            //     shouldWork: true,
            //     checkCmdResponseFunc: (res, db) => {
            //         const collOut = db.getCollection("collOut");
            //         assert.lt(collOut.countDocuments(), 100 /* numDocs */);
            //         collOut.drop();
            //     },
            // },
            // {
            //     cmdExtension: {readConcern: {level: "majority"}},
            //     terminateSecondaryReadsOnOrphanCleanup: false,
            //     shouldWork: true,
            //     checkCmdResponseFunc: (res, db) => {
            //         const collOut = db.getCollection("collOut");
            //         assert.lt(collOut.countDocuments(), 100 /* numDocs */);
            //         collOut.drop();
            //     },
            // },
        ]
    },
    {
        // Aggregate command with a $lookup stage.
        cmd: {
            aggregate: collAux.getName(),
            pipeline:
                [{$lookup: {from: coll.getName(), localField: "y", foreignField: "y", as: "docs"}}],
            $readPreference: {mode: "secondary"},
            cursor: {batchSize: 100},
        },
        returnsCursor: false,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "snapshot"}},
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {},
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {},
            },
        ]
    },
    {
        // Count command.
        // Adding a predicate to avoid executing a fastcount operation.
        cmd: {count: coll.getName(), query: {y: {$gte: 0}}, $readPreference: {mode: "secondary"}},
        returnsCursor: false,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "local"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "local"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {
                    assert.lt(res.n, 100 /* numDocs */);
                },
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {
                    assert.lt(res.n, 100 /* numDocs */);
                },
            },
            // Can't run with 'snapshot' read concern because: Count doesn't support 'snapshot'
            // read concern.
        ]
    },
    {
        // Distinct command.
        cmd: {distinct: coll.getName(), key: "y", $readPreference: {mode: "secondary"}},
        returnsCursor: false,
        testCases: [
            {
                cmdExtension: {readConcern: {level: "local"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                shouldWork: false,
            },
            {
                cmdExtension: {readConcern: {level: "local"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {
                    assert.lt(res.values.length, 100 /* numDocs */);
                },
            },
            {
                cmdExtension: {readConcern: {level: "majority"}},
                terminateSecondaryReadsOnOrphanCleanup: false,
                shouldWork: true,
                checkCmdResponseFunc: (res, db) => {
                    assert.lt(res.values.length, 100 /* numDocs */);
                },
            },
            // Can't run with 'snapshot' read concern because: Cannot run 'distinct' on a sharded
            // collection with readConcern level 'snapshot'
            // TODO (SERVER-13116): Add a case with 'snapshot' read concern.
        ]
    },
];

function moveRange(ns, targetShardName) {
    assert.commandWorked(st.s.adminCommand(
        {moveRange: ns, min: {x: MinKey}, max: {x: MaxKey}, toShard: targetShardName}));
}

function moveRangeRemovingShardVersionFieldFromRangeDeletionDoc(ns, donorShard, targetShard) {
    let hangBeforeSendingCommitDecisionFP =
        configureFailPoint(donorShard, "hangBeforeSendingCommitDecision");

    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandWorked(db.adminCommand(
                {moveRange: ns, min: {x: MinKey}, max: {x: MaxKey}, toShard: toShardName}));
        }, ns, targetShard.shardName), st.s.port);

    hangBeforeSendingCommitDecisionFP.wait();

    assert.eq(1,
              donorShard.getDB("config").getCollection("rangeDeletions").find().toArray().length);
    assert.eq(1,
              donorShard.getDB("config")
                  .getCollection("rangeDeletions")
                  .find({"preMigrationShardVersion": {$exists: true}})
                  .toArray()
                  .length);
    donorShard.getDB("config")
        .getCollection("rangeDeletions")
        .updateOne({"preMigrationShardVersion": {$exists: true}},
                   {$unset: {"preMigrationShardVersion": ""}});
    assert.eq(0,
              donorShard.getDB("config")
                  .getCollection("rangeDeletions")
                  .find({"preMigrationShardVersion": {$exists: true}})
                  .toArray()
                  .length);

    hangBeforeSendingCommitDecisionFP.off();

    awaitResult();
}

function waitUntilRangeDeletionCompletes(shard) {
    assert.soon(() => {
        return shard.getDB("config").getCollection("rangeDeletions").find().toArray().length == 0;
    });
}

function getServerStatusCounters(donorShard, targetShard) {
    let counters = {donor: {primary: 0, secondary: 0}, target: {primary: 0, secondary: 0}};
    counters.donor.primary = donorShard.rs.getPrimary()
                                 .adminCommand({serverStatus: 1})
                                 .metrics.operation.killedDueToRangeDeletion.toNumber();
    counters.donor.secondary = donorShard.rs.getSecondary()
                                   .adminCommand({serverStatus: 1})
                                   .metrics.operation.killedDueToRangeDeletion.toNumber();
    counters.target.primary = targetShard.rs.getPrimary()
                                  .adminCommand({serverStatus: 1})
                                  .metrics.operation.killedDueToRangeDeletion.toNumber();
    counters.target.secondary = targetShard.rs.getSecondary()
                                    .adminCommand({serverStatus: 1})
                                    .metrics.operation.killedDueToRangeDeletion.toNumber();
    return counters;
}

let originalTerminateSecondaryReadsOnOrphanCleanup = undefined;
function setTerminateSecondaryReadsOnOrphanCleanup(value) {
    originalTerminateSecondaryReadsOnOrphanCleanup =
        assert
            .commandWorked(st.shard0.rs.getSecondary().adminCommand(
                {getParameter: 1, terminateSecondaryReadsOnOrphanCleanup: 1}))
            .terminateSecondaryReadsOnOrphanCleanup;
    st.getAllShards().forEach((rs) => {
        rs.getSecondaries().forEach((conn) => {
            assert.commandWorked(conn.adminCommand(
                {setParameter: 1, terminateSecondaryReadsOnOrphanCleanup: value}));
        });
    });
}

function setTerminateSecondaryReadsOnOrphanCleanupToOriginalValue() {
    if (originalTerminateSecondaryReadsOnOrphanCleanup) {
        st.getAllShards().forEach((rs) => {
            rs.getSecondaries().forEach((conn) => {
                assert.commandWorked(conn.adminCommand({
                    setParameter: 1,
                    terminateSecondaryReadsOnOrphanCleanup:
                        originalTerminateSecondaryReadsOnOrphanCleanup
                }));
            });
        });
    }
    originalTerminateSecondaryReadsOnOrphanCleanup = undefined;
}

let originalInternalQueryExecYieldIterations = undefined;
function setInternalQueryExecYieldIterations(value) {
    originalInternalQueryExecYieldIterations =
        assert
            .commandWorked(st.shard0.rs.getSecondary().adminCommand(
                {getParameter: 1, internalQueryExecYieldIterations: 1}))
            .internalQueryExecYieldIterations;
    st.getAllShards().forEach((rs) => {
        rs.getSecondaries().forEach((conn) => {
            assert.commandWorked(
                conn.adminCommand({setParameter: 1, internalQueryExecYieldIterations: value}));
        });
    });
}

function setInternalQueryExecYieldIterationsToOriginalValue() {
    if (originalInternalQueryExecYieldIterations) {
        st.getAllShards().forEach((rs) => {
            rs.getSecondaries().forEach((conn) => {
                assert.commandWorked(conn.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: originalInternalQueryExecYieldIterations
                }));
            });
        });
    }
    originalInternalQueryExecYieldIterations = undefined;
}

function checkCommandFailsDueToRangeDeletionOnGetMore(cmd,
                                                      shouldWork,
                                                      checkCursorCmdDocsFunc,
                                                      removeShardVersionFieldFromRangeDeletionDoc,
                                                      terminateSecondaryReadsOnOrphanCleanup,
                                                      skipAfterMigrationQueryCheck) {
    const donorShard = nextDonorShard;
    const targetShard = nextTargetShard;

    jsTest.log.info("Running test 'checkCommandFailsDueToRangeDeletionOnGetMore'", {
        cmd,
        shouldWork,
        terminateSecondaryReadsOnOrphanCleanup,
        removeShardVersionFieldFromRangeDeletionDoc,
        donorShard: donorShard.shardName,
        targetShard: targetShard.shardName,
        checkCursorCmdDocsFunc,
    });

    setTerminateSecondaryReadsOnOrphanCleanup(terminateSecondaryReadsOnOrphanCleanup);

    // 1. Start a query before the migration happens.
    const cursor = new DBCommandCursor(db, assert.commandWorked(db.runCommand(cmd)));

    // 2. Execute a chunk migration.
    const countersBeforeRangeDeletion = getServerStatusCounters(donorShard, targetShard);
    if (removeShardVersionFieldFromRangeDeletionDoc) {
        moveRangeRemovingShardVersionFieldFromRangeDeletionDoc(
            coll.getFullName(), donorShard, targetShard);
    } else {
        moveRange(coll.getFullName(), targetShard.shardName);
    }

    // 3. Start a query after the migration to verify it won't be killed.
    const cursorAfterMigration = new DBCommandCursor(db, assert.commandWorked(db.runCommand(cmd)));

    // 4. Wait for Range Deletion to complete.
    waitUntilRangeDeletionCompletes(donorShard);

    let expectedCounters = countersBeforeRangeDeletion;

    // 5. Verify `getMore` has the expected behavior after a Range Deletion.
    if (shouldWork) {
        checkCursorCmdDocsFunc(cursor.toArray());
    } else {
        assert.throwsWithCode(() => cursor.itcount(), ErrorCodes.QueryPlanKilled);
        expectedCounters.donor.secondary = expectedCounters.donor.secondary + 1;
    }

    // 6. Verify that any query that started after the migration should never be killed.
    if (!skipAfterMigrationQueryCheck) {
        assert.eq(
            numDocs,
            cursorAfterMigration.itcount(),
            "Failed to read the expected documents from the query that started after the migration");
    } else {
        cursorAfterMigration.itcount();
    }

    // 7. Verify serverStatus counters have been properly updated.
    assert.eq(expectedCounters, getServerStatusCounters(donorShard, targetShard));

    // 8. Set back to its original value the `terminateSecondaryReadsOnOrphanCleanup` parameter.
    setTerminateSecondaryReadsOnOrphanCleanupToOriginalValue();

    nextDonorShard = targetShard;
    nextTargetShard = donorShard;
}

function checkCommandFailsDueToRangeDeletionOnInitialCmd(
    cmd,
    shouldWork,
    returnsCursor,
    checkCmdResponseFunc,
    removeShardVersionFieldFromRangeDeletionDoc,
    terminateSecondaryReadsOnOrphanCleanup) {
    const donorShard = nextDonorShard;
    const targetShard = nextTargetShard;

    jsTest.log.info("Running test 'checkCommandFailsDueToRangeDeletionOnInitialCmd'", {
        cmd,
        shouldWork,
        terminateSecondaryReadsOnOrphanCleanup,
        removeShardVersionFieldFromRangeDeletionDoc,
        donorShard: donorShard.shardName,
        targetShard: targetShard.shardName,
        returnsCursor,
        checkCmdResponseFunc,
    });

    // 1. Verify that the command works correctly without concurrent chunk migrations.
    if (returnsCursor) {
        const res = assert.commandWorked(db.runCommand(cmd));
        (new DBCommandCursor(db, res)).toArray();
    } else {
        assert.commandWorked(db.runCommand(cmd));
    }

    // 2. Reduce the amount of yield iterations to force a yieldAndRestore during the initial read
    // command.
    setInternalQueryExecYieldIterations(1);

    setTerminateSecondaryReadsOnOrphanCleanup(terminateSecondaryReadsOnOrphanCleanup);

    // 3. Start a query before the migration happens and pause it during the first yieldAndRestore
    // execution to be able to run a chunk migration during the query execution.
    let hangBeforeRestoreStarts = configureFailPoint(
        donorShard.rs.getSecondary(), "setYieldAllLocksHang", {namespace: coll.getFullName()});
    const awaitQuery = startParallelShell(
        funWithArgs(function(dbName, cmd, shouldWork, checkCmdResponseFunc, returnsCursor) {
            const dbTest = db.getSiblingDB(dbName);
            const res = dbTest.runCommand(cmd);
            if (!shouldWork) {
                assert.commandFailedWithCode(res, ErrorCodes.QueryPlanKilled);
                return;
            }

            assert.commandWorked(res);

            if (returnsCursor) {
                const cursor = new DBCommandCursor(dbTest, res);
                checkCmdResponseFunc(cursor.toArray());

            } else if (!returnsCursor) {
                checkCmdResponseFunc(res, dbTest);
            }
        }, db.getName(), cmd, shouldWork, checkCmdResponseFunc, returnsCursor), st.s.port);

    const countersBeforeRangeDeletion = getServerStatusCounters(donorShard, targetShard);

    // 4. Execute a chunk migration once the failpoint is hit.
    hangBeforeRestoreStarts.wait();
    if (removeShardVersionFieldFromRangeDeletionDoc) {
        moveRangeRemovingShardVersionFieldFromRangeDeletionDoc(
            coll.getFullName(), donorShard, targetShard);
    } else {
        moveRange(coll.getFullName(), targetShard.shardName);
    }

    // 5. Wait for Range Deletion to complete.
    waitUntilRangeDeletionCompletes(donorShard);

    // 6. Wait for the query to finish.
    hangBeforeRestoreStarts.off();
    awaitQuery();

    // 7. Verify serverStatus counters have been properly updated.
    let expectedCounters = countersBeforeRangeDeletion;
    if (!shouldWork) {
        expectedCounters.donor.secondary = expectedCounters.donor.secondary + 1;
    }
    assert.eq(expectedCounters, getServerStatusCounters(donorShard, targetShard));

    // 8. Set back to its original value the `terminateSecondaryReadsOnOrphanCleanup` parameter.
    setTerminateSecondaryReadsOnOrphanCleanupToOriginalValue();

    // 9. Set back to its original value the `internalQueryExecYieldIteration` parameter.
    setInternalQueryExecYieldIterationsToOriginalValue();

    nextDonorShard = targetShard;
    nextTargetShard = donorShard;
}

for (const cmdToTest of commandsToTest) {
    assert(cmdToTest.cmd instanceof Object && Array.isArray(cmdToTest.testCases),
           "commandsToTest elements must be an object with 'cmd' and 'testCases' fields. Found: " +
               tojson(cmdToTest));
    assert(
        cmdToTest.hasOwnProperty('returnsCursor') && typeof cmdToTest.returnsCursor === "boolean",
        "Each cmd to test must have a 'returnsCursor' field and it must be a boolean. Found: " +
            tojson(cmdToTest.returnsCursor));

    const returnsCursor = cmdToTest.returnsCursor;

    for (const test of cmdToTest.testCases) {
        assert(test.cmdExtension && test.cmdExtension instanceof Object,
               "Each test case must have a 'cmdExtension' field and it must be an object. Found: " +
                   tojson(test));
        assert(test.hasOwnProperty('shouldWork') && typeof test.shouldWork === "boolean",
               "Each test case must have a 'shouldWork' field and it must be a boolean. Found: " +
                   tojson(test));
        if (returnsCursor) {
            assert(
                !test.shouldWork ||
                    test.hasOwnProperty('checkCursorCmdDocsFunc') &&
                        test.checkCursorCmdDocsFunc instanceof Function,
                "If 'shouldWork' is true and the command returns a cursor, a test case must have " +
                    "a 'checkCursorCmdDocsFunc' field and it must be a function. Found: " +
                    tojson(test) + " on cmd " + cmdToTest.cmd);
        } else {
            assert(
                !test.shouldWork ||
                    test.hasOwnProperty('checkCmdResponseFunc') &&
                        test.checkCmdResponseFunc instanceof Function,
                "If 'shouldWork' is true and the command returns a cursor, a test case must have " +
                    "a 'checkCmdResponseFunc' field and it must be a function. Found: " +
                    tojson(test));
        }
        assert(
            !test.hasOwnProperty('removeShardVersionFieldFromRangeDeletionDoc') ||
                typeof test.removeShardVersionFieldFromRangeDeletionDoc === "boolean",
            "If 'removeShardVersionFieldFromRangeDeletionDoc' is provided, it must be a boolean. Found: " +
                tojson(test) + " on cmd " + cmdToTest.cmd);
        assert(
            !test.hasOwnProperty('terminateSecondaryReadsOnOrphanCleanup') ||
                typeof test.terminateSecondaryReadsOnOrphanCleanup === "boolean",
            "If 'terminateSecondaryReadsOnOrphanCleanup' is provided, it must be a boolean. Found: " +
                tojson(test) + " on cmd " + cmdToTest.cmd);
        assert(!test.hasOwnProperty('skipAfterMigrationQueryCheck') ||
                   typeof test.skipAfterMigrationQueryCheck === "boolean",
               "If 'skipAfterMigrationQueryCheck' is provided, it must be a boolean. Found: " +
                   tojson(test) + " on cmd " + cmdToTest.cmd);

        const cmd = {...cmdToTest.cmd, ...test.cmdExtension};
        const removeShardVersionFieldFromRangeDeletionDoc =
            test.hasOwnProperty('removeShardVersionFieldFromRangeDeletionDoc')
            ? test.removeShardVersionFieldFromRangeDeletionDoc
            : false;
        const terminateSecondaryReadsOnOrphanCleanup =
            test.hasOwnProperty('terminateSecondaryReadsOnOrphanCleanup')
            ? test.terminateSecondaryReadsOnOrphanCleanup
            : true;
        const skipAfterMigrationQueryCheck = test.hasOwnProperty('skipAfterMigrationQueryCheck')
            ? test.skipAfterMigrationQueryCheck
            : false;
        const shouldWork = test.shouldWork;
        const checkCmdResponseFunc = shouldWork
            ? (returnsCursor ? test.checkCursorCmdDocsFunc : test.checkCmdResponseFunc)
            : undefined;

        if (returnsCursor) {
            checkCommandFailsDueToRangeDeletionOnGetMore(
                cmd,
                shouldWork,
                checkCmdResponseFunc,
                removeShardVersionFieldFromRangeDeletionDoc,
                terminateSecondaryReadsOnOrphanCleanup,
                skipAfterMigrationQueryCheck);
        }

        checkCommandFailsDueToRangeDeletionOnInitialCmd(cmd,
                                                        shouldWork,
                                                        returnsCursor,
                                                        checkCmdResponseFunc,
                                                        removeShardVersionFieldFromRangeDeletionDoc,
                                                        terminateSecondaryReadsOnOrphanCleanup);
    }
}

st.stop();
