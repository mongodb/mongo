/**
 * Fixture to test rollback permutations with index builds.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

export class RollbackIndexBuildsTest {
    constructor(expectedErrors) {
        jsTestLog("Set up a Rollback Test.");
        const replTest = new ReplSetTest({
            name: jsTestName(),
            nodes: 3,
            useBridge: true,
        });
        replTest.startSet();
        let config = replTest.getReplSetConfig();
        config.members[2].priority = 0;
        config.settings = {chainingAllowed: false};
        replTest.initiate(config);
        this.rollbackTest = new RollbackTest(jsTestName(), replTest);
        this.expectedErrors = expectedErrors;
    }

    // Given two ordered arrays, returns all permutations of the two using all elements of each.
    static makeSchedules(rollbackOps, indexBuildOps) {
        // Appends to the 'result' array all permutations of the interleavings between two ordered
        // arrays.
        function makeCombinations(listA, listB, result, accum = []) {
            if (listA.length == 0 && listB.length == 0) {
                result.push(accum);
                accum = [];
            }

            if (listA.length) {
                let copy = accum.slice();
                copy.push(listA[0]);
                makeCombinations(listA.slice(1), listB, result, copy);
            }
            if (listB.length) {
                let copy = accum.slice();
                copy.push(listB[0]);
                makeCombinations(listA, listB.slice(1), result, copy);
            }
        }

        let schedules = [];
        // This function is exponential. Limit the number of operations to prevent generating
        // extremely large schedules.
        assert.lte(rollbackOps.length, 5);
        assert.lte(indexBuildOps.length, 5);
        makeCombinations(rollbackOps, indexBuildOps, schedules);
        jsTestLog("Generated " + schedules.length + " schedules ");
        return schedules;
    }

    runSchedules(schedules) {
        const self = this;
        let i = 0;
        schedules.forEach(function (schedule) {
            const collName = "coll_" + i;
            const indexSpec = {a: 1};

            jsTestLog("iteration: " + i + " collection: " + collName + " schedule: " + tojson(schedule));

            const primary = self.rollbackTest.getPrimary();
            const primaryDB = primary.getDB("test");
            const collection = primaryDB.getCollection(collName);

            let transitionedToSteadyState = false;
            let createdColl = false;
            let indexBuilds = [];

            schedule.forEach(function (op) {
                print("Running operation: " + op);
                switch (op) {
                    case "holdStableTimestamp":
                        assert.commandWorked(
                            primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}),
                        );
                        break;
                    case "transitionToRollback": {
                        const curPrimary = self.rollbackTest.transitionToRollbackOperations();
                        assert.eq(curPrimary, primary);
                        break;
                    }
                    case "transitionToSteadyState":
                        self.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

                        // After transitioning to rollback, allow the index build to complete on
                        // the rolling-back node so that rollback can finish.
                        IndexBuildTest.resumeIndexBuilds(primary);

                        self.rollbackTest.transitionToSyncSourceOperationsDuringRollback();

                        // To speed up the test, defer data validation until the fixture shuts down.
                        self.rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});
                        transitionedToSteadyState = true;
                        break;
                    case "createColl":
                        assert.commandWorked(collection.insert({a: "created collection explicitly"}));
                        createdColl = true;
                        break;
                    case "start":
                        if (!createdColl) {
                            assert.commandWorked(collection.insert({a: "created collection with start"}));
                            createdColl = true;
                        }
                        IndexBuildTest.pauseIndexBuilds(primary);

                        var errcodes = self.expectedErrors ? self.expectedErrors : [];
                        // This test creates indexes with majority of nodes not available for
                        // replication. So, disabling index build commit quorum.
                        indexBuilds.push(
                            IndexBuildTest.startIndexBuild(
                                primary,
                                collection.getFullName(),
                                indexSpec,
                                {},
                                errcodes,
                                0,
                            ),
                        );

                        IndexBuildTest.waitForIndexBuildToScanCollection(primaryDB, collName, "a_1");
                        break;
                    case "commit":
                        IndexBuildTest.resumeIndexBuilds(primary);
                        IndexBuildTest.waitForIndexBuildToStop(primaryDB, collName, "a_1");
                        break;
                    case "abort": {
                        const opId = IndexBuildTest.getIndexBuildOpId(primaryDB, collName, "a_1");
                        assert.commandWorked(primaryDB.killOp(opId));
                        IndexBuildTest.resumeIndexBuilds(primary);
                        IndexBuildTest.waitForIndexBuildToStop(primaryDB, collName, "a_1");
                        break;
                    }
                    case "drop":
                        collection.dropIndexes(indexSpec);
                        break;
                    default:
                        assert(false, "unknown operation for test: " + op);
                }
            });

            // Check for success -- any expected Error failures were passed
            // as parameters to the startIndexBuild() call
            indexBuilds.forEach((indexBuild) => indexBuild({checkExitSuccess: true}));

            if (!transitionedToSteadyState) {
                self.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
                self.rollbackTest.transitionToSyncSourceOperationsDuringRollback();

                // To speed up the test, defer data validation until the fixture shuts down.
                self.rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});
            }

            assert.commandWorked(primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "off"}));
            i++;
        });
    }

    stop() {
        this.rollbackTest.stop();
    }
}
