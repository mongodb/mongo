// Tests for validating arguments of the explain command.

var collName = "jstests_explain_invalid_arguments";
var t = db[collName];
t.drop();
t.insert({});

var explain = db.runCommand({
  explain: { find: collName },
  verbosity: "executionStats"
});
assert.commandWorked(explain);

explain = db.runCommand({
  explain: { find: collName },
  verosity: "executionStats"
});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'verosity' argument(s).", explain.errmsg);

explain = db.runCommand({
  explain: { find: collName },
  verbosity: "executionStats",
  foo: 1
});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'foo' argument(s).", explain.errmsg);

explain = db.runCommand({
  explain: { find: collName },
  foo: 1,
  verbosity: "executionStats",
  bar: 1
});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'foo', 'bar' argument(s).", explain.errmsg);

explain = db.runCommand({
  explain: { find: collName },
  explain: {},
  bar: 1
});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'bar' argument(s).", explain.errmsg);
