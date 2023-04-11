/**
 * Tests that a wildcard index is correctly maintained when document is updated.
 *
 * @tags: [
 *   requires_fcv_63,
 *   does_not_support_stepdowns,
 *   uses_full_validation,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");  // For "FeatureFlagUtil"

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes =
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompoundWildcardIndexes");

assert.commandWorked(coll.createIndex({"a.b.c.d.$**": 1}));
if (allowCompoundWildcardIndexes) {
    assert.commandWorked(coll.createIndex({"a.b.c.d.$**": 1, "other": 1}));
    assert.commandWorked(coll.createIndex({"pre": 1, "a.b.c.d.$**": 1, "other": 1}));
    assert.commandWorked(coll.createIndex({"pre": 1, "a.b.c.d.$**": -1}));
}

const validate = function() {
    const validateRes = coll.validate({full: true});
    assert.eq(validateRes.valid, true, tojson(validateRes));
};

assert.commandWorked(coll.insert({_id: 0, e: 1}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.c.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"a.b.c.d.e": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"pre": 1}}));
validate();

assert.commandWorked(coll.update({_id: 0}, {$set: {"other": 1}}));
validate();
})();
