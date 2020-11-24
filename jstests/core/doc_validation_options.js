// @tags: [
//      # Cannot implicitly shard accessed collections because of collection existing when none
//      # expected.
//      assumes_no_implicit_collection_creation_after_drop,
//      requires_non_retryable_commands,
//      requires_fcv_47,
// ]

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // for documentEq
load("jstests/libs/fixture_helpers.js");      // for getPrimaryForNodeHostingDatabase

function assertFailsValidation(res) {
    assert.writeError(res);
    assert.eq(res.getWriteError().code, ErrorCodes.DocumentValidationFailure);
}

const t = db[jsTestName()];
t.drop();

const validatorExpression = {
    a: 1
};
assert.commandWorked(db.createCollection(t.getName(), {validator: validatorExpression}));

assertFailsValidation(t.insert({a: 2}));
t.insert({_id: 1, a: 1});
assert.eq(1, t.count());

// test default to strict
assertFailsValidation(t.update({}, {$set: {a: 2}}));
assert.eq(1, t.find({a: 1}).itcount());

// check we can do a bad update in warn mode
assert.commandWorked(t.runCommand("collMod", {validationAction: "warn"}));
t.update({}, {$set: {a: 2}});
assert.eq(1, t.find({a: 2}).itcount());

// check log for message
// use getPrimaryForNodeHostingDatabase to return a connection to the db or the primary node
// if the db is sharded, so we can specifically search logs of the node which owns the
// document that generated the warning.
const conn = FixtureHelpers.getPrimaryForNodeHostingDatabase(db);
const logId = 20294;
const errInfo = {
    failingDocumentId: 1,
    details:
        {operatorName: "$eq", specifiedAs: {a: 1}, reason: "comparison failed", consideredValue: 2}
};
checkLog.containsJson(conn, logId, {
    "errInfo": function(obj) {
        return documentEq(obj, errInfo);
    }
});

// make sure persisted
const info = db.getCollectionInfos({name: t.getName()})[0];
assert.eq("warn", info.options.validationAction, tojson(info));

// Verify that validator expressions which throw accept writes when in 'warn' mode.
assert.commandWorked(t.runCommand("collMod", {validator: {$expr: {$divide: [10, 0]}}}));
const insertResult = t.insert({foo: '1'});
assert.commandWorked(insertResult, tojson(insertResult));
assert.commandWorked(t.remove({foo: '1'}));

// Reset the collection validator to the original.
assert.commandWorked(t.runCommand("collMod", {validator: validatorExpression}));

// check we can go back to enforce strict
assert.commandWorked(
    t.runCommand("collMod", {validationAction: "error", validationLevel: "strict"}));
assertFailsValidation(t.update({}, {$set: {a: 3}}));
assert.eq(1, t.find({a: 2}).itcount());

// check bad -> bad is ok
assert.commandWorked(t.runCommand("collMod", {validationLevel: "moderate"}));
t.update({}, {$set: {a: 3}});
assert.eq(1, t.find({a: 3}).itcount());

// test create
t.drop();
assert.commandWorked(
    db.createCollection(t.getName(), {validator: {a: 1}, validationAction: "warn"}));

t.insert({a: 2});
t.insert({a: 1});
assert.eq(2, t.count());
})();
