/**
 * Tests that the change stream with 'fullDocument: updateLookup' option performs the lookup only by
 * nss by default and does an additional collection UUID check when
 * 'matchCollectionUUIDForUpdateLookup: true' option is set.
 */
import {ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {
    assertCreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function assertUpdateLookupResultBeforeOp(op, changeStreamOptions) {
    jsTest.log(`Running change stream with update lookup test after '${op}' occurred with ${
        tojson(changeStreamOptions)}`);

    const collNameA = "collA";
    const collNameB = "collB";
    let cst;

    function tearDown() {
        cst.cleanUp();
        assertDropCollection(db, collNameA);
        assertDropCollection(db, collNameB);
    }

    assertCreateCollection(db, collNameA);
    cst = new ChangeStreamTest(db);

    let cursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {...changeStreamOptions, fullDocument: "updateLookup"}}],
        collection: collNameA
    });

    // Insert 'doc' into 'collA' and ensure it is seen in the change stream.
    const doc = {_id: 0, a: 1};
    assert.commandWorked(db.getCollection(collNameA).insert(doc));
    let expected = {
        documentKey: {_id: doc._id},
        fullDocument: doc,
        ns: {db: "test", coll: collNameA},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Update the 'doc' in order to generate the update event.
    assert.commandWorked(db.getCollection(collNameA).update({_id: doc._id}, {$inc: {a: 1}}));
    const updatedDocInCollA = {...doc, a: 2};

    // In case where a change stream is opened with the 'checkUUIDOnUpdateLookup' flag set to true,
    // no 'fullDocument' should be returned to the user as the collection on which 'updateLookup'
    // has been performed has a different UUID from the collection on which change stream has been
    // opened. In case of the flag being set to false or not present return the latest document on
    // the collection with the same name.
    let expectedFullDocument;
    if (op === 'renameCollection') {
        // Rename collection collA -> collB.
        assert.commandWorked(db.getCollection(collNameA).renameCollection(collNameB));

        // Create new collection with the old name, "collA", (yet UUID will be different) and insert
        // document with the same id.
        assertCreateCollection(db, collNameA);
        const newDocInNewCollA = {
            ...doc,
            b: "extra field in the new document in the new collection"
        };
        assert.commandWorked(db.getCollection(collNameA).insert(newDocInNewCollA));
        expectedFullDocument =
            changeStreamOptions.matchCollectionUUIDForUpdateLookup ? null : newDocInNewCollA;
    } else if (op === 'shardCollection') {
        if (FixtureHelpers.isSharded(db.getCollection(collNameA))) {
            jsTest.log("Early exit, because collection 'collA' is already sharded");
            tearDown();
            return;
        }

        // Sharding the unsharded collection does not change its UUID, therefore regardless of
        // 'matchCollectionUUIDForUpdateLookup' being set or not, we should observe the
        // 'updatedDocInCollA' on updateLookup.
        assert.commandWorked(db.adminCommand({
            shardCollection: db.getCollection(collNameA).getFullName(),
            key: {_id: 1},
            numInitialChunks: 2
        }));
        expectedFullDocument = updatedDocInCollA;
    } else if (op === 'reshardCollection') {
        if (!FixtureHelpers.isSharded(db.getCollection(collNameA))) {
            jsTest.log("Early exit, because collection 'collA' is not sharded");
            tearDown();
            return;
        }

        // Reshard the collection in order to generate the new collection with the same name, but
        // different UUID.
        assert.commandWorked(db.adminCommand({
            reshardCollection: db.getCollection(collNameA).getFullName(),
            key: {_id: 1},
            numInitialChunks: 2
        }));
        expectedFullDocument =
            changeStreamOptions.matchCollectionUUIDForUpdateLookup ? null : updatedDocInCollA;
    }

    expected = {
        documentKey: {_id: doc._id},
        fullDocument: expectedFullDocument,
        ns: {db: "test", coll: collNameA},
        operationType: "update",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    tearDown();
}

assertUpdateLookupResultBeforeOp('renameCollection', {});
assertUpdateLookupResultBeforeOp('renameCollection', {matchCollectionUUIDForUpdateLookup: false});
assertUpdateLookupResultBeforeOp('renameCollection', {matchCollectionUUIDForUpdateLookup: true});

if (FixtureHelpers.isMongos(db)) {
    assertUpdateLookupResultBeforeOp('shardCollection', {});
    assertUpdateLookupResultBeforeOp('shardCollection',
                                     {matchCollectionUUIDForUpdateLookup: false});
    assertUpdateLookupResultBeforeOp('shardCollection', {matchCollectionUUIDForUpdateLookup: true});

    assertUpdateLookupResultBeforeOp('reshardCollection', {});
    assertUpdateLookupResultBeforeOp('reshardCollection',
                                     {matchCollectionUUIDForUpdateLookup: false});
    assertUpdateLookupResultBeforeOp('reshardCollection',
                                     {matchCollectionUUIDForUpdateLookup: true});
}
