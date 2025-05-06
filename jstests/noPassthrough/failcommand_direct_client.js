/**
 * Tests that the failCommand failpoint triggers on direct client commands by default.
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rs = new ReplSetTest({nodes: 2});
rs.startSet();
rs.initiate();

const db = rs.getPrimary().getDB("test");
db.dropDatabase();
const coll = db.coll;

{
    const failpoint = configureFailPoint(
        rs.getPrimary(), "failCommand", {errorCode: ErrorCodes.BadValue, failCommands: ["find"]});

    assert.commandWorked(coll.insertOne({index: 1}));

    // Writing in a transaction uses direct client to issue a find command.
    {
        const session = db.getMongo().startSession();
        session.startTransaction();
        const error = assert.throws(() => session.getDatabase("test").coll.insertOne({index: 2}));
        assert.commandFailedWithCode(error, ErrorCodes.BadValue);
        session.abortTransaction();
    }

    failpoint.off();
}
{
    const failpoint = configureFailPoint(
        rs.getPrimary(),
        "failCommand",
        {errorCode: ErrorCodes.BadValue, failCommands: ["find"], failDirectClientCommands: false});

    assert.commandWorked(coll.insertOne({index: 1}));

    {
        const session = db.getMongo().startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("test").coll.insertOne({index: 2}));
        session.commitTransaction();
    }

    failpoint.off();
}

rs.stopSet();
