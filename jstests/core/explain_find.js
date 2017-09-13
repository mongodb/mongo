// Tests for explaining find through the explain command.

var collName = "jstests_explain_find";
var t = db[collName];
t.drop();

t.ensureIndex({a: 1});

for (var i = 0; i < 10; i++) {
    t.insert({_id: i, a: i});
}

var explain =
    db.runCommand({explain: {find: collName, filter: {a: {$lte: 2}}}, verbosity: "executionStats"});
printjson(explain);
assert.commandWorked(explain);
assert.eq(3, explain.executionStats.nReturned);

explain = db.runCommand(
    {explain: {find: collName, min: {a: 4}, max: {a: 6}}, verbosity: "executionStats"});
printjson(explain);
assert.commandWorked(explain);
assert.eq(2, explain.executionStats.nReturned);

// Compatibility test for the $explain OP_QUERY flag. This can only run if find command is disabled.
if (!db.getMongo().useReadCommands()) {
    var explain = t.find({$query: {a: 4}, $explain: true}).limit(-1).next();
    assert("queryPlanner" in explain);
    assert("executionStats" in explain);
    assert.eq(1, explain.executionStats.nReturned);
    assert("allPlansExecution" in explain.executionStats);
}
