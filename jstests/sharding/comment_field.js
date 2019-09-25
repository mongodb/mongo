/**
 * Test to verify that the 'comment' field set while running a command gets populated in $currentOp
 * and profiler. This test also verifies that for a sharded collection, the 'comment' fields gets
 * passed on from mongos to the respective shards.
 */
(function() {
"use strict";

load("jstests/libs/check_log.js");        // For checkLog.* helper functions.
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
    assert.eq(expectedNumOccurrences, numOccurrences);
}

function setPostCommandFailpointOnShards({mode, options}) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: "waitAfterCommandFinishesExecution", data: options, mode: mode}
    });
}

function runCommentParamTest(
    {coll, command, expectedRunningOps, commentObj, cmdName, parallelFunction}) {
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
              1);

    // Unset the failpoint to unblock the command and join with the parallel shell.
    setPostCommandFailpointOnShards({mode: "off", options: {}});
    outShell();

    // For 'update' and 'delete' commands log lines and profiler entries  are added for each
    // sub-operation.
    let expectedAdditionalEntries = (cmdName === "update") ? command["updates"].length : 0;
    expectedAdditionalEntries += (cmdName === "delete") ? command["deletes"].length : 0;

    // Run the 'checkLog' only for commands with uuid so that the we know the log line belongs to
    // current operation.
    if (commentObj["uuid"]) {
        let expectedLogLines = expectedRunningOps + 1;  // +1 for log line on mongos.
        expectedLogLines += expectedAdditionalEntries;
        verifyLogContains(
            [testDB, shard0DB, shard1DB],
            // Verify that a field with 'comment' exists in the same line as the command.
            [
                ", comment: ",
                checkLog.formatAsLogLine(commentObj),
                'appName: "MongoDB Shell" command: ' + ((cmdName === "getMore") ? cmdName : "")
            ],
            expectedLogLines);
    }

    // Reset log level to zero.
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.getSiblingDB("admin").setLogLevel(0);
    }

    // Verify that profile entry has 'comment' field.
    const profileFilter = {"command.comment": commentObj};
    assert.eq(shard0DB.system.profile.find(profileFilter).itcount() +
                  shard1DB.system.profile.find(profileFilter).itcount(),
              (expectedAdditionalEntries > 0) ? expectedAdditionalEntries : expectedRunningOps);
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

// For update command on a sharded collection, when all the shards are targetted.
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
    expectedRunningOps: 2
});

// For update command on a sharded collection, where a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {update: shardedColl.getName(), updates: [{q: {x: 3}, u: {x: 3, a: 1}}]},
    expectedRunningOps: 1
});

// For update command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        update: unshardedColl.getName(),
        updates: [{q: {x: 1}, u: {x: 1}}, {q: {x: -1}, u: {x: -1, a: 1}}]
    },
    expectedRunningOps: 1
});

// For delete command on a sharded collection, where all the shards are targetted.
runCommentParamTest({
    coll: shardedColl,
    command: {
        delete: shardedColl.getName(),
        deletes: [{q: {x: 3}, limit: 0}, {q: {x: -3}, limit: 0}, {q: {x: -1}, limit: 0}],
        ordered: false
    },
    expectedRunningOps: 2
});

// For delete command on a sharded collection, where a single shard is targetted.
runCommentParamTest({
    coll: shardedColl,
    command:
        {delete: shardedColl.getName(), deletes: [{q: {x: 1}, limit: 0}, {q: {x: 3}, limit: 0}]},
    expectedRunningOps: 1
});

// For delete command on an unsharded collection, where only primary shard is targetted.
runCommentParamTest({
    coll: unshardedColl,
    command: {
        delete: unshardedColl.getName(),
        deletes: [{q: {_id: 1}, limit: 0}, {q: {_id: 0}, limit: 0}]
    },
    expectedRunningOps: 1
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
// Tests for Legacy query.
//

// Verify that $comment at top level is treated as a 'comment' field.
const legacyComment = {
    testName: jsTestName(),
    commentField: "Legacy_find_comment"
};
runCommentParamTest({
    coll: shardedColl,
    expectedRunningOps: 2,
    cmdName: "find",
    commentObj: legacyComment,
    parallelFunction: `const sourceDB = db.getSiblingDB(jsTestName());
        sourceDB.getMongo().forceReadMode("legacy");
        sourceDB.coll.find({$query: {a: 1}, $comment: ${tojson(legacyComment)}});`
});

st.stop();
})();
