/**
 * Tests that document inserts with documents that are close to the size limit and that have larger
 * _id values can be successfully inserted into all nodes in a replica set. This implicitly tests
 * the message size limits that are applied on messages received by the secondaries' oplog fetcher.
 *
 * @tags: [
 *     # This test can be slow to execute. Exclude from any build variants that are known to be
 *     # slow.
 *     incompatible_aubsan,
 *     resource_intensive,
 *     tsan_incompatible,
 * ]
 */
import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";

const kDbName = 'insert_docs';
const kCollName = 'coll';

function runTest(db) {
    const coll = db[kCollName];

    // Generate _id values with different characters so we avoid duplicate key errors.
    let nextChar;
    const resetNextChar = () => { nextChar = 'a'.charCodeAt(0); };
    const getNextChar = () => String.fromCharCode(nextChar++);

    const runInsert = (idLength, payloadLength) => {
        const doc = {_id: getNextChar().repeat(idLength), payload: 'a'.repeat(payloadLength)};

        jsTestLog(`Inserting document with id length ${idLength}, payload length ${payloadLength}`);

        // The insert needs to be acknowledged by all nodes in the replica set, not just the
        // primary.
        return coll.insert(doc, {writeConcern: {w: 3}});
    };

    const kMaxUserSize = 16 << 20;
    const maxPayloadLengthForIdLength = (idLength) => {
        // Overhead:
        // - 4 bytes for object size
        // - 1 byte for string type tag
        // - 4 bytes for "_id" field name plus trailing \0 byte
        // - 4 bytes for string field length
        // - 1 byte for trailing \0 byte for string value
        // - 1 byte for string type tag
        // - 8 bytes for "payload" field name plus trailing \0 byte
        // - 4 bytes for string field length
        // - 1 byte for trailing \0 byte for string value
        // - 1 byte for trailing \0 byte for object
        // ========================================
        // = 29 bytes total overhead
        //
        // This formula is accurate for all _id lengths <= 16xxx.
        // It does not work accurately with arbitrary large _id values, because the _id value
        // is duplicated in the oplog entry, and the total size of the oplog entry may then
        // exceed the size limit when using too large _ids.
        return kMaxUserSize - 29 - idLength;
    };

    [1, 10, 100, 1000, 10000].forEach((idLength) => {
        coll.remove({});

        resetNextChar();

        // Test that inserting large documents into all nodes of the replica set works.
        // The documents created below have total sizes that are below the BSON object size limit,
        // but as the _id value gets repeated in oplog entries, the examples below are already at
        // the fringe of what can be inserted.
        const maxPayloadLength = maxPayloadLengthForIdLength(idLength);
        assert.commandWorked(runInsert(idLength, maxPayloadLength));
        assert.commandWorked(runInsert(idLength + 1, maxPayloadLength - 1));
        assert.commandWorked(runInsert(idLength - 1, maxPayloadLength + 1));

        assert.soon(() => {
            const count = coll.find({}).readConcern('majority').itcount();
            return count == 3;
        });

        assert.commandFailedWithCode(
            runInsert(idLength, maxPayloadLength + 1), ErrorCodes.BSONObjectTooLarge);
        assert.commandFailedWithCode(
            runInsert(idLength + 1, maxPayloadLength), ErrorCodes.BSONObjectTooLarge);
        assert.commandFailedWithCode(
            runInsert(idLength + 1, maxPayloadLength + 1), ErrorCodes.BSONObjectTooLarge);
    });

    // Now test documents with larger _id values. Large _id values can be a problem because the _id
    // is repeated in the oplog entry. The size of an oplog entry is affected the size of the
    // actually inserted document,
    // the size of the _id value and a few other factors (e.g. the size of the namespace string).

    // Here, we produce documents with large _id values and a large payload. The documents
    // themselves are exactly 16MiB large, and thus permitted. However, as the _id value is
    // duplicated in the oplog entry, and because there are other fields that are stored in an
    // oplog entry, the maximum _id length usable together with a large document payload is limited
    // to slightly less than 16KiB. The value of 135 has been determined for this particular test,
    // using the namespace string used by this test. It can be different for other namespace
    // values.
    const kMaxIdLengthUsableInOplogEntry = (16 << 10) - 135;

    const runInsertForIdLength = (idLength) => {
        return runInsert(idLength, maxPayloadLengthForIdLength(idLength));
    };

    // Still fits into oplog.
    assert.commandWorked(runInsertForIdLength(kMaxIdLengthUsableInOplogEntry));

    // Slighty too large for oplog.
    assert.commandFailedWithCode(
        runInsertForIdLength(kMaxIdLengthUsableInOplogEntry + 1), ErrorCodes.BSONObjectTooLarge);

    // Vastly too large for oplog.
    assert.commandFailedWithCode(
        runInsertForIdLength(kMaxIdLengthUsableInOplogEntry * 2), ErrorCodes.BSONObjectTooLarge);

    // Final check: insert a small document to verify that replication still works.
    coll.remove({});
    assert.commandWorked(coll.insert({_id: "test"}, {writeConcern: {w: 3}}));

    assert.soon(() => {
        const count = coll.find({}).readConcern('majority').itcount();
        return count == 1;
    });
}

const replTest = new ReplSetTest({nodes: 3});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary().getDB(kDbName));
replTest.stopSet();
