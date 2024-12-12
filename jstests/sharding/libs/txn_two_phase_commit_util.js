/**
 * Utilities for checking transaction coordinator behaviour in two phase commits.
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

export const checkDecisionIs = function(coordinatorConn, lsid, txnNumber, expectedDecision) {
    let coordDoc = coordinatorConn.getDB("config")
                       .getCollection("transaction_coordinators")
                       .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
    assert.neq(null, coordDoc);
    assert.eq(expectedDecision, coordDoc.decision.decision);
    if (expectedDecision === "commit") {
        assert.neq(null, coordDoc.decision.commitTimestamp);
    } else {
        assert.eq(null, coordDoc.decision.commitTimestamp);
    }
};

export const checkDocumentDeleted = function(coordinatorConn, lsid, txnNumber) {
    let coordDoc = coordinatorConn.getDB("config")
                       .getCollection("transaction_coordinators")
                       .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
    return null === coordDoc;
};

export const runCommitThroughMongosInParallelThread = function(
    lsidUUID, txnNumber, mongosHost, errorCode = ErrorCodes.OK) {
    return new Thread(runCommitThroughMongos,
                      extractUUIDFromObject(lsidUUID.id),
                      txnNumber,
                      mongosHost,
                      errorCode);
};

// lsidUUID is the UUID value in string format.
export const runCommitThroughMongos = function(lsidUUID, txnNumber, mongosHost, expectedCode) {
    const conn = new Mongo(mongosHost);
    const command = {
        commitTransaction: 1,
        lsid: {id: UUID(lsidUUID)},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    };

    if (expectedCode === ErrorCodes.MaxTimeMSExpired) {
        command.maxTimeMS = 1000 * 10;
    }

    const result = conn.adminCommand(command);
    if (expectedCode === ErrorCodes.OK) {
        assert.commandWorked(result);
    } else {
        assert.commandFailedWithCode(result, expectedCode);
    }
};
