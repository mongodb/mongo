/**
 * Tests shard key updates on a sharded timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   # Update on a sharded timeseries collection is supported since 7.1
 *   requires_fcv_71,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 *   featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

setUpShardedCluster();

(function testUpdateMultiModifyingShardKey() {
    // This will create a sharded collection with 2 chunks: (MinKey, meta: "A"] and [meta: "B",
    // MaxKey).
    const coll = prepareShardedCollection(
        {collName: getCallerName(1), initialDocList: docs, includeMeta: true});

    // This update command tries to update doc5_b_f104 into {_id: 5, meta: "A", f: 104}. The owning
    // shard would be the shard that owns [MinKey, meta: "A"].
    const updateMultiCmd = {
        update: coll.getName(),
        updates: [{
            q: {[metaFieldName]: "B", f: {$gt: 103}},
            u: {$set: {[metaFieldName]: "A"}},
            multi: true
        }]
    };
    jsTestLog(`Running update multi: ${tojson(updateMultiCmd)}`);

    // We don't allow update multi to modify the shard key at all.
    const res = assert.commandFailedWithCode(testDB.runCommand(updateMultiCmd),
                                             ErrorCodes.InvalidOptions,
                                             `cmd = ${tojson(updateMultiCmd)}`);
    assert.sameMembers(docs, coll.find().toArray(), "Collection contents did not match");
})();

// TODO SERVER-77607 This test case should succeed without the error and be enabled.
/*
(function testUpdateOneModifyingShardKey() {
    // This will create a sharded collection with 2 chunks: (MinKey, meta: "A"] and [meta: "B",
    // MaxKey).
    const coll = prepareShardedCollection(
        {collName: getCallerName(1), initialDocList: docs, includeMeta: true});

    // This update command tries to update doc5_b_f104 into {_id: 5, meta: "A", f: 104}. The owning
    // shard would be the shard that owns (MinKey, meta: "A"].
    const updateOneCmd = {
        update: coll.getName(),
        updates: [{
            q: {[metaFieldName]: "B", f: {$gt: 103}},
            u: {$set: {[metaFieldName]: "A"}},
            multi: false
        }]
    };
    jsTestLog(`Running update one: ${tojson(updateOneCmd)}`);

    // Retryable update one command can modify the shard key.
    const session = testDB.getMongo().startSession({retryWrites: true});
    const sessionDB = session.getDatabase(testDB.getName());

    // For now, we don't allow update one to modify the shard key at all.
    const res = assert.commandFailedWithCode(
        sessionDB.runCommand(updateOneCmd), 7717801, `cmd = ${tojson(updateOneCmd)}`);

    assert.sameMembers(docs, coll.find().toArray(), "Collection contents did not match");
})();

// TODO SERVER-76871 This test case should succeed without the error and be enabled.
(function testFindOneAndUpdateModifyingMetaShardKey() {
    // This will create a sharded collection with 2 chunks: (MinKey, meta: "A"] and [meta: "B",
    // MaxKey).
    const coll = prepareShardedCollection(
        {collName: getCallerName(1), initialDocList: docs, includeMeta: true});

    // This findAndModify command tries to update doc5_b_f104 into {_id: 5, meta: "A", f: 104}. The
    // owning shard would be the shard that owns (MinKey, meta: "A"].
    const findOneAndUpdateCmd = {
        findAndModify: coll.getName(),
        query: {[metaFieldName]: "B", f: {$gt: 103}},
        update: {$set: {[metaFieldName]: "A"}},
    };
    jsTestLog(`Running findAndModify update: ${tojson(findOneAndUpdateCmd)}`);

    // FindAndModify command in a transaction can modify the shard key.
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(testDB.getName());
    session.startTransaction();

    // For now, we don't allow findAndModify update to modify the shard key at all.
    const res = assert.commandFailedWithCode(
        sessionDB.runCommand(findOneAndUpdateCmd), 7717801, `cmd = ${tojson(findOneAndUpdateCmd)}`);

    session.abortTransaction();

    assert.sameMembers(docs, coll.find().toArray(), "Collection contents did not match");
})();

// TODO SERVER-76871 This test case should succeed without the error and be enabled.
(function testFindOneAndUpdateModifyingTimeShardKey() {
    // This will create a sharded collection with 2 chunks: [MinKey,
    // 'splitTimePointBetweenTwoShards') and ['splitTimePointBetweenTwoShards', MaxKey).
    const coll = prepareShardedCollection(
        {collName: getCallerName(1), initialDocList: docs, includeMeta: false});

    // This findAndModify command tries to update doc1_a_nofields into {_id: 1, tag: "A",
    // time: generateTimeValue(8)}. The owning shard would be the shard that owns [MinKey,
    // 'splitTimePointBetweenTwoShards').
    const findOneAndUpdateCmd = {
        findAndModify: coll.getName(),
        query: {[timeFieldName]: generateTimeValue(1)},
        update: {$set: {[timeFieldName]: generateTimeValue(8)}},
    };
    jsTestLog(`Running findAndModify update: ${tojson(findOneAndUpdateCmd)}`);

    // FindAndModify command in a transaction can modify the shard key.
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(testDB.getName());
    session.startTransaction();

    // For now, we don't allow findAndModify update to modify the shard key at all.
    const res = assert.commandFailedWithCode(
        sessionDB.runCommand(findOneAndUpdateCmd), 7717801, `cmd = ${tojson(findOneAndUpdateCmd)}`);

    session.abortTransaction();

    assert.sameMembers(docs, coll.find().toArray(), "Collection contents did not match");
});
*/

tearDownShardedCluster();
})();
