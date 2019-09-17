/**
 * Tests that a backtrace will appear in the $currentOp output if the backtrace option is
 * set to true and there is a latch timeout.
 *
 * @tags: [assumes_read_concern_unchanged, assumes_read_preference_unchanged]
 */
(function() {
"use strict";

const adminDB = db.getSiblingDB("admin");

const getCurrentOp = function() {
    jsTestLog("Getting $currentOp");
    let result =
        adminDB
            .aggregate(
                [
                    {
                        $currentOp:
                            {allUsers: true, idleConnections: true, localOps: true, backtrace: true}
                    },
                ],
                {readConcern: {level: "local"}})
            .toArray();
    assert(result);
    return result;
};

const blockedOpClientName = "DiagnosticCaptureTest";

const getClientName = function() {
    let myUri = adminDB.runCommand({whatsmyuri: 1}).you;
    return adminDB.aggregate([{$currentOp: {localOps: true}}, {$match: {client: myUri}}])
        .toArray()[0]
        .desc;
};

let clientName = getClientName();

try {
    assert.commandWorked(db.adminCommand({
        "configureFailPoint": 'currentOpSpawnsThreadWaitingForLatch',
        "mode": 'alwaysOn',
        "data": {
            'clientName': clientName,
        },
    }));

    let result = null;
    getCurrentOp().forEach(function(op) {
        jsTestLog(tojson(op));
        if (op["desc"] == blockedOpClientName) {
            result = op;
        }
    });
    assert(result);
    assert(result.hasOwnProperty("waitingForLatch"));
    assert(result["waitingForLatch"].hasOwnProperty("timestamp"));
    assert(result["waitingForLatch"].hasOwnProperty("captureName"));
    assert(result["waitingForLatch"].hasOwnProperty("backtrace"));
    result["waitingForLatch"]["backtrace"].forEach(function(frame) {
        assert(frame.hasOwnProperty("addr"));
        assert(typeof frame["addr"] === "string");
        assert(frame.hasOwnProperty("path"));
        assert(typeof frame["path"] === "string");
    });
} finally {
    assert.commandWorked(db.adminCommand(
        {"configureFailPoint": 'currentOpSpawnsThreadWaitingForLatch', "mode": 'off'}));

    getCurrentOp().forEach(function(op) {
        jsTestLog(tojson(op));
        if (op["desc"] == blockedOpClientName) {
            assert(!op.hasOwnProperty("waitingForLatch"));
        }
    });
}
})();
