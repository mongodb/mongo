/**
 * Ensures that the options passed in for TTL indexes are validated during index creation.
 *
 * @tags: [
 *     requires_fcv_61,
 *     requires_ttl_index,
 * ]
 */
(function() {
'use strict';

let coll = db.core_ttl_index_options;
coll.drop();

// Ensure that any overflows are caught when converting from seconds to milliseconds.
assert.commandFailedWithCode(
    coll.createIndexes([{x: 1}], {expireAfterSeconds: 9223372036854775808}),
    ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndexes([{x: 1}], {expireAfterSeconds: 9999999999999999}),
                             ErrorCodes.CannotCreateIndex);

// Ensure that we can provide a time that is larger than the current epoch time.
let secondsSinceEpoch = Date.now() / 1000;
assert.commandWorked(
    coll.createIndexes([{x_before_epoch: 1}], {expireAfterSeconds: secondsSinceEpoch + 1000}));

// 'expireAfterSeconds' cannot be less than 0.
assert.commandFailedWithCode(coll.createIndexes([{x: 1}], {expireAfterSeconds: -1}),
                             ErrorCodes.CannotCreateIndex);
assert.commandWorked(coll.createIndexes([{z: 1}], {expireAfterSeconds: 0}));

// Compound indexes are not support with TTL indexes.
assert.commandFailedWithCode(coll.createIndexes([{x: 1, y: 1}], {expireAfterSeconds: 100}),
                             ErrorCodes.CannotCreateIndex);

// 'expireAfterSeconds' should be a number.
assert.commandFailedWithCode(coll.createIndexes([{x: 1}], {expireAfterSeconds: "invalidOption"}),
                             ErrorCodes.CannotCreateIndex);

// Using 'expireAfterSeconds' as an index key is valid, but doesn't create a TTL index.
assert.commandWorked(coll.createIndexes([{x: 1, expireAfterSeconds: 3600}]));

// Create a valid TTL index.
assert.commandWorked(coll.createIndexes([{x: 1}, {y: 1}], {expireAfterSeconds: 3600}));
}());
