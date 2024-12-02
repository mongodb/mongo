/**
 * Tests shard key updates on a sharded timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
    generateTimeValue,
    getCallerName,
    metaFieldName,
    prepareShardedCollection,
    setUpShardedCluster,
    tearDownShardedCluster,
    testDB,
    timeFieldName
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

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
    assert.commandFailedWithCode(testDB.runCommand(updateMultiCmd),
                                 ErrorCodes.InvalidOptions,
                                 `cmd = ${tojson(updateMultiCmd)}`);
    assert.sameMembers(docs, coll.find().toArray(), "Collection contents did not match");
})();

(function testUpdateOneModifyingShardKey() {
    // This will create a sharded collection with 2 chunks: (MinKey, meta: "A"] and [meta: "B",
    // MaxKey).
    const coll = prepareShardedCollection(
        {collName: getCallerName(1), initialDocList: docs, includeMeta: true});

    // Update one command as retryable write can modify the shard key.
    const session = testDB.getMongo().startSession({retryWrites: true});
    const sessionDB = session.getDatabase(testDB.getName());

    // This update command tries to update doc5_b_f104 into {_id: 5, meta: "A", f: 104}. The owning
    // shard would be the shard that owns (MinKey, meta: "A"].
    const query = {[metaFieldName]: "B", f: {$gt: 103}};
    const update = {$set: {[metaFieldName]: "A"}};

    jsTestLog(`Running updateOne: {q: ${tojson(query)}, u: ${tojson(update)}}`);

    const result = assert.commandWorked(sessionDB.runCommand({
        update: coll.getName(),
        updates: [{q: query, u: update, multi: false}],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1)
    }));
    assert.eq(1, result.nModified, tojson(result));

    assert.docEq({_id: 5, [metaFieldName]: "A", f: 104, [timeFieldName]: generateTimeValue(5)},
                 coll.findOne({_id: 5}),
                 "Document was not updated correctly " + tojson(coll.find().toArray()));
})();

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
        new: true,
    };
    jsTestLog(`Running findAndModify update: ${tojson(findOneAndUpdateCmd)}`);

    // As of now, shard key update is only allowed in retryable writes or transactions when
    // 'featureFlagUpdateDocumentShardKeyUsingTransactionApi' is turned off and findAndModify on
    // timeseries collections does not support retryable writes. So we should use transaction here.
    //
    // TODO SERVER-67429 or SERVER-76583 Relax this restriction.
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(testDB.getName());
    let expectedDocs;
    withTxnAndAutoRetryOnMongos(session, () => {
        const res = assert.commandWorked(sessionDB.runCommand(findOneAndUpdateCmd));
        assert.eq(1, res.lastErrorObject.n, "Expected 1 document to be updated");
        assert.eq(
            true, res.lastErrorObject.updatedExisting, "Expected existing document to be updated");
        const updatedDoc = Object.assign(doc5_b_f104, {[metaFieldName]: "A"});
        assert.docEq(updatedDoc, res.value, "Wrong new document");
        expectedDocs = docs.filter(doc => doc._id !== 5);
        expectedDocs.push(updatedDoc);
    });

    assert.sameMembers(expectedDocs, coll.find().toArray(), "Collection contents did not match");
})();

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

    // As of now, shard key update is allowed in retryable writes or transactions when 'featureFlag-
    // UpdateDocumentShardKeyUsingTransactionApi' is turned off and findAndModify on timeseries
    // collections does not support retryable writes. So we should use transaction here.
    //
    // TODO SERVER-67429 or SERVER-76583 Relax this restriction.
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(testDB.getName());
    withTxnAndAutoRetryOnMongos(session, () => {
        const res = assert.commandWorked(sessionDB.runCommand(findOneAndUpdateCmd));
        assert.eq(1, res.lastErrorObject.n, "Expected 1 document to be updated");
        assert.eq(
            true, res.lastErrorObject.updatedExisting, "Expected existing document to be updated");
        assert.docEq(doc1_a_nofields, res.value, "Wrong old document");
    });

    const updatedDoc = Object.assign(doc1_a_nofields, {[timeFieldName]: generateTimeValue(8)});
    let expectedDocs = docs.filter(doc => doc._id !== 1);
    expectedDocs.push(updatedDoc);
    assert.sameMembers(expectedDocs, coll.find().toArray(), "Collection contents did not match");
})();

tearDownShardedCluster();
