// @tags: [
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]

assert.writeOK(db.evalprep.insert({}), "db must exist for eval to succeed");
db.evalprep.drop();

assert.eq(6, db.eval("5 + 1"), "A");
assert.throws(function(z) {
    db.eval("5 + function x; + 1");
});
