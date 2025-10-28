/**
 * Tests that filtering out the resume point produces a ChangeStreamFatalError.
 *
 * TODO SERVER-71565 Revisit this test if we make this no longer produce a fatal error.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = jsTestName();
const testColl = db.getCollection(collName);
assertDropAndRecreateCollection(db, collName);

const csCursor1 = testColl.watch();

// Insert two documents into the collection.
testColl.insertMany([{_id: 1}, {_id: 2}]);

// Retrieve the change stream events for the inserts.
assert.soon(() => csCursor1.hasNext(), "expecting the first change stream event");
const event1 = csCursor1.next();
assert.soon(() => csCursor1.hasNext(), "expecting the second change stream event");
const event2 = csCursor1.next();

// Create a new change stream cursor filtering out the resume point (SERVER-71565) while projecting out the '_id' field.
const csCursor2 = testColl.watch([{$match: {"fullDocument._id": {$gt: 1}}}, {$project: {fullDocument: 1, _id: 0}}], {
    resumeAfter: event1._id,
    batchSize: 0,
});

// As long as SERVER-71565 is not fixed, we expect the new change stream to throw a fatal error
// because the resume token is not present in the new stream.
assert.throwsWithCode(() => csCursor2.hasNext(), ErrorCodes.ChangeStreamFatalError);
