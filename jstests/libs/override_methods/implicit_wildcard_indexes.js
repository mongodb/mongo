/**
 * This file defines command overrides in order to implcitly create additional wildcard indexes for
 * tests running in this suite.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

const prefix = "HIDDEN_WILDCARD_";
const isHiddenWildcardIndex = {
    $regexMatch: {input: "$name", regex: prefix}
};

/**
 * Error codes we ignore when trying to create implicit indexes because they depend on the test
 * structure itself.
 */
const acceptableErrorCodes = [
    // Wildcard prefix overlap.
    7246204,
    7246200,
    // Non-wildcard field in compound index can't be multikey.
    7246301,
    // Index already exists with a different name.
    85,
    // Too many compound keys.
    13103,
    // Trying to create a wildcard index on a timeseries collection results in a duplicate wildcard
    // key error or invalid wildcardProjection.
    7246201,
    9,
];

/**
 * Substitute the field 'subFieldname' with a wildcard field & generate appropriate  exclusion
 * projections for the other fields in the CWI as needed.
 */
function transformToWildcard(key, subFieldname) {
    const subFieldElems = subFieldname.split(".");
    subFieldElems.pop();
    subFieldElems.push("");
    const wildcardPrefix = subFieldElems.join(".");

    let outKey = {};
    let exclusionProjection = {};
    let hasWildcard = false;
    for (const [field, dir] of Object.entries(key)) {
        if (field === subFieldname && !hasWildcard) {
            outKey[wildcardPrefix + "$**"] = dir;
            hasWildcard = true;
        } else {
            outKey[field] = dir;
            if (field.startsWith(wildcardPrefix)) {
                exclusionProjection[field] = 0;
            }
        }
    }
    return [outKey, exclusionProjection];
}

/**
 * If the keyPattern does not correspond to a wildcard index already, generate one
 * wildcard index per entry in the keyPattern. For example, the following indexes would
 * result in the following CWIs:
 * - {a.b.c: 1} ->
 *   1. [{a.b.$**: 1}, no exclusion]
 * - {a: -1, b: 1, c.d: 1} ->
 *   1. [{$**: -1, b: 1, c.d: 1}, {b: 0, c.d: 0}],
 *   2. [{a: -1, $**: 1, c.d: 1}, {a: 0, c.d: 0}],
 *   3. [{a: -1, b: 1, c.$**: 1}, no exclusion]
 */
function transformIntoWildcardIndexes(keyPattern) {
    let out = [];
    for (const [field, value] of Object.entries(keyPattern)) {
        if (field.endsWith("$**") || (typeof value === "string")) {
            // This is either already a wildcard index, or would be an invalid wildcard index, so
            // there's no point in implicitly creating any new indexes from it.
            return [];
        }
        out.push(transformToWildcard(keyPattern, field).concat([prefix + out.length + "_"]));
    }
    return out;
}

/**
 * Returns a list of indexes implicitly created by this suite for the current test (up to now).
 */
function getHiddenIndexes(dbName, collName) {
    return db.getSiblingDB(dbName)[collName]
        .aggregate([
            {$indexStats: {}},
            {$addFields: {isHidden: isHiddenWildcardIndex}},
            {$match: {isHidden: true}}
        ])
        .toArray();
}

/**
 * Execute a call to 'runCommand', then perform the following additional actions:
 *  1. createIndexes: we will try to transform the indexes passed into the command into additional
 * (hidden!) wildcard indexes and create them as well.
 *  2. listIndexes: we will filter out any implicitly created wildcard indexes in order to avoid
 * failing in tests that expect only the idnexes they explicitly created.
 *  3. insert: we may have successfully created an implicit compound wildcard index on an earlier
 * index creation call, only to fail to insert a document in the collection with an array value on
 * one of the non-wildcard components in the index. In this case, we will drop all implicitly
 * created indexes.
 */
function runCommandOverride(conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    const collName = cmdObj[cmdName];

    // Actually run the provided command.
    let res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
    if (cmdName == "createIndexes" && res.ok) {
        // We successfully created one or more indexes, so now we can try to convert them into some
        // number of wildcard indexes.
        const indexes = cmdObj.indexes;
        for (const {key, name} of indexes) {
            // Attempt to convert the original index passed to 'createIndexes' to one
            // or more wildcard indexes. note that we attempt to create one at a time, since some of
            // these index creations may fail with an error code we can ignore.
            const wildIndexes = transformIntoWildcardIndexes(key);
            for (const wildIndex of wildIndexes) {
                const [newKey, exclusionProjection, newName] = wildIndex;
                const index = {key: newKey, name: newName + name};
                if (Object.keys(exclusionProjection).length > 0) {
                    index["wildcardProjection"] = exclusionProjection;
                }

                // Note: if we try to run 'createIndexes' directly here, we will
                // re-enter the override method.
                const wildRes = originalRunCommand.apply(
                    conn, makeRunCommandArgs({createIndexes: collName, indexes: [index]}));

                if (wildRes.ok) {
                    print("Created implicit wildcard index: ", tojsononeline(index));
                } else if (!wildRes.ok && !new Set(acceptableErrorCodes).has(wildRes.code)) {
                    print("Unexpected error code: ", wildRes.code, wildRes.errmsg);
                }

                assert.commandWorkedOrFailedWithCode(wildRes, acceptableErrorCodes);
            }
        }
    } else if (cmdName == "listIndexes" && res.ok) {
        // If we're listing indexes, we need to make sure we eliminate any of the
        // implicitly created indexes from consideration.
        if (res.cursor.firstBatch) {
            res.cursor.firstBatch =
                res.cursor.firstBatch.filter(idx => !idx.name.startsWith(prefix));
        }
    } else if (cmdName == "insert" && res.ok) {
        const writeErrors =
            res.writeErrors ? res.writeErrors.filter(err => err.code === 7246301) : [];
        if (writeErrors.length > 0) {
            // We are trying to make a non-wildcard component of the index multikey.
            // Drop all the implicitly created indexes and try again, since we can't be
            // sure which index was problematic.
            print("Trying to make a non-wildcard component of the index multikey!");

            // We must successfully drop our hidden indexes, otherwise we cannot proceed with the
            // test. We're not sure which index was problematic (or if multiple indexes might be
            // problematic) so we just drop them all.
            const hiddenIndexes = getHiddenIndexes(dbName, collName).map(idx => idx.name);
            assert.commandWorked(originalRunCommand.apply(
                conn, makeRunCommandArgs({dropIndexes: collName, index: hiddenIndexes})));

            // Now run the original command again.
            res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
        }
    }

    return res;
}

/**
 * Hides any implicitly created wildcard indexes. Note that tests using {$indexStats} directly have
 * to be manually excluded from the suite, since they won't pass through this override.
 */
DBCollection.prototype.getIndexes = function(filter) {
    const res = this.aggregate([
                        {$indexStats: {}},
                        {$match: filter || {}},
                        // Hide the implicitly created index(es) from tests that look
                        // for indexes.
                        {$addFields: {isHidden: isHiddenWildcardIndex}},
                        {$match: {isHidden: false}},
                        // The information listed in 'spec' is usually returned inline
                        // at the root level.
                        {$replaceWith: {$mergeObjects: ["$$ROOT", "$spec"]}},
                        // This info isn't shown in 'getIndexes'.
                        {$project: {host: 0, accesses: 0, spec: 0, isHidden: 0}},
                    ])
                    .toArray();
    return res;
};

DBCollection.prototype.getIndices = DBCollection.prototype.getIndexes;
DBCollection.prototype.getIndexSpecs = DBCollection.prototype.getIndexes;

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_wildcard_indexes.js");
OverrideHelpers.overrideRunCommand(runCommandOverride);
}());
