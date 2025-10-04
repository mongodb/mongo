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
 *   uses_parallel_shell,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

describe("Query shape hash in $currentOp", function () {
    const coll = assertDropAndRecreateCollection(db, jsTestName());
    const qsutils = new QuerySettingsUtils(db, coll.getName());

    // Comment used to identify the query in '$currentOp' aggregation stage.
    const comment = "queryShapeHashInCurrentOpTest";
    const queryFilter = {
        x: 1,
        y: 1,
    };

    const findQueryRepresentative = qsutils.makeFindQueryInstance({filter: queryFilter, comment});
    const findQuery = qsutils.withoutDollarDB(findQueryRepresentative);

    // Fail point used to hang inside 'find' operation while parallel shell queries '$currentOp'.
    const kFailPointInFind = "waitInFindBeforeMakingBatch";

    function getQueryShapeHashFromCurrentOp(failPointName, cmd) {
        jsTest.log.info(`[denis631] Create Fail Point ${failPointName} for '${coll.getFullName()}' collection.`);
        const failPoint = configureFailPoint(db, failPointName, {nss: coll.getFullName()});

        jsTest.log.info(`[denis631] Start 'cmd' in parallel shell.`, {cmd});
        const cmdShell = startParallelShell(
            funWithArgs(
                (dbName, cmd) => {
                    let testDB = db.getSiblingDB(dbName);
                    return assert.commandWorked(testDB.runCommand(cmd));
                },
                db.getName(),
                cmd,
            ),
            db.getMongo().port,
        );
        jsTest.log.info("[denis631] waiting for fail point");
        failPoint.wait();
        jsTest.log.info("[denis631] done waiting for fail point");

        const queryShapeHashFromCurrentOp = (() => {
            const currentOps = waitForCurOpByFailPoint(
                db,
                coll.getFullName(),
                failPointName,
                {"command.filter": queryFilter},
                {localOps: true},
            );
            jsTest.log.info("$currentOps", {currentOps});
            assert.eq(1, currentOps.length, `expecting only 1 currentOp, 'currentOps': ${JSON.stringify(currentOps)}`);
            assert(
                currentOps[0].queryShapeHash,
                `expect 'currentOp' to have queryShapeHash field. 'currentOps': ${JSON.stringify(currentOps)}`,
            );
            return currentOps[0].queryShapeHash;
        })();
        failPoint.off();

        jsTest.log.info("Await execution of parallel shell.");
        cmdShell();

        return queryShapeHashFromCurrentOp;
    }

    before(function () {
        qsutils.removeAllQuerySettings();

        assert.commandWorked(coll.createIndexes([{x: 1}, {y: 1}, {x: 1, y: 1}]));
        assert.commandWorked(
            coll.insertMany([
                {x: 1, y: 1},
                {x: 2, y: 2},
                {x: 3, y: 3},
            ]),
        );
    });

    it("for find should be same as QueryShapeHash reported in explain", function () {
        const queryShapeHashFromCurrentOp = getQueryShapeHashFromCurrentOp(kFailPointInFind, findQuery);

        jsTest.log.info("Make sure query shape hash from '$currentOp' matches the one from explain.");
        const queryShapeHashFromExplain = qsutils.getQueryShapeHashFromExplain(findQueryRepresentative);
        assert.eq(
            queryShapeHashFromCurrentOp,
            queryShapeHashFromExplain,
            "Query shape hash from the '$currentOp' doesn't match the one from explain.",
        );
    });
});
