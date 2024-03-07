/**
 * Tests mongos behavior when reading against views in a transaction.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    flushRoutersAndRefreshShardMetadata
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const shardedDbName = "shardedDB";
const shardedCollName = "sharded";
const shardedViewName = "sharded_view";

const unshardedDbName = "unshardedDB";
const unshardedCollName = "unsharded";
const unshardedViewName = "unsharded_view";

const viewOnShardedViewName = "sharded_view_view";

function setUpUnshardedCollectionAndView(st, session, primaryShard) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: unshardedDbName, primaryShard: primaryShard}));

    assert.commandWorked(st.s.getDB(unshardedDbName)[unshardedCollName].insert(
        {_id: 1, x: "unsharded"}, {writeConcern: {w: "majority"}}));

    const unshardedView = session.getDatabase(unshardedDbName)[unshardedViewName];
    assert.commandWorked(unshardedView.runCommand(
        "create", {viewOn: unshardedCollName, pipeline: [], writeConcern: {w: "majority"}}));

    return unshardedView;
}

function setUpShardedCollectionAndView(st, session, primaryShard) {
    const ns = shardedDbName + "." + shardedCollName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: shardedDbName, primaryShard: primaryShard}));
    assert.commandWorked(st.s.getDB(shardedDbName)[shardedCollName].insert(
        {_id: -1}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(st.s.getDB(shardedDbName)[shardedCollName].insert(
        {_id: 1}, {writeConcern: {w: "majority"}}));

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));

    const shardedView = session.getDatabase(shardedDbName)[shardedViewName];
    assert.commandWorked(shardedView.runCommand(
        "create", {viewOn: shardedCollName, pipeline: [], writeConcern: {w: "majority"}}));

    flushRoutersAndRefreshShardMetadata(st, {ns, dbNames: [shardedDbName, unshardedDbName]});

    return shardedView;
}

const st = new ShardingTest({shards: 2, mongos: 1});
const session = st.s.startSession();

// Set up an unsharded collection on shard0.
const unshardedView = setUpUnshardedCollectionAndView(st, session, st.shard0.shardName);

// Set up a sharded collection with one chunk on each shard in a database with shard0 as its
// primary shard.
const shardedView = setUpShardedCollectionAndView(st, session, st.shard0.shardName);

// Set up a view on the sharded view, in the same database.
const viewOnShardedView = session.getDatabase(shardedDbName)[viewOnShardedViewName];
assert.commandWorked(viewOnShardedView.runCommand(
    "create", {viewOn: shardedViewName, pipeline: [], writeConcern: {w: "majority"}}));

//
// The first statement a participant shard receives reading from a view should succeed.
//

function readFromViewOnFirstParticipantStatement(session, view, viewFunc, numDocsExpected) {
    session.startTransaction();
    assert.eq(viewFunc(view), numDocsExpected);
    assert.commandWorked(session.commitTransaction_forTesting());
}

// Unsharded view.
readFromViewOnFirstParticipantStatement(session, unshardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 1);
readFromViewOnFirstParticipantStatement(session, unshardedView, (view) => {
    return view.distinct("_id").length;
}, 1);
readFromViewOnFirstParticipantStatement(session, unshardedView, (view) => {
    return view.find().itcount();
}, 1);

// Sharded view.
readFromViewOnFirstParticipantStatement(session, shardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 2);
readFromViewOnFirstParticipantStatement(session, shardedView, (view) => {
    return view.distinct("_id").length;
}, 2);
readFromViewOnFirstParticipantStatement(session, shardedView, (view) => {
    return view.find().itcount();
}, 2);

// View on sharded view.
readFromViewOnFirstParticipantStatement(session, viewOnShardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 2);
readFromViewOnFirstParticipantStatement(session, viewOnShardedView, (view) => {
    return view.distinct("_id").length;
}, 2);
readFromViewOnFirstParticipantStatement(session, viewOnShardedView, (view) => {
    return view.find().itcount();
}, 2);

//
// A later statement a participant shard receives reading from a view should succeed.
//

function readFromViewOnLaterParticipantStatement(session, view, viewFunc, numDocsExpected) {
    session.startTransaction();
    assert.eq(view.aggregate({$match: {}}).itcount(), numDocsExpected);
    assert.eq(viewFunc(view), numDocsExpected);
    assert.commandWorked(session.commitTransaction_forTesting());
}

// Unsharded view.
readFromViewOnLaterParticipantStatement(session, unshardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 1);
readFromViewOnLaterParticipantStatement(session, unshardedView, (view) => {
    return view.distinct("_id").length;
}, 1);
readFromViewOnLaterParticipantStatement(session, unshardedView, (view) => {
    return view.find().itcount();
}, 1);

// Sharded view.
readFromViewOnLaterParticipantStatement(session, shardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 2);
readFromViewOnLaterParticipantStatement(session, shardedView, (view) => {
    return view.distinct("_id").length;
}, 2);
readFromViewOnLaterParticipantStatement(session, shardedView, (view) => {
    return view.find().itcount();
}, 2);

// View on sharded view.
readFromViewOnLaterParticipantStatement(session, viewOnShardedView, (view) => {
    return view.aggregate({$match: {}}).itcount();
}, 2);
readFromViewOnLaterParticipantStatement(session, viewOnShardedView, (view) => {
    return view.distinct("_id").length;
}, 2);
readFromViewOnLaterParticipantStatement(session, viewOnShardedView, (view) => {
    return view.find().itcount();
}, 2);

//
// Transactions on shards that return a view resolution error on the first statement remain
// aborted if the shard is not targeted by the retry on the resolved namespace.
//
// This may happen when reading from a sharded view, because mongos will target the primary
// shard first to resolve the view, but the retry on the underlying sharded collection is not
// guaranteed to target the primary again.
//

// Assumes the request in viewFunc does not target the primary shard, Shard0.
function primaryShardNotReTargeted_FirstStatement(session, view, viewFunc, numDocsExpected) {
    session.startTransaction();
    assert.eq(viewFunc(view), numDocsExpected);

    // There should not be an in-progress transaction on the primary shard.
    assert.commandFailedWithCode(st.rs0.getPrimary().getDB("foo").runCommand({
        find: "bar",
        lsid: session.getSessionId(),
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);

    assert.commandWorked(session.commitTransaction_forTesting());

    // The transaction should not have been committed on the primary shard.
    assert.commandFailedWithCode(st.rs0.getPrimary().getDB("foo").runCommand({
        find: "bar",
        lsid: session.getSessionId(),
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);
}

// This is only possible against sharded views.
primaryShardNotReTargeted_FirstStatement(session, shardedView, (view) => {
    return view.aggregate({$match: {_id: 1}}).itcount();
}, 1);
primaryShardNotReTargeted_FirstStatement(session, shardedView, (view) => {
    return view.distinct("_id", {_id: {$gte: 1}}).length;
}, 1);
primaryShardNotReTargeted_FirstStatement(session, shardedView, (view) => {
    return view.find({_id: 1}).itcount();
}, 1);

// View on sharded view.
primaryShardNotReTargeted_FirstStatement(session, viewOnShardedView, (view) => {
    return view.aggregate({$match: {_id: 1}}).itcount();
}, 1);
primaryShardNotReTargeted_FirstStatement(session, viewOnShardedView, (view) => {
    return view.distinct("_id", {_id: {$gte: 1}}).length;
}, 1);
primaryShardNotReTargeted_FirstStatement(session, viewOnShardedView, (view) => {
    return view.find({_id: 1}).itcount();
}, 1);

//
// Shards do not abort on a view resolution error if they have already completed a statement for
// a transaction.
//

// Assumes the primary shard for view is Shard0.
function primaryShardNotReTargeted_LaterStatement(session, view, viewFunc, numDocsExpected) {
    session.startTransaction();
    // Complete a statement on the primary shard for the view.
    assert.eq(view.aggregate({$match: {_id: -1}}).itcount(), 1);
    // Targets the primary first, but the resolved retry only targets Shard1.
    assert.eq(viewFunc(view), numDocsExpected);
    assert.commandWorked(session.commitTransaction_forTesting());
}

// This is only possible against sharded views.
primaryShardNotReTargeted_LaterStatement(session, shardedView, (view) => {
    return view.aggregate({$match: {_id: 1}}).itcount();
}, 1);
primaryShardNotReTargeted_LaterStatement(session, shardedView, (view) => {
    return view.distinct("_id", {_id: {$gte: 1}}).length;
}, 1);
primaryShardNotReTargeted_LaterStatement(session, shardedView, (view) => {
    return view.find({_id: 1}).itcount();
}, 1);

// View on sharded view.
primaryShardNotReTargeted_LaterStatement(session, viewOnShardedView, (view) => {
    return view.aggregate({$match: {_id: 1}}).itcount();
}, 1);
primaryShardNotReTargeted_LaterStatement(session, viewOnShardedView, (view) => {
    return view.distinct("_id", {_id: {$gte: 1}}).length;
}, 1);
primaryShardNotReTargeted_LaterStatement(session, viewOnShardedView, (view) => {
    return view.find({_id: 1}).itcount();
}, 1);

//
// Reading from a view using $lookup and $graphLookup should succeed.
//

function assertAggResultEqInTransaction(coll, pipeline, expected) {
    session.startTransaction();
    const resArray = coll.aggregate(pipeline).toArray();
    assert(arrayEq(resArray, expected), tojson({got: resArray, expected: expected}));
    assert.commandWorked(session.commitTransaction_forTesting());
}

// TODO SERVER-84470 Remove this check once lookup on unsplittable collection still on the primary
// is supported
const isTrackUnshardedEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
if (!isTrackUnshardedEnabled) {
    // Set up an unsharded collection to use for $lookup, as lookup into a sharded collection in a
    // transaction is not yet supported.
    // TODO SERVER-39162: Add testing for lookup into sharded collections in a transaction.
    const lookupDbName = "dbForLookup";
    const lookupCollName = "collForLookup";
    assert.commandWorked(
        st.s.getDB(lookupDbName)[lookupCollName].insert({_id: 1}, {writeConcern: {w: "majority"}}));
    const lookupColl = session.getDatabase(unshardedDbName)[unshardedCollName];

    // Lookup the document in the unsharded collection with _id: 1 through the unsharded view.
    assertAggResultEqInTransaction(
        lookupColl,
        [
            {$match: {_id: 1}},
            {
                $lookup:
                    {from: unshardedViewName, localField: "_id", foreignField: "_id", as: "matched"}
            },
            {$unwind: "$matched"},
            {$project: {_id: 1, matchedX: "$matched.x"}}
        ],
        [{_id: 1, matchedX: "unsharded"}]);

    // Find the same document through the view using $graphLookup.
    assertAggResultEqInTransaction(lookupColl,
                                   [
                                     {$match: {_id: 1}},
                                     {
                                       $graphLookup: {
                                           from: unshardedViewName,
                                           startWith: "$_id",
                                           connectFromField: "_id",
                                           connectToField: "_id",
                                           as: "matched"
                                       }
                                     },
                                     {$unwind: "$matched"},
                                     {$project: {_id: 1, matchedX: "$matched.x"}}
                                   ],
                                   [{_id: 1, matchedX: "unsharded"}]);
}
st.stop();
