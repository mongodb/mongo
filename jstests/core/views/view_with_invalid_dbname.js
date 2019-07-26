// When loading the view catalog, the server should not crash because it encountered a view with an
// invalid name. This test is specifically for the case of a view with a dbname that contains an
// embedded null character (SERVER-36859).
//
// The 'restartCatalog' command is not available on embedded.
// @tags: [ incompatible_with_embedded, SERVER-38379 ]

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
assert.commandWorked(db.system.views.insert(viewDef));

// If the reinitialization of the durable view catalog tries to create a NamespaceString using
// the 'viewName' field, it will throw an exception in a place that is not exception safe,
// resulting in an invariant failure. This previously occurred because validation was only
// checking the collection part of the namespace, not the dbname part. With correct validation
// in place, reinitialization succeeds despite the invalid name.
assert.commandWorked(db.adminCommand({restartCatalog: 1}));

// Don't let the bogus view stick around, or else it will cause an error in validation.
const res = db.system.views.deleteOne({_id: viewName});
assert.eq(1, res.deletedCount);
}());
