/**
 * Tests that applyOps can include operations on the system.views namespace.
 * @tags: [
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   assumes_superuser_permissions,
 *   requires_non_retryable_commands,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   # Tenant migrations don't support applyOps.
 *   tenant_migration_incompatible,
 * ]
 */

(function() {
"use strict";

const backingColl = db.apply_ops_system_dot_views_backing;
backingColl.drop();
const view = db.apply_ops_view;
view.drop();

// Make sure system.views exists before we try to insert to it with applyOps.
db["unused_apply_ops_view"].drop();
assert.commandWorked(
    db.runCommand({create: "unused_apply_ops_view", viewOn: backingColl.getName(), pipeline: []}));

assert.commandWorked(backingColl.insert([{a: 0}, {a: 1}]));
const ops = [{
    "ts": new Timestamp(1585942982, 2),
    "t": new NumberLong("1"),
    "v": new NumberInt("2"),
    "op": "i",
    "ns": db["system.views"].getFullName(),
    "wall": new Date(1585942982442),
    "o": {
        "_id": view.getFullName(),
        "viewOn": backingColl.getName(),
        "pipeline": [{"$match": {"a": 0}}]
    }
}];

assert.commandWorked(db.adminCommand({applyOps: ops}));
assert.eq(view.find({}, {_id: 0, a: 1}).toArray(), [{a: 0}]);
}());
