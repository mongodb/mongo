/**
 * Tests that deleting and updating time-series collections from a multi-document transaction is
 * disallowed.
 * @tags: [
 *     requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB(jsTestName());

const metaFieldName = "meta";
const timeFieldName = "time";
const collectionName = "t";

assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(
    testDB.createCollection(testDB.getCollection(collectionName).getName(),
                            {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const session = testDB.getMongo().startSession();
const sessionColl = session.getDatabase(jsTestName()).getCollection(collectionName);
session.startTransaction();
// Time-series delete in a multi-document transaction should fail.
assert.commandFailedWithCode(sessionColl.remove({[metaFieldName]: "a"}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

session.startTransaction();
// Time-series update in a multi-document transaction should fail.
assert.commandFailedWithCode(session.getDatabase(jsTestName()).runCommand({
    update: collectionName,
    updates: [{q: {[metaFieldName]: "a"}, u: {"$set": {"b": "a"}}, multi: true}],
}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
session.endSession();
rst.stopSet();