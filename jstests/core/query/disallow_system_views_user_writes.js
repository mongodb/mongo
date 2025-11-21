/**
 * Tests that users are disallowed from writing to the system.views collecion.
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   # Time series collections cannot be used as a source for `viewOn` or
 *   # shave view-like limitations in this context.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */
const viewNs = "test.view";
const viewDefinition = {
    _id: viewNs,
    viewOn: "coll",
    pipeline: [],
};
const invalidField = {
    invalidField: true,
};

assert.commandFailedWithCode(db["system.views"].insert(viewDefinition), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db["system.views"].update({}, invalidField), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(
    db.runCommand({findAndModify: "system.views", query: {}, update: invalidField}),
    ErrorCodes.InvalidNamespace,
);
assert.commandFailedWithCode(db["system.views"].remove({}), ErrorCodes.InvalidNamespace);
