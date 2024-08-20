/**
 * Tests that updates with "_$internalApplyOplogUpdate" are correctly applied on a time-series
 * buckets collection.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   requires_fcv_71,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

(function() {
"use strict";

// Skipping the compression check since we are testing specifically on uncompressed format.
TestData.skipEnforceTimeseriesBucketsAreAlwaysCompressedOnValidate = true;

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const collName = jsTestName();
const bucketsCollName = "system.buckets." + collName;
const coll = testDB.getCollection(collName);
coll.drop();
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: "t"}}));

const insertDocFull = {
    "_id": ObjectId("64d3c7004c83948224c45ddf"),
    "control": {
        "version": 1,
        "min": {"_id": ObjectId("64d24b52469e18af504e506e"), "t": ISODate("2023-08-09T17:04:00Z")},
        "max":
            {"_id": ObjectId("64d24b52469e18af504e506f"), "t": ISODate("2023-08-09T17:05:42.238Z")}
    },
    "data": {
        "_id": {
            "0": ObjectId("64d24b52469e18af504e506e"),
            "1": ObjectId("64d24b52469e18af504e506f"),
        },
        "t": {
            "0": ISODate("2023-08-09T17:04:02.238Z"),
            "1": ISODate("2023-08-09T17:05:42.238Z"),
        }
    }
};

const updateDoc1 = {
    "$v": 2,
    "diff": {
        "scontrol": {
            "u": {
                "max": {
                    "_id": ObjectId("64d24b52469e18af504e506e"),
                    "t": ISODate("2023-08-09T17:04:02.238Z")
                }
            }
        },
        "sdata": {
            "s_id": {
                "i": {
                    "0": ObjectId("64d24b52469e18af504e506e"),
                }
            },
            "st": {
                "i": {
                    "0": ISODate("2023-08-09T17:04:02.238Z"),
                }
            }
        }
    }
};

const updateDoc2 = {
    "$v": 2,
    "diff": {
        "scontrol": {
            "u": {
                "max": {
                    "_id": ObjectId("64d24b52469e18af504e506f"),
                    "t": ISODate("2023-08-09T17:05:42.238Z")
                }
            }
        },
        "sdata": {
            "s_id": {
                "i": {
                    "1": ObjectId("64d24b52469e18af504e506f"),
                }
            },
            "st": {
                "i": {
                    "1": ISODate("2023-08-09T17:05:42.238Z"),
                }
            }
        }
    }
};

// First inserts measurement 0 and 1.
testDB.getCollection(bucketsCollName).insertOne(insertDocFull);

function runTest(runInTxn) {
    // Inserts measurement 0 and 1 again through update oplog entries on the buckets collection.
    // Only two measurements are expected to exist.
    const updCmd = {
        update: bucketsCollName,
        updates: [
            {
                q: {"_id": ObjectId("64d3c7004c83948224c45ddf")},
                u: [{$_internalApplyOplogUpdate: {oplogUpdate: updateDoc1}}]
            },
            {
                q: {"_id": ObjectId("64d3c7004c83948224c45ddf")},
                u: [{$_internalApplyOplogUpdate: {oplogUpdate: updateDoc2}}]
            },
        ]
    };
    if (runInTxn) {
        const session = testDB.getMongo().startSession();
        const sessionDB = session.getDatabase("test");
        session.startTransaction();
        assert.commandWorked(sessionDB.runCommand(updCmd));
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
    } else {
        assert.commandWorked(testDB.runCommand(updCmd));
    }

    assert.eq(testDB.getCollection(collName).find().itcount(), 2);
}

runTest(/*runInTxn=*/ true);
runTest(/*runInTxn=*/ false);

rst.stopSet();
})();
