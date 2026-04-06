/**
 * Tests that applyOps with operations having the 'rid' field works since the field is stripped.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rs = new ReplSetTest({nodes: 2});
rs.startSet();
rs.initiate();

const insertOp = {
    "op": "i",
    "ns": "applyOps_rid_ignored.test",
    "o": {
        "_id": ObjectId("698f6c6297f4ac88d31be65a"),
        "a": 1,
    },
    "o2": {
        "_id": ObjectId("698f6c6297f4ac88d31be65a"),
    },
    "rid": NumberLong(1),
};

const updateOp = {
    "op": "u",
    "ns": "applyOps_rid_ignored.test",
    "o": {
        "$v": 2,
        "diff": {
            "i": {
                "b": 1,
            },
        },
    },
    "o2": {
        "_id": ObjectId("698f6c6297f4ac88d31be65a"),
    },
    "rid": NumberLong(2),
};

const deleteOp = {
    "op": "d",
    "ns": "applyOps_rid_ignored.test",
    "o": {
        "_id": ObjectId("698f6c6297f4ac88d31be65a"),
    },
    "rid": NumberLong(3),
};

const db = rs.getPrimary().getDB(jsTestName());

assert.commandWorked(db.createCollection("test"));

// The 'insert' op will succeed with 'rid' field ignored as it is stripped from applyOps commands.
assert.commandWorked(db.adminCommand({applyOps: [insertOp]}));
assert.eq(db.test.find().itcount(), 1);

// The 'update' op will succeed with 'rid' field ignored as it is stripped from applyOps commands.
assert.commandWorked(db.adminCommand({applyOps: [updateOp]}));
assert.eq(db.test.find({b: 1}).itcount(), 1);

// The 'delete' op will succeed with 'rid' field ignored when replicated record ids are not enabled.
assert.commandWorked(db.adminCommand({applyOps: [deleteOp]}));
assert.eq(db.test.find().itcount(), 0);

rs.stopSet();
