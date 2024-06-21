/**
 * Tests for explaining find through the explain command.
 * @tags: [
 *   assumes_read_concern_local,
 *   requires_fcv_81,
 * ]
 */

var collName = "jstests_explain_find";
var t = db[collName];
t.drop();

t.createIndex({a: 1});

for (var i = 0; i < 10; i++) {
    t.insert({_id: i, a: i});
}

var explain =
    db.runCommand({explain: {find: collName, filter: {a: {$lte: 2}}}, verbosity: "executionStats"});
assert.commandWorked(explain);
assert.eq(3, explain.executionStats.nReturned);

explain = db.runCommand({
    explain: {find: collName, min: {a: 4}, max: {a: 6}, hint: {a: 1}},
    verbosity: "executionStats",
});
assert.commandWorked(explain);
assert.eq(2, explain.executionStats.nReturned);

// Invalid verbosity string.
let error = assert.throws(function() {
    t.explain("foobar").find().finish();
});
assert.commandFailedWithCode(error, ErrorCodes.BadValue);

error = assert.throws(function() {
    t.find().explain("foobar");
});
assert.commandFailedWithCode(error, ErrorCodes.BadValue);

const serverVer = db.version().split('.');
if ((serverVer[0] == 7 && serverVer[1] >= 3) || serverVer[0] > 7) {
    // Starting in 7.3 running explain() against a non-existent database should result in
    // an EOF plan against mongos or mongod.
    let dbdne = db.getSiblingDB("does_not_exist_hopefully");
    var explain = dbdne.runCommand(
        {explain: {find: collName, filter: {a: {$lte: 2}}}, verbosity: "executionStats"});
    assert.commandWorked(explain);

    // Explain output differs slightly under SBE versus classic engine
    if (explain.queryPlanner.winningPlan.queryPlan) {
        assert.eq("EOF", explain.queryPlanner.winningPlan.queryPlan.stage);
        assert.eq("nonExistentNamespace", explain.queryPlanner.winningPlan.queryPlan.type, explain);
    } else {
        assert.eq("EOF", explain.queryPlanner.winningPlan.stage);
        assert.eq("nonExistentNamespace", explain.queryPlanner.winningPlan.type);
    }

    assert.eq("does_not_exist_hopefully.jstests_explain_find", explain.queryPlanner.namespace);
    assert.eq({"a": {"$lte": 2}}, explain.queryPlanner.parsedQuery);
}
