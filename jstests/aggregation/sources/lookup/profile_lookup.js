// @tags: [does_not_support_stepdowns, requires_profiling]
//
// Tests that profiled $lookups contain the correct namespace and that Top is updated accordingly.

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const localColl = db.local;
const foreignColl = db.foreign;
localColl.drop();
foreignColl.drop();

assert.commandWorked(localColl.insert([{a: 1}, {b: 1}, {a: 2}]));
assert.commandWorked(foreignColl.insert({a: 1}));

db.system.profile.drop();
db.setProfilingLevel(2);

let oldTop = db.adminCommand("top");

localColl.aggregate(
    [{$lookup: {from: foreignColl.getName(), as: "res", localField: "a", foreignField: "a"}}]);

db.setProfilingLevel(0);

// Confirm that namespace is the local rather than foreign collection.
let profileDoc = db.system.profile.findOne();
assert.eq("test.local", profileDoc.ns);

// Confirm that the local collection had one command added to Top.
let newTop = db.adminCommand("top");
assert.eq(1,
          newTop.totals[localColl.getFullName()].commands.count -
              oldTop.totals[localColl.getFullName()].commands.count);

const actualCount = newTop.totals[foreignColl.getFullName()].commands.count -
    oldTop.totals[foreignColl.getFullName()].commands.count;

// TODO SERVER-64722 Reenable this test after SERVER-64722 is fixed.
if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    // Confirm that for each document in local, the foreign collection had one entry added to Top.
    assert.eq(3, actualCount);
}
}());
