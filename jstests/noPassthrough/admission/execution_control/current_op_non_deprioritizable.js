/**
 * Tests that an aggregation whose pipeline starts with a $currentOp stage is marked as
 * non-deprioritizable by execution admission control. This protects monitoring operations from
 * being moved to the low-priority pool.
 *
 * See SERVER-128760 / CLOUDP-319941.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

describe("Aggregation starting with $currentOp is non-deprioritizable", function () {
    let rs;
    let primary;
    let adminDb;

    before(function () {
        rs = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    executionControlDeprioritizationGate: true,
                },
            },
        });
        rs.startSet();
        rs.initiate();

        primary = rs.getPrimary();
        adminDb = primary.getDB("admin");
    });

    after(function () {
        rs.stopSet();
    });

    it("should mark a $currentOp aggregation as non-deprioritizable", function () {
        const before = getTotalMarkedNonDeprioritizableCount(primary);

        // $currentOp must be the first stage and is run as a collectionless aggregation on admin.
        assert.commandWorked(
            adminDb.runCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}),
        );

        const after = getTotalMarkedNonDeprioritizableCount(primary);

        assert.gt(
            after,
            before,
            "Expected totalMarkedNonDeprioritizable to increase after a $currentOp aggregation.",
            {
                before,
                after,
            },
        );
    });

    it("should still mark a $currentOp aggregation with trailing stages as non-deprioritizable", function () {
        const before = getTotalMarkedNonDeprioritizableCount(primary);

        assert.commandWorked(
            adminDb.runCommand({
                aggregate: 1,
                pipeline: [{$currentOp: {}}, {$match: {active: true}}, {$count: "n"}],
                cursor: {},
            }),
        );

        const after = getTotalMarkedNonDeprioritizableCount(primary);

        assert.gt(
            after,
            before,
            "Expected totalMarkedNonDeprioritizable to increase after a $currentOp aggregation with trailing stages.",
            {before, after},
        );
    });
});
