// This test ensures that listCollections does not conflict with multi-statement transactions
// as a result of taking MODE_S locks that are incompatible with MODE_IX needed for writes.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_snapshot_read_concern
// ]

// TODO (SERVER-39704): Remove the following load after SERVER-39704 is completed
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let dbName = "list_collections_not_blocked";
let mydb = db.getSiblingDB(dbName);
let session = db.getMongo().startSession({causalConsistency: false});
let sessionDb = session.getDatabase(dbName);

mydb.foo.drop({writeConcern: {w: "majority"}});

assert.commandWorked(mydb.createCollection("foo", {writeConcern: {w: "majority"}}));

if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    // Before starting the transaction below, access the collection so it can be implicitly
    // sharded and force all shards to refresh their database versions because the refresh
    // requires an exclusive lock and would block behind the transaction.
    assert.eq(sessionDb.foo.find().itcount(), 0);
    assert.commandWorked(sessionDb.runCommand({listCollections: 1, nameOnly: true}));
}

// TODO (SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(
    session,
    () => {
        assert.commandWorked(sessionDb.foo.insert({x: 1}));

        for (let nameOnly of [false, true]) {
            // Check that both the nameOnly and full versions of listCollections don't block.
            let res = mydb.runCommand({listCollections: 1, nameOnly, maxTimeMS: 20 * 1000});
            assert.commandWorked(res, "listCollections should have succeeded and not timed out");
            let collObj = res.cursor.firstBatch[0];
            // collObj should only have name and type fields.
            assert.eq("foo", collObj.name);
            assert.eq("collection", collObj.type);
            assert(collObj.hasOwnProperty("idIndex") == !nameOnly, tojson(collObj));
            assert(collObj.hasOwnProperty("options") == !nameOnly, tojson(collObj));
            assert(collObj.hasOwnProperty("info") == !nameOnly, tojson(collObj));
        }
    },
    {readConcern: {level: "snapshot"}},
);

session.endSession();
