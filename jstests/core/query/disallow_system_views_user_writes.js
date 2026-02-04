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
// TODO (SERVER-117130): Remove the mongos pinning once the related issue is resolved.
// When a database is dropped, a stale router will report "database not found" error for
// deletes (instead of "ok") when pauseMigrationsDuringMultiUpdates is enabled.
if (TestData.pauseMigrationsDuringMultiUpdates) {
    TestData.pinToSingleMongos = true;
}

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
