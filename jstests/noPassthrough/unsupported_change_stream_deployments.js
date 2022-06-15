// Tests that the $changeStream stage returns an error when run against a standalone mongod.
// @tags: [requires_sharding, uses_change_streams, requires_majority_read_concern]

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

function assertChangeStreamNotSupportedOnConnection(conn) {
    const notReplicaSetErrorCode = 40573;
    assertErrorCode(conn.getDB("test").non_existent, [{$changeStream: {}}], notReplicaSetErrorCode);
    assertErrorCode(conn.getDB("test").non_existent,
                    [{$changeStream: {fullDocument: "updateLookup"}}],
                    notReplicaSetErrorCode);
}

const conn = MongoRunner.runMongod({enableMajorityReadConcern: ""});
assert.neq(null, conn, "mongod was unable to start up");
// $changeStream cannot run on a non-existent database.
assert.commandWorked(conn.getDB("test").ensure_db_exists.insert({}));
assertChangeStreamNotSupportedOnConnection(conn);
assert.eq(0, MongoRunner.stopMongod(conn));
}());
