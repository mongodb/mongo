/**
 * Tests that attempting to create a TTL index on a capped collection will fail.
 */

(function() {
"use strict";

// Ensure that on an uncapped collection, both non-TTL and TTL indexes can be created
const uncappedColl = db.getCollection(jsTestName() + "_uncapped");
uncappedColl.drop();
assert.commandWorked(db.createCollection(uncappedColl.getName(), {capped: false}));
assert.commandWorked(uncappedColl.createIndex({foo: 1}));
assert.commandWorked(uncappedColl.createIndex({bar: 1}, {expireAfterSeconds: 10}));

// Ensure that on a capped collection, a TTL index cannot be created.
const cappedColl = db.getCollection(jsTestName() + "_capped");
cappedColl.drop();
assert.commandWorked(db.createCollection(cappedColl.getName(), {capped: true, size: 102400}));
assert.commandWorked(cappedColl.createIndex({foo: 1}));
assert.commandFailedWithCode(cappedColl.createIndex({bar: 1}, {expireAfterSeconds: 10}),
                             ErrorCodes.CannotCreateIndex);
})();
