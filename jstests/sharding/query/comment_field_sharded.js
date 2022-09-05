/**
 * Test to verify that the 'comment' field set while running a command gets populated in $currentOp
 * and profiler. This test also verifies that for a sharded collection, the 'comment' fields gets
 * passed on from mongos to the respective shards.
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
load("jstests/libs/profiler.js");         // For profilerHas*OrThrow helper functions.

// This test runs manual getMores using different connections, which will not inherit the
// implicit session of the cursor establishing command.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB(jsTestName());
const shardedColl = testDB.coll;
const unshardedColl = testDB.unsharded;
const shard0DB = st.shard0.getDB(jsTestName());
const shard1DB = st.shard1.getDB(jsTestName());

assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

// Shard shardedColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
st.shardColl(shardedColl, {x: 1}, {x: 0}, {x: 1});

// Insert one document on each shard.
assert.commandWorked(shardedColl.insert({x: 1, _id: 1}));
assert.commandWorked(shardedColl.insert({x: -1, _id: 0}));

assert.commandWorked(unshardedColl.insert({x: 1, _id: 1}));

// changes the 'slowms' threshold to -1ms. This will log all the commands.
assert.commandWorked(testDB.adminCommand({profile: 0, slowms: -1}));

/**
 * Verifies that there are 'expectedNumOccurrences' log lines contains every element of
 * 'inputArray'.
 */
function verifyLogContains(connections, inputArray, expectedNumOccurrences) {
    let numOccurrences = 0;
    for (let conn of connections) {
        const logs = checkLog.getGlobalLog(conn);
        for (let logMsg of logs) {
            let numMatches = 0;
            for (let input of inputArray) {
                numMatches += logMsg.includes(input) ? 1 : 0;
            }
            numOccurrences += ((numMatches == inputArray.length) ? 1 : 0);
        }
    }
    assert.eq(expectedNumOccurrences, numOccurrences, "Failed to find messages " + inputArray);
}

function setPostCommandFailpointOnShards({mode, options}) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: "waitAfterCommandFinishesExecution", data: options, mode: mode}
    });
}

function runCommentParamTest({
    coll,
    command,
    expectedRunningOps,
    expectedProfilerEntries,
    commentObj,
    cmdName,
    parallelFunction
}) {
    if (!cmdName) {
        cmdName = Object.keys(command)[0];
    }
    setPostCommandFailpointOnShards(
        {mode: "alwaysOn", options: {ns: coll.getFullName(), commands: [cmdName]}});

    // Restart profiler.
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();

        // Enable profiling and changes the 'slowms' threshold to -1ms. This will log all the
        // commands.
        shardDB.setProfilingLevel(2, -1);
    }

    if (!commentObj) {
        commentObj = {
            testName: jsTestName(),
            commentField: "comment_" + cmdName,
            uuid: UUID().hex()
        };
        command["comment"] = commentObj;
    }

    // If 'parallelFunction' is passed as a parameter, do not use the 'runCommand' to execute the
    // command.
    if (!parallelFunction) {
        parallelFunction = `const sourceDB = db.getSiblingDB(jsTestName());
            let cmdRes = sourceDB.runCommand(${tojson(command)});
            assert.commandWorked(cmdRes); `;
    }

    // Force a refresh on the shards. This is necessary because MongoS could get StaleDbVersion
    // error upon sending an agg request, causing it to retry the agg command from the top and
    // resulting in more profiler entries than what is expected.
    assert.commandWorked(st.rs0.getPrimary().getDB(testDB.getName()).adminCommand({
        _flushDatabaseCacheUpdates: testDB.getName(),
        syncFromConfig: true
    }));
    assert.commandWorked(st.rs1.getPrimary().getDB(testDB.getName()).adminCommand({
        _flushDatabaseCacheUpdates: testDB.getName(),
        syncFromConfig: true
    }));

    // Run the 'command' in a parallel shell.
    let outShell = startParallelShell(parallelFunction, st.s.port);

    // Wait for the parallel shell to hit the failpoint and verify that the 'comment' field is
    // present in $currentOp.
    const filter = {
        [`command.${cmdName}`]:
            (cmdName == "explain" || cmdName == "getMore") ? {$exists: true} : coll.getName(),
        "command.comment": commentObj
    };
    assert.soon(
        () => testDB.getSiblingDB("admin")
                  .aggregate([{$currentOp: {localOps: false}}, {$match: filter}])
                  .toArray()
                  .length == expectedRunningOps,
        () => tojson(
            testDB.getSiblingDB("admin").aggregate([{$currentOp: {localOps: false}}]).toArray()));

    // Verify that MongoS also shows the comment field in $currentOp.
    assert.eq(testDB.getSiblingDB("admin")
                  .aggregate([{$currentOp: {localOps: true}}, {$match: filter}])
                  .toArray()
                  .length,
              1,
              testDB.getSiblingDB("admin").aggregate([{$currentOp: {localOps: true}}]).toArray());

    // Unset the failpoint to unblock the command and join with the parallel shell.
    setPostCommandFailpointOnShards({mode: "off", options: {}});
    outShell();

    // Verify that profile entry has 'comment' field.
    expectedProfilerEntries =
        expectedProfilerEntries ? expectedProfilerEntries : expectedRunningOps;
    let expectedProfilerEntriesList = expectedProfilerEntries;
    if (!Array.isArray(expectedProfilerEntriesList)) {
        expectedProfilerEntriesList = [expectedProfilerEntriesList];
    }
    const profileFilter = {"command.comment": commentObj};
    const foundProfilerEntriesCount = shard0DB.system.profile.find(profileFilter).itcount() +
        shard1DB.system.profile.find(profileFilter).itcount();
    assert(expectedProfilerEntriesList.includes(foundProfilerEntriesCount),
           () => 'Expected count of profiler entries to be in ' +
               tojson(expectedProfilerEntriesList) + ' but found' + foundProfilerEntriesCount +
               ' instead. ' + tojson({
                     [st.shard0.name]: shard0DB.system.profile.find().toArray(),
                     [st.shard1.name]: shard1DB.system.profile.find().toArray()
                 }));

    // Run the 'checkLog' only for commands with uuid so that the we know the log line belongs to
    // current operation.
    if (commentObj["uuid"]) {
        // Verify that a field with 'comment' exists in the same line as the command.
        const expectStrings = [
            ',"comment":',
            checkLog.formatAsJsonLogLine(commentObj),
            '"appName":"MongoDB Shell","command":{' +
                ((cmdName === "getMore") ? '"' + cmdName + '"' : "")
        ];

        verifyLogContains(
            [testDB, shard0DB, shard1DB],
            expectStrings,
            ((cmdName === "update" || cmdName === "delete")
                 ? expectedRunningOps
                 : 0) +  // For 'update' and 'delete' commands we also log an additional line
                         // for the entire operation.
                foundProfilerEntriesCount +
                1  // +1 to account for log line on mongos.
        );
    }
}

// For find command on a sharded collection, when all the shards are targetted.
runCommentParamTest(
    {coll: shardedColl, command: {find: shardedColl.getName(), filter: {}}, expectedRunningOps: 2});

// For find command on a sharded collection, when a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {find: shardedColl.getName(), filter: {x: 3}},
    expectedRunningOps: 1
});

// For find command on an unsharded collection. Query targets only the primary shard.
runCommentParamTest({
    coll: unshardedColl,
    command: {find: unshardedColl.getName(), filter: {}},
    expectedRunningOps: 1
});

// For insert command on a sharded collection, where all the shards are targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {insert: shardedColl.getName(), documents: [{x: 0.5}, {x: -0.5}], ordered: false},
    expectedRunningOps: 2
});

// For insert command on a sharded collection, where a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {insert: shardedColl.getName(), documents: [{x: 4}]},
    expectedRunningOps: 1
});

// For insert command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {insert: unshardedColl.getName(), documents: [{x: 3}]},
    expectedRunningOps: 1
});

// For update command on a sharded collection, when all the shards are targetted. For update command
// profiler entries are only added for each sub-operation.
runCommentParamTest({
    coll: shardedColl,
    command: {
        update: shardedColl.getName(),
        updates: [
            {q: {x: 3}, u: {$set: {a: 1}}},
            {q: {x: 2}, u: {$set: {a: 1}}},
            {q: {x: -3}, u: {$set: {a: 1}}}
        ],
        ordered: false
    },
    expectedRunningOps: 2,
    expectedProfilerEntries: 3
});

// For update command on a sharded collection, where a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {update: shardedColl.getName(), updates: [{q: {x: 3}, u: {x: 3, a: 1}}]},
    expectedRunningOps: 1,
    expectedProfilerEntries: 1
});

// For update command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        update: unshardedColl.getName(),
        updates: [{q: {x: 1}, u: {x: 1}}, {q: {x: -1}, u: {x: -1, a: 1}}]
    },
    expectedRunningOps: 1,
    expectedProfilerEntries: 2
});

// For delete command on a sharded collection, where all the shards are targetted. For delete
// command profiler entries are only added for each sub-operation.
runCommentParamTest({
    coll: shardedColl,
    command: {
        delete: shardedColl.getName(),
        deletes: [{q: {x: 3}, limit: 0}, {q: {x: -3}, limit: 0}, {q: {x: -1}, limit: 0}],
        ordered: false
    },
    expectedRunningOps: 2,
    expectedProfilerEntries: 3
});

// For delete command on a sharded collection, where a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command:
        {delete: shardedColl.getName(), deletes: [{q: {x: 1}, limit: 0}, {q: {x: 3}, limit: 0}]},
    expectedRunningOps: 1,
    expectedProfilerEntries: 2
});

// For delete command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        delete: unshardedColl.getName(),
        deletes: [{q: {_id: 1}, limit: 0}, {q: {_id: 0}, limit: 0}]
    },
    expectedRunningOps: 1,
    expectedProfilerEntries: 2
});

// For createIndexes command on a sharded collection,  where all the shards are targetted.
runCommentParamTest({
    coll: shardedColl,
    command:
        {createIndexes: shardedColl.getName(), indexes: [{name: "newField_1", key: {newField: 1}}]},
    expectedRunningOps: 2
});

// For createIndexes command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        createIndexes: unshardedColl.getName(),
        indexes: [{name: "newField_1", key: {newField: 1}}]
    },
    expectedRunningOps: 1
});

//
// Tests for 'explain' on a sharded collection.
//

let innerComment = {name: "comment_field_explain", type: "innerComment"};
let outerComment = {name: "comment_field_explain", type: "outerComment", uuid: UUID().hex()};

// Verify that comment field gets populated on the profiler for the case where explain is within
// aggregate.
runCommentParamTest({
    coll: shardedColl,
    command: {
        aggregate: shardedColl.getName(),
        pipeline: [],
        comment: innerComment,
        cursor: {},
        explain: true
    },
    expectedRunningOps: 2,
    commentObj: innerComment,
    cmdName: "explain"
});

// Verify that a comment field attached to an inner command of explain get passed on to the shards
// and is visible on currentOp and profiler entry.
runCommentParamTest({
    coll: shardedColl,
    command: {
        explain: {aggregate: shardedColl.getName(), pipeline: [], comment: innerComment, cursor: {}}
    },
    expectedRunningOps: 2,
    commentObj: innerComment
});

// Verify that when a comment field is attached to an inner command of explain and there is another
// 'comment' field at the top level, top level comment gets the precedence.
runCommentParamTest({
    coll: shardedColl,
    command: {
        explain:
            {aggregate: shardedColl.getName(), pipeline: [], comment: innerComment, cursor: {}},
        comment: outerComment
    },
    expectedRunningOps: 2,
    commentObj: outerComment
});

//
// Tests for 'explain' on an unsharded collection.
//

innerComment = {
    name: "comment_field_explain",
    type: "innerComment_unsharded"
};
outerComment = {
    name: "comment_field_explain",
    type: "outerComment_unsharded",
    uuid: UUID().hex()
};

// Verify that comment field gets populated on the profiler for the case where explain is within
// aggregate.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        aggregate: unshardedColl.getName(),
        pipeline: [],
        comment: innerComment,
        cursor: {},
        explain: true
    },
    expectedRunningOps: 1,
    commentObj: innerComment,
    cmdName: "explain"
});

// Verify that a comment field attached to an inner command of explain get passed on to the shard
// and is visible on currentOp and profiler entry.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        explain:
            {aggregate: unshardedColl.getName(), pipeline: [], comment: innerComment, cursor: {}}
    },
    expectedRunningOps: 1,
    commentObj: innerComment
});

// Verify that when a comment field is attached to an inner command of explain and there is another
// / 'comment' field at the top level, top level comment gets the precedence.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        explain:
            {aggregate: unshardedColl.getName(), pipeline: [], comment: innerComment, cursor: {}},
        comment: outerComment
    },
    expectedRunningOps: 1,
    commentObj: outerComment
});

//
// Tests for 'getMore' comment propagation on a sharded collection.
//

// Verify the 'comment' field on the aggregate command is propagated to the subsequent getMore
// command.
let comment = {comment: "aggregate_comment", commandName: "aggregate", uuid: UUID().hex()};
let res = assert.commandWorked(
    testDB.runCommand({aggregate: "coll", pipeline: [], comment: comment, cursor: {batchSize: 0}}));
runCommentParamTest({
    coll: shardedColl,
    command: {getMore: res.cursor.id, collection: shardedColl.getName()},
    expectedRunningOps: 2,
    commentObj: comment
});

// Verify the 'comment' field on the getMore command takes precedence over the 'comment' field on
// the originating command.
res = assert.commandWorked(
    testDB.runCommand({aggregate: "coll", pipeline: [], comment: comment, cursor: {batchSize: 0}}));
comment = {
    comment: "getmore_comment",
    commandName: "getmore",
    uuid: UUID().hex()
};
runCommentParamTest({
    coll: shardedColl,
    command: {getMore: res.cursor.id, collection: shardedColl.getName(), comment: comment},
    expectedRunningOps: 2,
    commentObj: comment
});

// Verify the 'comment' field on the find command is propagated to the subsequent getMore command.
comment = {
    comment: "find_comment",
    commandName: "find",
    uuid: UUID().hex()
};
res = assert.commandWorked(
    testDB.runCommand({find: shardedColl.getName(), filter: {}, comment: comment, batchSize: 0}));
runCommentParamTest({
    coll: shardedColl,
    command: {getMore: res.cursor.id, collection: shardedColl.getName()},
    expectedRunningOps: 2,
    commentObj: comment
});

// Verify the 'comment' field on the getMore command takes precedence over the 'comment' field on
// the originating command.
res = assert.commandWorked(
    testDB.runCommand({find: shardedColl.getName(), filter: {}, comment: comment, batchSize: 0}));
comment = {
    comment: "getmore_comment",
    commandName: "getmore",
    uuid: UUID().hex()
};
runCommentParamTest({
    coll: shardedColl,
    command: {getMore: res.cursor.id, collection: shardedColl.getName(), comment: comment},
    expectedRunningOps: 2,
    commentObj: comment
});

//
// Tests for 'getMore' comment propagation on an unsharded collection.
//

// Verify the 'comment' field on the aggregate command is propagated to the subsequent getMore
// command.
comment = {
    comment: "unsharded_aggregate_comment",
    commandName: "aggregate",
    uuid: UUID().hex()
};
res = assert.commandWorked(testDB.runCommand(
    {aggregate: unshardedColl.getName(), pipeline: [], comment: comment, cursor: {batchSize: 0}}));
runCommentParamTest({
    coll: unshardedColl,
    command: {getMore: res.cursor.id, collection: unshardedColl.getName(), batchSize: 1},
    expectedRunningOps: 1,
    commentObj: comment
});

// Verify the 'comment' field on the getMore command takes precedence over the 'comment' field on
// the originating command.
res = assert.commandWorked(testDB.runCommand(
    {aggregate: unshardedColl.getName(), pipeline: [], comment: comment, cursor: {batchSize: 0}}));
comment = {
    comment: "unsharded_getmore_comment",
    commandName: "getmore",
    uuid: UUID().hex()
};
runCommentParamTest({
    coll: unshardedColl,
    command: {
        getMore: res.cursor.id,
        collection: unshardedColl.getName(),
        comment: comment,
        batchSize: 1
    },
    expectedRunningOps: 1,
    commentObj: comment
});

//
// Tests for '$unionWith' stage as part of aggregate on a sharded collection.
//

// For aggregate command with a $unionWith stage, where a sharded collection unions with a sharded
// collection, each shard receives an aggregate operation for the outer pipeline (with the stages
// prior to the $unionWith stage) and the inner pipeline. Each aggregate operation is followed up by
// a getMore to exhaust the cursor. So there should be 8 profiler entries which has the 'comment'
// field. In addition there is an aggregate operation which does merge cursors.
runCommentParamTest({
    coll: shardedColl,
    command: {
        aggregate: shardedColl.getName(),
        pipeline: [
            {$match: {p: null}},
            {$unionWith: {coll: shardedColl.getName(), pipeline: [{$group: {_id: "$x"}}]}}
        ],
        cursor: {}
    },
    expectedRunningOps: 2,
    expectedProfilerEntries: 9
});

// For aggregate command with a $unionWith stage, where a sharded collection unions with an
// unsharded collection, each shard receives an aggregate & getMore operation (with the stages prior
// to the $unionWith stage) for the outer pipeline and 1 aggregate operation for the inner pipeline
// on unsharded collection. So there should be 5 profiler entries which has the 'comment' field. In
// addition there is an aggregate operation which does merge cursors.
runCommentParamTest({
    coll: shardedColl,
    command: {
        aggregate: shardedColl.getName(),
        pipeline: [
            {$match: {p: null}},
            {$unionWith: {coll: unshardedColl.getName(), pipeline: [{$group: {_id: "$x"}}]}}
        ],
        cursor: {}
    },
    expectedRunningOps: 2,
    // If the $unionWith runs on the primary and the primary supports it, the aggregate for the
    // inner pipeline can be done as a local read and will not be logged.
    expectedProfilerEntries: [5, 6]
});

// For aggregate command with a $unionWith stage, where an unsharded collection unions with a
// sharded collection, each shard receives an aggregate & getMore operation for the inner pipeline.
// So there should be 4 profiler entries which has the 'comment' field. In addition there is an
// aggregate operation which does merge cursors.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        aggregate: unshardedColl.getName(),
        pipeline: [{$unionWith: shardedColl.getName()}],
        cursor: {}
    },
    expectedRunningOps: 1,
    expectedProfilerEntries: 5
});

// For aggregate command with a $unionWith stage, where a sharded collection unions with a sharded
// collection. When the batch size is 0, no getMore commands should be issued. In this case, the
// outer pipeline would still be split.
runCommentParamTest({
    coll: shardedColl,
    command: {
        aggregate: shardedColl.getName(),
        pipeline: [{$unionWith: shardedColl.getName()}],
        cursor: {batchSize: 0}
    },
    expectedRunningOps: 2,
    expectedProfilerEntries:
        3  // Additional profiler entry for the pipeline that executes merge cursors.
});

// For aggregate command with a $unionWith stage, where a sharded collection unions with an
// unsharded collection. When the batch size is 0, no getMore commands should be issued. In this
// case, the outer pipeline would still be split.
runCommentParamTest({
    coll: shardedColl,
    command: {
        aggregate: shardedColl.getName(),
        pipeline: [{$unionWith: unshardedColl.getName()}],
        cursor: {batchSize: 0}
    },
    expectedRunningOps: 2,
    expectedProfilerEntries:
        3  // Additional profiler entry for the pipeline that executes merge cursors.
});

// For aggregate command with a $unionWith stage, where an unsharded collection unions with a
// sharded collection. When the batch size is 0, no getMore commands should be issued.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        aggregate: unshardedColl.getName(),
        pipeline: [{$unionWith: shardedColl.getName()}],
        cursor: {batchSize: 0}
    },
    expectedRunningOps: 1
});

st.stop();
})();
