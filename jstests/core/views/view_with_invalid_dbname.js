// When loading the view catalog, the server should not crash because it encountered a view with an
// invalid name. This test is specifically for the case of a view with a dbname that contains an
// embedded null character (SERVER-36859).
(function() {
"use strict";

// Create a view whose dbname has an invalid embedded NULL character. That's not possible with
// the 'create' command, but it is possible by manually inserting into the 'system.views'
// collection.
const viewName = "dbNameWithEmbedded\0Character.collectionName";
const collName = "viewOnForViewWithInvalidDBNameTest";
const viewDef = {
    _id: viewName,
    viewOn: collName,
    pipeline: []
};

try {
    assert.commandWorked(db.system.views.insert(viewDef));
} finally {
    // Don't let the bogus view stick around, or else it will cause an error in validation.
    var result = db.system.views.deleteOne({_id: viewName});
}
// If this test otherwise succeeded, assert cleaning up succeeded.
// Skip this assertion if the test otherwise failed, to avoid masking the original error.
assert.eq(1, result.deletedCount);
}());
