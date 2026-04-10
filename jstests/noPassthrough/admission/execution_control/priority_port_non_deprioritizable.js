/**
 * Tests that:
 * 1. Commands executed through the priority port are marked as non-deprioritizable.
 * 2. Index builds initiated through the priority port still run as background tasks
 *    (low-priority), because the build runs on a separate opCtx/client.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_83,
 *   # The priority port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    getLowPriorityWriteCount,
    getTotalMarkedNonDeprioritizableCount,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

describe("Priority port non-deprioritizable", function () {
    let rs;
    let primary;
    let priorityDb;

    before(function () {
        rs = new ReplSetTest({
            nodes: 1,
            usePriorityPorts: true,
            nodeOptions: {
                setParameter: {
                    executionControlDeprioritizationGate: true,
                    executionControlBackgroundTasksDeprioritization: true,
                },
            },
        });
        rs.startSet();
        rs.initiate();

        primary = rs.getPrimary();
        const priorityConn = rs.getNewConnectionToPriorityPort(primary);
        priorityDb = priorityConn.getDB("test");

        // Insert a document so find has something to return.
        assert.commandWorked(primary.getDB("test").coll.insert({_id: 1, x: 1}));
    });

    after(function () {
        rs.stopSet();
    });

    it("should mark find commands through the priority port as non-deprioritizable", function () {
        const before = getTotalMarkedNonDeprioritizableCount(primary);
        assert.commandWorked(priorityDb.runCommand({find: "coll", filter: {_id: 1}}));
        const after = getTotalMarkedNonDeprioritizableCount(primary);

        assert.gt(after, before, "Expected totalMarkedNonDeprioritizable to increase after find on priority port.");
    });

    it("should keep index builds as background tasks even when initiated from priority port", function () {
        const lowPriorityWritesBefore = getLowPriorityWriteCount(primary);

        assert.commandWorked(priorityDb.runCommand({createIndexes: "coll", indexes: [{key: {x: 1}, name: "x_1"}]}));

        const lowPriorityWritesAfter = getLowPriorityWriteCount(primary);

        assert.gt(
            lowPriorityWritesAfter,
            lowPriorityWritesBefore,
            "Expected low-priority write count to increase from the index build, " +
                "indicating the build thread was correctly treated as a background task " +
                "and not marked as non-deprioritizable from the priority port.",
        );
    });
});
