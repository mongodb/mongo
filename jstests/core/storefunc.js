// @tags: [
//   requires_fcv_61,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
// ]

(function() {
'use strict';

// Use a private sister database to avoid conflicts with other tests that use system.js
const testdb = db.getSiblingDB("storefunc");
let res;

const s = testdb.system.js;
assert.commandWorked(s.remove({}));
assert.eq(0, s.countDocuments({}));

assert.commandWorked(s.insert({_id: "x", value: "3"}));
assert.eq(1, s.countDocuments({}));

assert.commandWorked(s.remove({_id: "x"}));
assert.eq(0, s.countDocuments({}));
assert.commandWorked(s.insert({_id: "x", value: "4"}));
assert.eq(1, s.countDocuments({}));

assert.eq(4, s.findOne({_id: "x"}).value, "E2 ");

assert.eq(4, s.findOne().value, "setup - F");
assert.commandWorked(s.update({_id: "x"}, {$set: {value: 5}}));
assert.eq(1, s.countDocuments({}));
assert.eq(5, s.findOne().value, "setup - H");

assert.commandWorked(s.update({_id: "x"}, {$set: {value: 6}}));
assert.eq(1, s.countDocuments({}));
assert.eq(6, s.findOne().value, "setup - B");

assert(s.getIndexKeys().length > 0, "no indexes");
assert(s.getIndexKeys()[0]._id, "no _id index");

// Renaming from system.js to another system namespace is an existing
// check handled by both the access control system and command namespace checking.
assert.commandFailedWithCode(s.renameCollection('system.js_old'),
                             [ErrorCodes.IllegalOperation, ErrorCodes.Unauthorized]);
assert.commandFailedWithCode(s.renameCollection('old_system_js'), ErrorCodes.IllegalOperation);
})();
