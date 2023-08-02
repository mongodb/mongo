/**
 * This test serves as a baseline for measuring the performance of the RollbackTest fixture.
 */

import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

let rollbackTest = new RollbackTest();
rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();
rollbackTest.stop();
