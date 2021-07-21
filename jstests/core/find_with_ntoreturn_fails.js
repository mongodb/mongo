/**
 * Tests that 'runCommand' with 'ntoreturn' throws the expected error code and that 'limit' and
 * 'batchSize' cannot be used alongside of 'ntoreturn'.
 * @tags: [requires_fcv_51]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("test_ntoreturn");
assert.commandFailedWithCode(testDB.runCommand({find: "coll", ntoreturn: 1}), [5746101, 5746102]);
assert.commandFailedWithCode(testDB.runCommand({find: "coll", limit: 1, ntoreturn: 1}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(testDB.runCommand({find: "coll", batchSize: 1, ntoreturn: 1}),
                             ErrorCodes.BadValue);
}());
