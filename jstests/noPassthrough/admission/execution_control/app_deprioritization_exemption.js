/**
 * Tests for execution control deprioritization exemptions.
 *
 * @tags: [
 *   # The test deploys replica sets with a execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 *   # The test is flaky in slow variants when couting the number of deprioritizations.
 *   incompatible_with_windows_tls,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    getLowPriorityReadCount,
    getNormalPriorityFinishedCount,
    insertTestDocuments,
    setExecutionControlDeprioritizationExemptions,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

function createAppConnection(host, appName) {
    return new Mongo(`mongodb://${host}/?appName=${appName}`);
}

function getDeprioritizationTestParams(exemptions = []) {
    return {
        executionControlDeprioritizationGate: true,
        executionControlHeuristicDeprioritization: true,
        executionControlBackgroundTasksDeprioritization: false,
        executionControlHeuristicNumAdmissionsDeprioritizeThreshold: 5,
        internalQueryExecYieldIterations: 1,
        executionControlApplicationDeprioritizationExemptions: {appNames: exemptions},
    };
}

describe("Execution control deprioritization exemptions", function () {
    describe("Get and set deprioritization exemptions", function () {
        let standalone;

        before(function () {
            standalone = MongoRunner.runMongod({
                setParameter: {
                    executionControlApplicationDeprioritizationExemptions: {appNames: ["foo", "bar", "baz"]},
                },
            });
        });

        after(function () {
            MongoRunner.stopMongod(standalone);
        });

        it("should get exemptions set at startup", function () {
            const res = assert.commandWorked(
                standalone.adminCommand({getParameter: 1, executionControlApplicationDeprioritizationExemptions: 1}),
            );
            assert.eq(res.executionControlApplicationDeprioritizationExemptions, {appNames: ["foo", "bar", "baz"]});
        });

        it("should update exemptions at runtime", function () {
            assert.commandWorked(
                standalone.adminCommand({
                    setParameter: 1,
                    executionControlApplicationDeprioritizationExemptions: {appNames: ["newFoo", "newBar"]},
                }),
            );
            const res = assert.commandWorked(
                standalone.adminCommand({getParameter: 1, executionControlApplicationDeprioritizationExemptions: 1}),
            );
            assert.eq(res.executionControlApplicationDeprioritizationExemptions, {appNames: ["newFoo", "newBar"]});
        });

        it("should clear exemptions when set to empty", function () {
            assert.commandWorked(
                standalone.adminCommand({
                    setParameter: 1,
                    executionControlApplicationDeprioritizationExemptions: {appNames: []},
                }),
            );
            const res = assert.commandWorked(
                standalone.adminCommand({getParameter: 1, executionControlApplicationDeprioritizationExemptions: 1}),
            );
            assert.eq(res.executionControlApplicationDeprioritizationExemptions, {appNames: []});
        });
    });

    describe("Exemptions protect from deprioritization", function () {
        let replTest, primary, coll;
        const kNumDocs = 1000;
        const kExemptApp = "exemptApp";

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: getDeprioritizationTestParams([kExemptApp])},
            });
            replTest.startSet();
            replTest.initiate();
            primary = replTest.getPrimary();
            coll = primary.getDB(jsTestName()).coll;
            insertTestDocuments(coll, kNumDocs, {payloadSize: 256, includeRandomString: true});
            assert.commandWorked(coll.createIndex({payload: 1}));
        });

        after(function () {
            replTest.stopSet();
        });

        it("should NOT deprioritize exempt apps", function () {
            const initialLowPriority = getLowPriorityReadCount(primary);
            const conn = createAppConnection(primary.host, kExemptApp);
            conn.getDB(jsTestName()).coll.find().hint({$natural: 1}).itcount();
            assert.eq(getLowPriorityReadCount(primary), initialLowPriority);
        });

        it("should deprioritize non-exempt apps", function () {
            const initialLowPriority = getLowPriorityReadCount(primary);
            const conn = createAppConnection(primary.host, "nonExemptApp");
            conn.getDB(jsTestName()).coll.find().hint({$natural: 1}).itcount();
            assert.gt(getLowPriorityReadCount(primary), initialLowPriority);
        });

        it("should handle runtime exemption changes", function () {
            const appName = "dynamicExemptApp";

            // Initially NOT exempt - should be deprioritized.
            let conn = createAppConnection(primary.host, appName);
            const before1 = getLowPriorityReadCount(primary);
            conn.getDB(jsTestName()).coll.find().hint({$natural: 1}).itcount();
            assert.gt(getLowPriorityReadCount(primary), before1);

            // Add to exemptions.
            setExecutionControlDeprioritizationExemptions(primary, [kExemptApp, appName]);

            // New connection should be exempt.
            conn = createAppConnection(primary.host, appName);
            const before2 = getLowPriorityReadCount(primary);
            conn.getDB(jsTestName()).coll.find().hint({$natural: 1}).itcount();
            assert.eq(getLowPriorityReadCount(primary), before2);
        });

        it("should apply exemption change mid-operation: non-exempt to exempt", function () {
            const kFailPoint = "setYieldAllLocksHang";
            const appName = "midOpExemptTest";
            const comment = "midOpExemptTestComment";

            setExecutionControlDeprioritizationExemptions(primary, [kExemptApp]);
            const initialLowPriority = getLowPriorityReadCount(primary);

            assert.commandWorked(
                primary.adminCommand({
                    configureFailPoint: kFailPoint,
                    mode: {skip: 10},
                    data: {namespace: coll.getFullName()},
                }),
            );

            let waitForShell;
            try {
                waitForShell = startParallelShell(
                    `
                    const conn = new Mongo("mongodb://${primary.host}/?appName=${appName}");
                    conn.getDB("${jsTestName()}").coll.find().hint({$natural: 1}).comment("${comment}").itcount();
                `,
                    primary.port,
                );

                // Wait for deprioritization.
                assert.soon(() => getLowPriorityReadCount(primary) > initialLowPriority);

                // Wait for failpoint.
                assert.soon(() => {
                    const ops = primary
                        .getDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, localOps: true}},
                            {$match: {numYields: {$gt: 10}, "command.comment": comment}},
                        ])
                        .toArray();
                    return ops.length > 0;
                });

                // Add exemption mid-operation.
                setExecutionControlDeprioritizationExemptions(primary, [kExemptApp, appName]);
            } finally {
                assert.commandWorked(primary.adminCommand({configureFailPoint: kFailPoint, mode: "off"}));
                if (waitForShell) waitForShell();
            }
        });

        it("should apply exemption change mid-operation: exempt to non-exempt", function () {
            const kFailPoint = "setYieldAllLocksHang";
            const appName = "midOpRemoveExemptTest";
            const comment = "midOpRemoveExemptTestComment";

            setExecutionControlDeprioritizationExemptions(primary, [kExemptApp, appName]);
            const initialLowPriority = getLowPriorityReadCount(primary);

            assert.commandWorked(
                primary.adminCommand({
                    configureFailPoint: kFailPoint,
                    mode: {skip: 10},
                    data: {namespace: coll.getFullName()},
                }),
            );

            let waitForShell;
            let midOpLowPriority;
            try {
                waitForShell = startParallelShell(
                    `
                    const conn = new Mongo("mongodb://${primary.host}/?appName=${appName}");
                    conn.getDB("${jsTestName()}").coll.find().hint({$natural: 1}).comment("${comment}").itcount();
                `,
                    primary.port,
                );

                // Wait for failpoint.
                assert.soon(() => {
                    const ops = primary
                        .getDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, localOps: true}},
                            {$match: {numYields: {$gt: 10}, "command.comment": comment}},
                        ])
                        .toArray();
                    return ops.length > 0;
                });

                // Should NOT be deprioritized (exempt).
                midOpLowPriority = getLowPriorityReadCount(primary);
                assert.eq(midOpLowPriority, initialLowPriority);

                // Remove exemption mid-operation.
                setExecutionControlDeprioritizationExemptions(primary, [kExemptApp]);
            } finally {
                assert.commandWorked(primary.adminCommand({configureFailPoint: kFailPoint, mode: "off"}));
                if (waitForShell) waitForShell();
            }

            // After removing exemption, subsequent yields should deprioritize.
            assert.gt(getLowPriorityReadCount(primary), midOpLowPriority);
        });

        it("should exempt operations matching app name prefix", function () {
            setExecutionControlDeprioritizationExemptions(primary, [kExemptApp, "testPrefix"]);
            const initialLowPriority = getLowPriorityReadCount(primary);
            const conn = createAppConnection(primary.host, "testPrefixApp1");
            conn.getDB(jsTestName()).coll.find().hint({$natural: 1}).itcount();
            assert.eq(getLowPriorityReadCount(primary), initialLowPriority);
        });

        it("should still use normal priority tickets for exempt apps", function () {
            const initialNormal = getNormalPriorityFinishedCount(primary);
            const conn = createAppConnection(primary.host, kExemptApp);
            for (let i = 0; i < 10; i++) {
                conn.getDB(jsTestName()).coll.find({_id: i}).itcount();
            }
            assert.gt(getNormalPriorityFinishedCount(primary), initialNormal);
        });
    });

    describe("Initial sync deprioritization", function () {
        const kNumDocs = 5000;
        const kInitialSyncExemptions = ["NetworkInterfaceTL", "OplogFetcher", "MongoDB Internal Client"];
        let replTest;

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {setParameter: getDeprioritizationTestParams()},
            });
            replTest.startSet();
            replTest.initiate();
        });

        after(function () {
            if (replTest) replTest.stopSet();
        });

        it("should deprioritize initial sync WITHOUT replication exemptions", function () {
            const primary = replTest.getPrimary();
            insertTestDocuments(primary.getDB(jsTestName()).coll, kNumDocs, {payloadSize: 256});

            const beforeLowPriority = getLowPriorityReadCount(primary);
            const newNode = replTest.add();
            replTest.reInitiate();
            replTest.awaitSecondaryNodes();

            assert.eq(newNode.getDB(jsTestName()).coll.countDocuments({}), kNumDocs);
            const afterLowPriority = getLowPriorityReadCount(primary);
            assert.gt(
                afterLowPriority,
                beforeLowPriority,
                "Initial sync should be deprioritized without replication exemptions",
            );
        });

        it("should NOT deprioritize initial sync WITH replication exemptions", function () {
            const primary = replTest.getPrimary();
            setExecutionControlDeprioritizationExemptions(primary, kInitialSyncExemptions);
            insertTestDocuments(primary.getDB(jsTestName()).coll, kNumDocs, {payloadSize: 256, startId: kNumDocs});

            const beforeLowPriority = getLowPriorityReadCount(primary);
            const newNode = replTest.add();
            replTest.reInitiate();
            replTest.awaitSecondaryNodes();

            assert.eq(newNode.getDB(jsTestName()).coll.countDocuments({}), 2 * kNumDocs);
            const afterLowPriority = getLowPriorityReadCount(primary);
            assert.eq(
                afterLowPriority,
                beforeLowPriority,
                "Initial sync should NOT be deprioritized with replication exemptions",
            );
        });
    });
});
