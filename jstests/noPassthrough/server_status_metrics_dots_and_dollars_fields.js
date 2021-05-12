/**
 * Tests for serverStatus metrics.dotsAndDollarsFields stats.
 */
(function() {
"use strict";

const mongod = MongoRunner.runMongod();
const dbName = "dots_and_dollars_fields";
const db = mongod.getDB(dbName);
const collName = "server_status_metrics_dots_and_dollars_fields";
const coll = db[collName];
let serverStatusMetrics = db.serverStatus().metrics.dotsAndDollarsFields;

const isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                    .featureFlagDotsAndDollars.value;
if (!isDotsAndDollarsEnabled) {
    jsTestLog("Skipping test because the dots and dollars feature flag is disabled");
    MongoRunner.stopMongod(mongod);
    return;
}

//
// Test that "metrics.dotsAndDollarsField.inserts" is being updated correctly.
//

let insertCount = 0;
function runCommandAndCheckInsertCount(cmdToRun, add) {
    assert.commandWorked(cmdToRun());

    insertCount += add;
    const dotsAndDollarsMetrics = db.serverStatus().metrics.dotsAndDollarsFields;
    assert.eq(dotsAndDollarsMetrics.inserts, insertCount, dotsAndDollarsMetrics);
}

runCommandAndCheckInsertCount(() => coll.insert({"$a.b": 1}), 1);
runCommandAndCheckInsertCount(() => coll.insert({a: {a: 1}, "$.": 1, "$second": 1, "$3rd": 1}), 1);
// Only account for top-level $-prefixed fields.
runCommandAndCheckInsertCount(() => coll.insert({a: {"$a.b": 1}}), 0);
runCommandAndCheckInsertCount(() => coll.insert({obj: {obj: {arr: [1, 2, {"$a$": 1}]}}}), 0);
// This update command is actually an upsert which inserts an object.
runCommandAndCheckInsertCount(
    () => coll.update(
        {"field-not-found": 1},
        [{$replaceWith: {$setField: {field: {$literal: "$a.b"}, input: "$$ROOT", value: 1}}}],
        {upsert: true}),
    1);
runCommandAndCheckInsertCount(
    () => coll.insertMany([{"$dollar-field": 1}, {"$dollar-field": 1}, {"$$-field": 1}]), 3);
// An upsert findAndModify command because no match document was found.
runCommandAndCheckInsertCount(() => db.runCommand({
    findAndModify: collName,
    query: {"not-found": 1},
    update: [{$replaceWith: {$literal: {_id: 2, "$a$": 3}}}],
    upsert: true,
}),
                              1);

//
// Test that "metrics.dotsAndDollarsField.updates" is being updated correctly.
//

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 1}));

let updateCount = 0;

function runCommandAndCheckUpdateCount(cmdToRun, add) {
    assert.commandWorked(cmdToRun());

    updateCount += add;
    const dotsAndDollarsMetrics = db.serverStatus().metrics.dotsAndDollarsFields;
    assert.eq(dotsAndDollarsMetrics.updates, updateCount, dotsAndDollarsMetrics);
}

//
// Test pipeline-style updates.
//
runCommandAndCheckUpdateCount(() => db.runCommand({
    update: collName,
    updates: [{q: {}, u: [{$replaceWith: {$literal: {$db: 1}}}], upsert: true}]
}),
                              1);

runCommandAndCheckUpdateCount(() => db.runCommand({
    update: collName,
    updates: [
        {q: {}, u: [{$replaceWith: {$literal: {"$dollar-field": 1}}}], upsert: true},
        {
            q: {_id: 1},
            u: [{
                $replaceWith: {
                    $setField:
                        {field: {$literal: "$ab"}, input: {$literal: {"a.b": "b"}}, value: 12345}
                }
            }]
        }
    ]
}),
                              2);

// No-op because no match document found and 'upsert' is set to false, so do not tick.
runCommandAndCheckUpdateCount(() => db.runCommand({
    update: collName,
    updates: [{
        q: {"not-found": "null"},
        u: [{$replaceWith: {$literal: {"$dollar-field": 1}}}],
        upsert: false
    }]
}),
                              0);

//
// Test findAndModify command.
//
runCommandAndCheckUpdateCount(() => db.runCommand({
    findAndModify: collName,
    query: {_id: 1},
    update: {_id: 1, out: {$in: 1, "x": 2}},
    upsert: true,
}),
                              1);
runCommandAndCheckUpdateCount(() => db.runCommand({
    findAndModify: collName,
    query: {_id: 1},
    update: [{$replaceWith: {$literal: {_id: 1, "$dollar-field": 3}}}],
}),
                              1);

MongoRunner.stopMongod(mongod);
})();
