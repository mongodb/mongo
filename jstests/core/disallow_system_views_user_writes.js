/**
 * Tests that users are disallowed from writing to the system.views collecion.
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   requires_fcv_47,
 * ]
 */
(function() {
"use strict";

const viewNs = "test.view";
const viewDefinition = {
    _id: viewNs,
    viewOn: "coll",
    pipeline: []
};
const invalidField = {
    invalidField: true
};

assert.commandFailedWithCode(db.system.views.insert(viewDefinition), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.system.views.update({}, invalidField), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(
    db.runCommand({findAndModify: "system.views", query: {}, update: invalidField}),
    ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.system.views.remove({}), ErrorCodes.InvalidNamespace);
})();
