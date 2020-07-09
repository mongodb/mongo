/**
 * Tests the creation of view with a duplicate name to a collection.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   # applyOps is not retryable.
 *   requires_non_retryable_writes,
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