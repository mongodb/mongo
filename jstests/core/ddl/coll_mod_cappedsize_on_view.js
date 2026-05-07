/**
 * Regression test for SERVER-124967.
 * collMod with cappedSize or cappedMax against a view must return an error.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_non_retryable_commands,
 * ]
 */

const backingColl = db.collmod_cappedsize_view_backing;
const viewName = "collmod_cappedsize_view";

// Drop any leftover state from a previous run, then create a backing collection and a view on top
// of it.
backingColl.drop();
db[viewName].drop();
assert.commandWorked(db.createCollection(backingColl.getName()));
assert.commandWorked(db.createView(viewName, backingColl.getName(), []));

// collMod with cappedSize on a view must fail gracefully.
assert.commandFailedWithCode(
    db.runCommand({collMod: viewName, cappedSize: 1024}),
    ErrorCodes.InvalidOptions,
    "collMod with cappedSize on a view must fail with InvalidOptions",
);

// Same for cappedMax.
assert.commandFailedWithCode(
    db.runCommand({collMod: viewName, cappedMax: 100}),
    ErrorCodes.InvalidOptions,
    "collMod with cappedMax on a view must fail with InvalidOptions",
);
