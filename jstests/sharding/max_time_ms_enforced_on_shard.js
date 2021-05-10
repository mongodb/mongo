//
// Test that when maxTimeMS is being used in a sharded cluster, that it is enforced on the mongod,
// not just on the mongos.
//

(function() {
'use strict';

const dbName = "test";
const collName = "coll";

const st = new ShardingTest({shards: 1, mongos: 1});
const mongosDB = st.s0.getDB(dbName);
let coll = mongosDB.getCollection(collName);

const sessionOptions = {
    causalConsistency: false
};
const session = mongosDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

// Disable maxTime enforcement on mongos so we can verify it's happening on mongod.
assert.commandWorked(
    mongosDB.adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: 'alwaysOn'}));

jsTestLog("Creating collection with insert.");
assert.commandWorked(sessionColl.insert({_id: 0}));

jsTestLog("Starting the transaction.");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));

jsTestLog("Run a separate write.");
assert.commandFailedWithCode(
    coll.runCommand({insert: collName, documents: [{_id: 1, nonTxn: 1}], maxTimeMS: 1000}),
    ErrorCodes.MaxTimeMSExpired);

jsTestLog("Aborting the transaction.");
session.abortTransaction();

jsTestLog("Checking document set.");
// We would expect only document {_id: 0} to be present in the database, since the transaction above
// was aborted and the write to insert {_id: 1, nonTxn: 1} failed with a MaxTimeMS error. If
// the timeout was only enforced on the mongos, however, then the write on the mongod might slip
// in once the transaction is aborted.
let docs = coll.find().toArray();
assert.sameMembers(docs, [{_id: 0}]);

st.stop();
})();
