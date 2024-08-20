/*
 *  Test that aggregation's $out stage serializes behind a drop database and fails.
 *
 *  @tags: [
 *      requires_fcv_71,
 *      does_not_support_stepdowns, # DropDatabaseCoordinator drops the input collection on step-up
 *  ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: {rs0: {nodes: 1}}});
const dbName = "test";
const db = st.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Setup and populate input collection.
const inputCollName = "input_coll";
const inputColl = db[inputCollName];
assert.commandWorked(inputColl.insert({_id: 0}));

const outputCollName = "output_coll";

let failpoint =
    configureFailPoint(st.rs0.getPrimary(), 'blockBeforeInternalRenameAndBeforeTakingDDLLocks');

function aggOut(inputCollName, outputCollName) {
    // Make sure the aggregation fails because the database has been dropped
    assert.commandFailedWithCode(
        db.runCommand({aggregate: inputCollName, pipeline: [{$out: outputCollName}], cursor: {}}),
        ErrorCodes.NamespaceNotFound);
}

// Start an aggregation with $out stage, will block before renaming the tmp collection
const awaitShell =
    startParallelShell(funWithArgs(aggOut, inputCollName, outputCollName), st.s.port);

// Wait for the aggregation to arrive right before the rename
failpoint.wait();

// Drop the database and unblock the aggregation
assert.commandWorked(db.dropDatabase());
failpoint.off();

awaitShell();

st.stop();
