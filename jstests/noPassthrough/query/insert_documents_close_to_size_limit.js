/**
 * Tests that document inserts with documents that are close to the size limit and that have larger
 * _id values can be successfully inserted into all nodes in a replica set. This implicitly tests
 * the message size limits that are applied on messages received by the secondaries' oplog fetcher.
 */
import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = jsTestName();
const kCollName = 'coll';

function runTest(coll) {
    // Generate _id values with different characters so we avoid duplicate key errors.
    let nextChar = 'a'.charCodeAt(0);
    const nextIdChar = () => String.fromCharCode(nextChar++);

    const generateDoc = (idLength, payloadLength) => {
        return {_id: nextIdChar().repeat(idLength), payload: 'a'.repeat(payloadLength)};
    };

    // The insert needs to be acknowledged by all nodes in the replica set, not just the primary.
    const runInsert = (doc) => coll.insert(doc, {writeConcern: {w: 3}});

    // Test that inserting large documents into all nodes of the replica set works.
    // The documents created below have total sizes that are below the BSON object size limit, but
    // as the _id value gets repeated in oplog entries, the examples below are already at the fringe
    // of what can be inserted. Note that the error code returned by the shell is 'BadValue' and not
    // 'BSONObjectTooLarge'.
    assert.commandWorked(runInsert(generateDoc(16384, 16760803)));
    assert.commandFailedWithCode(runInsert(generateDoc(16384, 16760804)), ErrorCodes.BadValue);
    assert.commandWorked(runInsert(generateDoc(16385, 16760802)));
    assert.commandFailedWithCode(runInsert(generateDoc(16385, 16760803)), ErrorCodes.BadValue);
    assert.commandWorked(runInsert(generateDoc(16485, 16760702)));
    assert.commandFailedWithCode(runInsert(generateDoc(16485, 16760703)), ErrorCodes.BadValue);

    assert.eq(3, coll.count());
}

function runReplicaSetTest() {
    const replTest = new ReplSetTest({
        name: jsTestName(),
        nodes: 3,
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
    });

    replTest.startSet();
    replTest.initiate();
    const db = replTest.getPrimary().getDB(kDbName);
    const coll = assertDropAndRecreateCollection(db, kCollName);
    runTest(coll);
    replTest.stopSet();
}

runReplicaSetTest();
