// Tests for explaining find through the explain command.

var collName = "jstests_explain_find";
var t = db[collName];
t.drop();

t.ensureIndex({a: 1});

for (var i = 0; i < 10; i++) {
    t.insert({_id: i, a: i});
}

var explain = db.runCommand({
    explain: {
        find: collName,
        filter: {a: {$lte: 2}}
    },
    verbosity: "executionStats"
});
printjson(explain);
assert.commandWorked(explain);
assert.eq(3, explain.executionStats.nReturned);

explain = db.runCommand({
    explain: {
        find: collName,
        options: {
            min: {a: 4},
            max: {a: 6}
        }
    },
    verbosity: "executionStats"
});
printjson(explain);
assert.commandWorked(explain);
assert.eq(2, explain.executionStats.nReturned);
