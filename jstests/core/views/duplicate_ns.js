/**
 * Tests the creation of view with a duplicate name to a collection.
 *
 * The test runs commands that are not allowed with security token: applyOps.
 * @tags: [
 *   not_allowed_with_security_token,
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   # applyOps is not retryable.
 *   requires_non_retryable_writes,
 *   # Having duplicate namespaces is not supported and will cause tenant migrations to fail.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";

const dbName = "views_duplicate_ns";
const viewsDb = db.getSiblingDB(dbName);
const collName = "myns";
const viewId = dbName + "." + collName;

assert.commandWorked(viewsDb.dropDatabase());
assert.commandWorked(viewsDb.runCommand({create: collName}));
assert.commandWorked(viewsDb.createCollection("system.views"));

// There is a loophole to create a view with a duplicate namespace, by directly adding it via
// applyOps. Trying to write to system.views or to use the proper DDL commands will prevent this
// from happening. We may close this loophole in the future, but for now we should ensure that it
// works and does not crash the server.
assert.commandWorked(viewsDb.adminCommand({
    applyOps: [{
        op: "i",
        ns: dbName + ".system.views",
        o: {
            _id: viewId,
            viewOn: "coll",
            pipeline: [],
        }
    }]
}));
assert.eq(2,
          viewsDb.getCollectionInfos()
              .filter(coll => {
                  return coll.name === collName;
              })
              .length);
}());
