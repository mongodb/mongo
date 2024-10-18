/*
 * Test that when featureFlagReshardingSkipCloningAndApplying is enabled, a recipient shard that
 * is not going to own any chunks for the collection after resharding would only clone the options
 * and indexes for the collection, i.e. skip cloning documents and fetching/applying oplog entries
 * for it. In addition, test that after failover the skip is respected and the
 * 'approxDocumentsToCopy' and 'approxBytesToCopy' are restored correctly.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

function getDottedField(doc, fieldName) {
    let val = doc;
    const fieldNames = fieldName.split(".");
    for (let i = 0; i < fieldNames.length; i++) {
        val = val[fieldNames[i]];
    }
    return val;
}

function getCollectionUuid(db, collName) {
    const listCollectionRes =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    return listCollectionRes.cursor.firstBatch[0].info.uuid;
}

function checkCollectionOptionsAndIndexes(conn, ns, expectedOptions, expectedIndexes) {
    const coll = conn.getCollection(ns);
    const listIndexesDoc = coll.exists();
    for (let fieldName in expectedOptions) {
        const actual = getDottedField(listIndexesDoc.options, fieldName);
        const expected = expectedOptions[fieldName];
        assert.eq(bsonUnorderedFieldsCompare(actual, expected), 0, {fieldName, actual, expected});
    }

    const actualIndexes = coll.getIndexes();
    expectedIndexes.forEach(expectedIndex => {
        assert(
            actualIndexes.some(actualIndex => bsonWoCompare(actualIndex.key, expectedIndex) != 0),
            {actualIndexes, expectedIndex});
    });
}

function checkMoveCollectionCloningMetrics(
    st, ns, numDocs, numBytes, primaryShardName, toShardName) {
    assert.neq(primaryShardName, toShardName);
    let currentOps;
    assert.soon(() => {
        currentOps = st.s.getDB("admin")
                         .aggregate([
                             {$currentOp: {allUsers: true, localOps: false}},
                             {
                                 $match: {
                                     type: "op",
                                     "originatingCommand.reshardCollection": ns,
                                     recipientState: {$exists: true}
                                 }
                             },
                         ])
                         .toArray();
        if (currentOps.length < 2) {
            return false;
        }
        for (let op of currentOps) {
            if (op.recipientState != "cloning") {
                return false;
            }
        }
        return true;
    }, () => tojson(currentOps));

    assert.eq(currentOps.length, 2, currentOps);
    currentOps.forEach(op => {
        if (op.shard == primaryShardName) {
            assert.eq(op.approxDocumentsToCopy, 0, {op});
            assert.eq(op.approxBytesToCopy, 0, {op});
        } else if (op.shard == toShardName) {
            assert.eq(op.approxDocumentsToCopy, numDocs, {op});
            assert.eq(op.approxBytesToCopy, numBytes, {op});
        } else {
            throw Error("Unexpected shard name " + tojson(op));
        }
    });
}

function checkCollectionExistence(conn, ns, exists) {
    const coll = conn.getCollection(ns);
    if (exists) {
        assert(coll.exists());
    } else {
        assert(!coll.exists());
    }
}

function checkOplogBufferAndConflictStashCollections(conn, collUuid, donorShardName, exists) {
    const oplogBufferNs = "config.localReshardingOplogBuffer." + collUuid + "." + donorShardName;
    checkCollectionExistence(conn, oplogBufferNs, exists);
    const conflictStashNs =
        "config.localReshardingConflictStash." + collUuid + "." + donorShardName;
    checkCollectionExistence(conn, conflictStashNs, exists);
}

function runTest(featureFlagReshardingSkipCloningAndApplying) {
    jsTest.log("Testing with " + tojson({featureFlagReshardingSkipCloningAndApplying}));
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        rs: {
            nodes: 2,
            setParameter: {
                featureFlagReshardingSkipCloningAndApplyingIfApplicable:
                    featureFlagReshardingSkipCloningAndApplying
            }
        }
    });

    // Create an unsharded collection on shard0 (primary shard) and move the collection from
    // shard0 to shard1.
    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName);
    const options = {validator: {x: {$gte: 0}}};

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(db.runCommand(Object.assign({
        create: collName,
    },
                                                     options)));

    const numDocs = 100;
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        const doc = {_id: i, x: i};
        docs.push(doc);
    }
    const numBytes = numDocs * Object.bsonsize({_id: 0, x: 0});
    assert.commandWorked(coll.insert(docs));
    const collUuid = extractUUIDFromObject(getCollectionUuid(db, collName));

    const indexes = [{x: 1}];
    indexes.forEach(index => assert.commandWorked(coll.createIndex(index)));

    const oldShard0Primary = st.rs0.getPrimary();
    const oldShard1Primary = st.rs1.getPrimary();
    checkCollectionOptionsAndIndexes(oldShard0Primary, ns, options, indexes);

    const shard0Indexes = st.rs0.getPrimary().getCollection(ns).getIndexes();
    assert(shard0Indexes.some(index => bsonWoCompare(index.key, {x: 1}) != 0), shard0Indexes);

    // Pause resharding recipients (both shard0 and shard1) at the "cloning" state.
    const shard0CloningFps =
        st.rs0.nodes.map(node => configureFailPoint(node, "reshardingPauseRecipientBeforeCloning"));
    const shard1CloningFps =
        st.rs1.nodes.map(node => configureFailPoint(node, "reshardingPauseRecipientBeforeCloning"));

    // Also pause resharding coordinator before it starts committing.
    const beforePersistingDecisionFps = st.configRS.nodes.map(
        node => configureFailPoint(node, "reshardingPauseCoordinatorBeforeDecisionPersisted"));

    const thread = new Thread((host, ns, toShard) => {
        const mongos = new Mongo(host);
        assert.soonRetryOnAcceptableErrors(() => {
            assert.commandWorked(mongos.adminCommand({moveCollection: ns, toShard}));
            return true;
        }, ErrorCodes.FailedToSatisfyReadPreference);
    }, st.s.host, ns, st.shard1.shardName);
    thread.start();

    checkMoveCollectionCloningMetrics(st,
                                      ns,
                                      numDocs,
                                      numBytes,
                                      st.shard0.shardName /* primaryShard */,
                                      st.shard1.shardName /* toShard */);

    // Trigger a failover on shard0.
    assert.commandWorked(
        oldShard0Primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(oldShard0Primary.adminCommand({replSetFreeze: 0}));
    const newShard0Primary = st.rs0.waitForPrimary();

    // Trigger a failover on shard1.
    assert.commandWorked(
        oldShard1Primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(oldShard1Primary.adminCommand({replSetFreeze: 0}));
    const newShard1Primary = st.rs1.waitForPrimary();

    checkMoveCollectionCloningMetrics(st,
                                      ns,
                                      numDocs,
                                      numBytes,
                                      st.shard0.shardName /* primaryShard */,
                                      st.shard1.shardName /* toShard */);

    shard0CloningFps.forEach(fp => fp.off());
    shard1CloningFps.forEach(fp => fp.off());

    // Verify that shard0 which is a recipient that is not going to own any chunk for the collection
    // after resharding skipped fetching/applying oplog entries.
    beforePersistingDecisionFps.forEach(fp => {
        if (fp.conn == st.configRS.getPrimary()) {
            fp.wait();
        }
    });
    checkOplogBufferAndConflictStashCollections(
        newShard0Primary,
        collUuid,
        st.shard0.shardName,
        !featureFlagReshardingSkipCloningAndApplying /* exists */);
    checkOplogBufferAndConflictStashCollections(
        newShard1Primary, collUuid, st.shard0.shardName, true /* exists */);

    beforePersistingDecisionFps.forEach(fp => fp.off());
    thread.join();

    checkCollectionOptionsAndIndexes(newShard1Primary, ns, options, indexes);
    // Verify that shard0 cloned the options and indexes for the collection.
    checkCollectionOptionsAndIndexes(newShard0Primary, ns, options, indexes);

    assert(coll.drop());
    st.stop();
}

for (let featureFlagReshardingSkipCloningAndApplying of [true, false]) {
    runTest(featureFlagReshardingSkipCloningAndApplying);
}
