/**
 * Confirms that $currentOp provides query shape hash for queries that have shape.
 *
 * Excluding test suites that do not expect parallel shell or connect to shards directly.
 * @tags: [
 *   # Rerouting reads makes the test miss the fail point inside 'find'.
 *   assumes_read_preference_unchanged,
 *   # This test is using fail point which is not supported in serverless.
 *   command_not_supported_in_serverless,
 *   directly_against_shardsvrs_incompatible,
 *   tenant_migration_incompatible,
 *   uses_parallel_shell,
 *   requires_fcv_80,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();
const queryFilter = {
    x: 1,
    y: 1
};
const findQueryRepresentative = qsutils.makeFindQueryInstance({filter: queryFilter});
const findQuery = qsutils.withoutDollarDB(findQueryRepresentative);

jsTestLog("Create indexes for query planner to have some work.");
assert.commandWorked(coll.createIndexes([{x: 1}, {y: 1}, {x: 1, y: 1}]));

jsTestLog("Create Fail Point.");
// Fail point used to hang inside 'find' operation while parallel shell queries '$currentOp'.
const kFailPointInFind = "waitInFindBeforeMakingBatch";
let findCmdFailPoint = configureFailPoint(db, kFailPointInFind, {nss: coll.getFullName()});

jsTestLog("Start 'find' in parallel shell.");
const joinParallelFindThread = startParallelShell(
    funWithArgs(
        (dbName, findQuery) => {
            let testDB = db.getSiblingDB(dbName);
            return assert.commandWorked(testDB.runCommand(findQuery));
        },
        db.getName(),
        findQuery),
    db.getMongo().port,
);

jsTestLog("Get query shape hash reported by '$currentOp' aggregation stage.");
const queryShapeHashFromCurrentOp = (() => {
    const currentOps = waitForCurOpByFailPoint(db,
                                               coll.getFullName(),
                                               kFailPointInFind,
                                               {"command.filter": queryFilter},
                                               {localOps: true});
    assert.eq(1,
              currentOps.length,
              `expecting only 1 currentOp, 'currentOps': ${JSON.stringify(currentOps)}`);
    assert(currentOps[0].queryShapeHash,
           `expect 'currentOp' to have queryShapeHash field. 'currentOps': ${
               JSON.stringify(currentOps)}`);
    return currentOps[0].queryShapeHash;
})();

findCmdFailPoint.off();

jsTestLog("Await execution of parallel shell.");
joinParallelFindThread();

const querySettings = {
    indexHints: {
        ns: {db: db.getName(), coll: coll.getName()},
        allowedIndexes: [{x: 1}],
    }
};

jsTestLog("Make sure query shape hash from '$currentOp' matches the one from query settings.");
qsutils.withQuerySettings(findQueryRepresentative, querySettings, () => {
    const queryShapeHashFromQuerySettings =
        qsutils.getQueryShapeHashFromQuerySettings(findQueryRepresentative);
    assert.eq(queryShapeHashFromCurrentOp,
              queryShapeHashFromQuerySettings,
              "Query shape hash from the '$currentOp' doesn't match the one from query settings.");
});
