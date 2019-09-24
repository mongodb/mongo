/*
 * Tests that changing the shard key value of a document using pipeline updates.
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
'use strict';

load("jstests/sharding/libs/update_shard_key_helpers.js");

const st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});
const kDbName = 'db';
const mongos = st.s0;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const ns = kDbName + '.foo';

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, shard0);

// Tuples represent [shouldRunCommandInTxn, runUpdateAsFindAndModifyCmd, isUpsert].
const changeShardKeyOptions = [
    [false, false, false],
    [true, false, false],
    [true, true, false],
    [false, true, false],
    [false, false, true],
    [true, false, true],
    [false, true, true],
    [true, true, true]
];

// Test pipeline updates where the document being updated remains on the same shard.

changeShardKeyOptions.forEach(function(updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog("Testing changing the shard key using pipeline style update and " +
              (isFindAndModify ? "findAndModify command " : "update command ") +
              (runInTxn ? "in transaction " : "as retryable write"));

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300}, {"x": 4}],
        [
            [{$set: {"x": {$multiply: ["$x", 2]}}}, {$addFields: {"z": 1}}],
            [{$set: {"x": {$multiply: ["$x", -1]}}}, {$addFields: {"z": 1}}]
        ],
        upsert,
        [{"x": 600, "z": 1}, {"x": -4, "z": 1}]);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              [{"x.a": 300}, {"x.a": 4}],
                              [
                                  [{$set: {"x": {"a": {$multiply: ["$x.a", 2]}, "y": 1}}}],
                                  [{$set: {"x": {"a": {$multiply: ["$x.a", -1]}, "y": 1}}}]
                              ],
                              upsert,
                              [{"x": {"a": 600, "y": 1}}, {"x": {"a": -4, "y": 1}}]);
    assertCanUpdatePartialShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
        [[{$set: {"x": {$multiply: ["$x", 2]}}}], [{$set: {"x": {$multiply: ["$x", -1]}}}]],
        upsert,
        [{"x": 600}, {"x": -4}]);

    assertCanUnsetSKFieldUsingPipeline(st,
                                       kDbName,
                                       ns,
                                       session,
                                       sessionDB,
                                       runInTxn,
                                       isFindAndModify,
                                       {"x": 300, "y": 80},
                                       [{$project: {"y": 0}}],
                                       upsert,
                                       {"x": 300, "y": 80});

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              {"_id": 300},
                              [{$set: {"_id": {$multiply: ["$_id", 2]}}}],
                              {"_id": 600});
        assertCannotUpdate_idDottedPath(st,
                                        kDbName,
                                        ns,
                                        session,
                                        sessionDB,
                                        runInTxn,
                                        isFindAndModify,
                                        {"_id.a": 300},
                                        [{$set: {"_id": {"a": {$multiply: ["$_id.a", 2]}}}}],
                                        {"_id": {"a": 600}});
        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(st,
                                            kDbName,
                                            ns,
                                            session,
                                            sessionDB,
                                            runInTxn,
                                            {"x": 300},
                                            [{$set: {"x": {$multiply: ["$x", 2]}}}],
                                            {"x": 600});
        }
    }
});

// Test pipeline updates where the document being updated will move shards.

changeShardKeyOptions.forEach(function(updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog("Testing changing the shard key using pipeline style update and " +
              (isFindAndModify ? "findAndModify command " : "update command ") +
              (runInTxn ? "in transaction " : "as retryable write"));

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300}, {"x": 4}],
        [
            [{$set: {"x": {$multiply: ["$x", -1]}}}, {$addFields: {"z": 1}}],
            [{$set: {"x": {$multiply: ["$x", 100]}}}, {$addFields: {"z": 1}}]
        ],
        upsert,
        [{"x": -300, "z": 1}, {"x": 400, "z": 1}]);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              [{"x.a": 300}, {"x.a": 4}],
                              [
                                  [{$set: {"x": {"a": {$multiply: ["$x.a", -1]}, "y": 1}}}],
                                  [{$set: {"x": {"a": {$multiply: ["$x.a", 100]}, "y": 1}}}]
                              ],
                              upsert,
                              [{"x": {"a": -300, "y": 1}}, {"x": {"a": 400, "y": 1}}]);
    assertCanUpdatePartialShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
        [[{$set: {"x": {$multiply: ["$x", -1]}}}], [{$set: {"x": {$multiply: ["$x", 100]}}}]],
        upsert,
        [{"x": -300}, {"x": 400}]);
    assertCanUnsetSKFieldUsingPipeline(st,
                                       kDbName,
                                       ns,
                                       session,
                                       sessionDB,
                                       runInTxn,
                                       isFindAndModify,
                                       {"x": 300, "y": 80},
                                       [{$project: {"y": 0}}],
                                       upsert,
                                       {"x": 300, "y": 80});

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              {"_id": 300},
                              [{$set: {"_id": {$multiply: ["$_id", -1]}}}],
                              {"_id": -300});
        assertCannotUpdate_idDottedPath(st,
                                        kDbName,
                                        ns,
                                        session,
                                        sessionDB,
                                        runInTxn,
                                        isFindAndModify,
                                        {"_id.a": 300},
                                        [{$set: {"_id": {"a": {$multiply: ["$_id.a", -1]}}}}],
                                        {"_id": {"a": -300}});
        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(st,
                                            kDbName,
                                            ns,
                                            session,
                                            sessionDB,
                                            runInTxn,
                                            {"x": 300},
                                            [{$set: {"x": {$multiply: ["$x", -1]}}}],
                                            {"x": -300});
        }
    }
});

st.stop();
})();