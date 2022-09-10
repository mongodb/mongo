(function() {
"use strict";

// Validate that we can internally generate a special query which along with a document returns its
// RecordID.

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_findone_rid;
coll.drop();

assert.commandWorked(coll.insertMany([
    {a: 1},
    {a: 2},
    {a: 3},
]));

try {
    // The "createView" statement issues a special query which returns a RecordId to check a view
    // for existence and modify its definition if it exists. We're aiming to validate that this
    // internal query can be executed and the view can be successfully created.

    assert.commandWorked(db.createView("cqf_findone_rid_view", "cqf_findone_rid", []));
    const res = db.cqf_findone_rid_view.explain("executionStats").find().finish();
    assert.eq(3, res.executionStats.nReturned);
} finally {
    db.cqf_findone_rid_view.drop();
}
}());
