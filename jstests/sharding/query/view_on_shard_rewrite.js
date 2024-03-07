/**
 * On sharded clusters, reading from views results in the following:
 *  1) mongos executes the view read against the primary shard, agnostic to whether the read
 *     is on a view or an unsharded collection.
 *  2) The primary shard resolves the view and on determining that the underlying collection is
 *     sharded or is unsharded but does not reside on the primary shard, returns an
 *     'CommandOnShardedViewNotSupportedOnMongod' error to mongos, with the resolved view definition
 *     attached.
 *  3) mongos rewrites the query and resolved view definition to be an aggregate on the underlying
 *     collection and executes.
 *
 * @tags: [
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {profilerHasSingleMatchingEntryOrThrow} from "jstests/libs/profiler.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const dbName = "view_on_shard_rewrite";
const collName = "coll";
const viewName = "view";
const collNS = dbName + "." + collName;
const viewNs = dbName + "." + viewName;
const mongosDB = st.s.getDB(dbName);
const mongosView = mongosDB[viewName];

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const shard0Primary = st.rs0.getPrimary();
assert.commandWorked(shard0Primary.getDB(dbName).setProfilingLevel(2, -1));

function insertDocumentsAndCreateView() {
    assert.commandWorked(mongosDB.getCollection(collName).insert([{_id: 1}, {_id: 2}, {_id: 3}]));
    assert.commandWorked(mongosDB.createView(viewName, collName, [{$addFields: {foo: "$_id"}}]));
}

function assertReadOnView(view, expectKickBackToMongos) {
    let comment = UUID();
    jsTest.log("Using comment: " + tojson(comment));

    const result = view.find({}).sort({_id: 1}).comment(comment).toArray();
    assert.eq(result, [{_id: 1, foo: 1}, {_id: 2, foo: 2}, {_id: 3, foo: 3}]);

    // Check if kickback happened using the profile collection.
    if (expectKickBackToMongos) {
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0Primary.getDB(dbName),
            filter: {
                'command.comment': comment,
                ok: 0,
                errCode: ErrorCodes.CommandOnShardedViewNotSupportedOnMongod,
            },
            errorMsgFilter: {ns: viewNs, 'command.comment': comment}
        });
    } else {
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: shard0Primary.getDB(dbName),
            filter: {'command.comment': comment, errCode: {$exists: false}},
            errorMsgFilter: {ns: viewNs, 'command.comment': comment}
        });
    }
}

insertDocumentsAndCreateView();

// Read view on unsharded collection.
assertReadOnView(mongosView, false /* expectKickBackToMongos */);

// Shard the collection.
assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {_id: 1}}));

// Read view on sharded collection.
assertReadOnView(mongosView, true /* expectKickBackToMongos */);

if (FeatureFlagUtil.isPresentAndEnabled(st.s, 'TrackUnshardedCollectionsUponCreation')) {
    // Recreate the collection as unsplittable on the db-primary shard.
    mongosDB[collName].drop();
    mongosDB.runCommand({createUnsplittableCollection: collName});
    insertDocumentsAndCreateView();

    // Read view on unsharded collection on the db-primary shard.
    assertReadOnView(mongosView, false /* expectKickBackToMongos */);

    // Move the collection to the other shard.
    assert.commandWorked(st.s.adminCommand({moveCollection: collNS, toShard: st.shard1.shardName}));
    mongosView.find().itcount();  // Make sure the mongos updates its routing info.

    // Read view on unsharded collection on a shard other than the db-primary.
    assertReadOnView(mongosView, true /* expectKickBackToMongos */);

    // Test that when reading from views within a multi-document transaction, the shard considers
    // the transaction timestamp to decide whether it can read locally.
    {
        mongosDB.getSiblingDB('otherDb').runCommand({createUnsplittableCollection: 'otherColl'});

        let session1 = st.s.startSession();
        session1.startTransaction({readConcern: {level: 'snapshot'}});
        session1.getDatabase('otherDb')['otherColl'].find().itcount();

        let session2 = st.s.startSession();
        session2.startTransaction({readConcern: {level: 'majority'}});
        session2.getDatabase('otherDb')['otherColl'].find().itcount();

        // Move back to db-primary shard.
        assert.commandWorked(
            st.s.adminCommand({moveCollection: collNS, toShard: st.shard0.shardName}));
        mongosView.find().itcount();  // Make sure the mongos updates its routing info.

        // MoveCollection results in a new instance of the collection (different uuid), so the
        // transaction fails with a TransientTransactionError. We'd otherwise expect to have
        // kicked-back to mongos, since shard1 owned the underlying collection at the transaction
        // snapshot.
        assert.throwsWithCode(() => assertReadOnView(session1.getDatabase(dbName)[viewName],
                                                     true /* expectKickBackToMongos */),
                              ErrorCodes.StaleChunkHistory);
        assert.throwsWithCode(() => assertReadOnView(session2.getDatabase(dbName)[viewName],
                                                     true /* expectKickBackToMongos */),
                              ErrorCodes.MigrationConflict);

        session1.abortTransaction();
        session2.abortTransaction();

        // Try the transactions again, with the underlying collection now living on the db-primary
        // shard.
        session1.startTransaction({readConcern: {level: 'snapshot'}});
        session2.startTransaction({readConcern: {level: 'majority'}});
        assertReadOnView(session1.getDatabase(dbName)[viewName], false /* expectKickBackToMongos */)
        assertReadOnView(session1.getDatabase(dbName)[viewName], false /* expectKickBackToMongos */)
        session1.commitTransaction();
        session2.commitTransaction();
    }
}

st.stop();
