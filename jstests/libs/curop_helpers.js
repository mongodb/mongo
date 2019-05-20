// Wait until the current operation matches the filter.
function waitForCurOpByFilter(db, filter) {
    assert.soon(
        () => {
            return db.getSiblingDB("admin")
                       .aggregate([{$currentOp: {}}, {$match: filter}])
                       .itcount() == 1;
        },
        () => {
            return "Failed to find a matching op for filter \"" + tojson(filter) +
                "\" in currentOp output: " + tojson(db.aggregate([{$currentOp: {}}]).toArray());
        });
}

// Wait until the current operation reaches the fail point "failPoint" for the given
// namespace "nss".
function waitForCurOpByFailPoint(db, nss, failPoint) {
    let filter = {$and: [{"ns": nss}, {"msg": failPoint}]};
    waitForCurOpByFilter(db, filter);
}
