// @tags: [
//      # Cannot implicitly shard accessed collections because of collection existing when none
//      # expected.
//      assumes_no_implicit_collection_creation_after_drop,
//      requires_non_retryable_commands,
//      requires_getmore,
//      no_selinux,
// ]

import {assertFailsValidation} from "jstests/libs/doc_validation_utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const t = db[jsTestName()];
t.drop();

const validatorExpression = {
    a: 1
};
assert.commandWorked(db.createCollection(t.getName(), {validator: validatorExpression}));

assertFailsValidation(t.insert({a: 2}));
assert.commandWorked(t.insert({_id: 1, a: 1}));
assert.eq(1, t.count());

// test default to strict
assertFailsValidation(t.update({}, {$set: {a: 2}}));
assert.eq(1, t.find({a: 1}).itcount());

if (FeatureFlagUtil.isPresentAndEnabled(db, "ErrorAndLogValidationAction")) {
    const res = assert.commandWorkedOrFailedWithCode(
        t.runCommand("collMod", {validationAction: "errorAndLog"}), ErrorCodes.InvalidOptions);
    if (res.ok) {
        assertFailsValidation(t.update({}, {$set: {a: 2}}));
        // make sure persisted
        const info = db.getCollectionInfos({name: t.getName()})[0];
        assert.eq("errorAndLog", info.options.validationAction, tojson(info));
    }
}

// check we can do a bad update in warn mode
assert.commandWorked(t.runCommand("collMod", {validationAction: "warn"}));
assert.commandWorked(t.update({}, {$set: {a: 2}}));
assert.eq(1, t.find({a: 2}).itcount());
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
assert.commandWorked(t.update({}, {$set: {a: 3}}));
assert.eq(1, t.find({a: 3}).itcount());

// test create
t.drop();
assert.commandWorked(
    db.createCollection(t.getName(), {validator: {a: 1}, validationAction: "warn"}));

assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: 1}));
assert.eq(2, t.count());
