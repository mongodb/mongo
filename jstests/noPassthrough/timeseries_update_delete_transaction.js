/**
 * Tests that deleting and updating time-series collections from a multi-document transaction is
 * disallowed.
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB(jsTestName());

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(testDB.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    rst.stopSet();
    return;
}

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
assert.commandFailedWithCode(sessionColl.update({[metaFieldName]: "a"}, {"$set": {"b": "a"}}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
session.endSession();
rst.stopSet();
})();
