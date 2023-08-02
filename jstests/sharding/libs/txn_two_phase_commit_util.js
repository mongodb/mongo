/**
 * Utilities for checking transaction coordinator behaviour in two phase commits.
 */

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
