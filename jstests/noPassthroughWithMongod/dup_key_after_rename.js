// SERVER-47647 Test that duplicate key error message has correct namespace after rename
(function() {
"use strict";
const before = db.dup_key_before_rename;
before.drop();

const after = db.dup_key_after_rename;
after.drop();

assert.commandWorked(before.insert({_id: 1}));
assert.commandWorked(before.renameCollection(after.getName()));
const res = after.insert({_id: 1});
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
const err = res.getWriteError();
assert.gt(
    err.errmsg.indexOf(after.getName()), 0, "error message does not contain new collection name");
})();
