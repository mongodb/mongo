// Wait until the current operation matches the filter. Returns the resulting array of operations.
export function waitForCurOpByFilter(db, filter, options = {}) {
    const adminDB = db.getSiblingDB("admin");
    let results = [];
    assert.soon(
        () => {
            results = adminDB.aggregate([{$currentOp: options}, {$match: filter}]).toArray();
            return results.length > 0;
        },
        () => {
            let allResults = adminDB.aggregate([{$currentOp: options}]).toArray();
            return "Failed to find a matching op for filter: " + tojson(filter) +
                "in currentOp output: " + tojson(allResults);
        });
    return results;
}

// Wait until the current operation reaches the fail point "failPoint" for the given namespace
// "nss". Accepts an optional filter to apply alongside the "failpointMsg". Returns the resulting
// array of operations.
export function waitForCurOpByFailPoint(db, nss, failPoint, filter = {}, options = {}) {
    const adjustedFilter = {
        $and: [{ns: nss}, filter, {$or: [{failpointMsg: failPoint}, {msg: failPoint}]}]
    };
    return waitForCurOpByFilter(db, adjustedFilter, options);
}

// Wait until the current operation reaches the fail point "failPoint" with no namespace. Returns
// the resulting array of operations.
export function waitForCurOpByFailPointNoNS(db, failPoint, filter = {}, options = {}) {
    const adjustedFilter = {$and: [filter, {$or: [{failpointMsg: failPoint}, {msg: failPoint}]}]};
    return waitForCurOpByFilter(db, adjustedFilter, options);
}

/**
 * Wait using asset.soon for a curop to be found given a command comment and filters.
 *
 * @param {object} db the Database to find the current ops in
 * @param {string} comment The content of the command comment that is expected to be in the current
 *     op
 * @param {object} filter Additional filters to indentify the wanted current op to have
 * @param {object} options CurrentOp query options
 *
 * @returns {array} list of found current ops. Always length of 1 or bigger.
 */
export function waitForCurOpByComment(db, comment, filter = {}, options = {}) {
    const adjustedFilter = {$and: [filter, {"command.comment": comment}]};
    return waitForCurOpByFilter(db, adjustedFilter, options);
}
