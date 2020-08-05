// Wait until the current operation matches the filter. Returns the resulting array of operations.
function waitForCurOpByFilter(db, filter, options = {}) {
    const adminDB = db.getSiblingDB("admin");
    let results = [];
    assert.soon(
        () => {
            results = adminDB.aggregate([{$currentOp: options}, {$match: filter}]).toArray();
            return results.length > 0;
        },
        () => {
            return "Failed to find a matching op for filter: " + tojson(filter) +
                "in currentOp output: " + tojson(results);
        });
    return results;
}

// Wait until the current operation reaches the fail point "failPoint" for the given namespace
// "nss". Accepts an optional filter to apply alongside the failpoint 'msg'. Returns the resulting
// array of operations.
function waitForCurOpByFailPoint(db, nss, failPoint, filter = {}, options = {}) {
    const adjustedFilter = {$and: [{ns: nss}, filter, {msg: failPoint}]};
    return waitForCurOpByFilter(db, adjustedFilter, options);
}

// Wait until the current operation reaches the fail point "failPoint" with no namespace. Returns
// the resulting array of operations.
function waitForCurOpByFailPointNoNS(db, failPoint, filter = {}, options = {}) {
    const adjustedFilter = {$and: [filter, {msg: failPoint}]};
    return waitForCurOpByFilter(db, adjustedFilter, options);
}
