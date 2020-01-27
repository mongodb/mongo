/**
 * Test of the RollbackTest helper library.
 */
(function() {
"use strict";

load("jstests/replsets/rslib.js");
load("jstests/replsets/libs/rollback_test.js");

let checkDataConsistencyCallCount = 0;
let stopSetCallCount = 0;

const rollbackTest = new RollbackTest("rollbacktest_unittest");
rollbackTest._checkDataConsistencyImpl = function() {
    ++checkDataConsistencyCallCount;
};

const rst = rollbackTest.getTestFixture();
rst.stopSet = function(signal, forRestart, opts) {
    // Unconditionally skip in rst.stopSet because rbt.stop does its own validation.
    assert.eq(opts, {"skipCheckDBHashes": true, "skipValidation": true});
    ++stopSetCallCount;

    // We don't care about doing the regular stopSet actions.
    for (let i = rst.nodeList().length - 1; i >= 0; --i) {
        rst.stop(i);
    }
};

rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.stop();

assert.eq(checkDataConsistencyCallCount, 1);
assert.eq(stopSetCallCount, 1);
})();
