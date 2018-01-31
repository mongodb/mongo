// @tags: [
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]

t = db.jstests_js7;
t.drop();

assert.eq(17, db.eval(function(foo) {
    return foo;
}, 17));
