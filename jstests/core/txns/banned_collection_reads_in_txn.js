// Tests that it is illegal to read from system.views and system.profile within a transaction.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: profile.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_snapshot_read_concern
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const session = db.getMongo().startSession();

// Use a custom database to avoid conflict with other tests.
const testDB = session.getDatabase("no_reads_from_system_colls_in_txn");
assert.commandWorked(testDB.dropDatabase());

testDB.runCommand({create: "foo", viewOn: "bar", pipeline: []});

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(testDB.runCommand({find: "system.views", filter: {}}),
                             [ErrorCodes.OperationNotSupportedInTransaction, 51071]);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(testDB.runCommand({find: "system.profile", filter: {}}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

// The following tests a {find: uuid} command which is not supported on mongos or with replica set
// endpoints.
if (FixtureHelpers.isMongos(testDB) || TestData.testingReplicaSetEndpoint) {
    quit();
}

const collectionInfos =
    new DBCommandCursor(testDB, assert.commandWorked(testDB.runCommand({listCollections: 1})));
let systemViewsUUID = null;
while (collectionInfos.hasNext()) {
    const next = collectionInfos.next();
    if (next.name === "system.views") {
        systemViewsUUID = next.info.uuid;
    }
}
assert.neq(null, systemViewsUUID, "did not find UUID for system.views");

session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(testDB.runCommand({find: systemViewsUUID, filter: {}}),
                             [ErrorCodes.OperationNotSupportedInTransaction, 51070, 7195700]);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
