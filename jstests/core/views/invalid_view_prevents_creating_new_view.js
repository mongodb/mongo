/**
 * Test that, when an existing view in system.views is invalid because of a $out in the
 * pipeline, the database errors on creation of a new view.
 *
 * The test runs commands that are not allowed with security token: applyOps.
 * @tags: [
 *   not_allowed_with_security_token,
 *   # applyOps is not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   # applyOps is not retryable.
 *   requires_non_retryable_commands,
 *   # Tenant migrations don't support applyOps.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";

// For arrayEq.
load("jstests/aggregation/extras/utils.js");

const viewsDBName = jsTestName();

let viewsDB = db.getSiblingDB(viewsDBName);
assert.commandWorked(viewsDB.dropDatabase());

// Create an initial collection and view so the DB and system.views collection exist.
assert.commandWorked(viewsDB.runCommand({create: "collection"}));
assert.commandWorked(viewsDB.runCommand({create: "testView", viewOn: "collection"}));

assert.commandWorked(viewsDB.adminCommand({
    applyOps: [{
        op: "i",
        ns: viewsDBName + ".system.views",
        o: {
            _id: viewsDBName + ".invalidView",
            viewOn: "collection",
            pipeline: [{$project: {_id: false}}, {$out: "notExistingCollection"}],
        }
    }]
}));
assert.commandFailedWithCode(
    viewsDB.runCommand({create: "viewWithBadViewCatalog", viewOn: "collection", pipeline: []}),
    ErrorCodes.OptionNotSupportedOnView);
assert.commandWorked(viewsDB.adminCommand({
    applyOps: [{op: "d", ns: viewsDBName + ".system.views", o: {_id: viewsDBName + ".invalidView"}}]
}));
}());
