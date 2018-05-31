/**
 * Confirms that a parent operation correctly inherits 'numYields' from each of its child operations
 * as the latter are popped off the CurOp stack.
 */
(function() {
    "use strict";

    // Start a single mongoD using MongoRunner.
    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    // Create the test DB and collection.
    const testDB = conn.getDB("currentop_yield");
    const adminDB = conn.getDB("admin");
    const testColl = testDB.test;

    // Queries current operations until a single matching operation is found.
    function awaitMatchingCurrentOp(match) {
        let currentOp = null;
        assert.soon(() => {
            currentOp = adminDB.aggregate([{$currentOp: {}}, match]).toArray();
            return (currentOp.length === 1);
        });
        return currentOp[0];
    }

    // Executes a bulk remove using the specified 'docsToRemove' array, captures the 'numYields'
    // metrics from each child op, and confirms that the parent op's 'numYields' total is equivalent
    // to the sum of the child ops.
    function runYieldTest(docsToRemove) {
        // Sets parameters such that all operations will yield & the operation hangs on the server
        // when we need to test.
        assert.commandWorked(
            testDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "alwaysOn"}));
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "hangAfterAllChildRemoveOpsArePopped", mode: "alwaysOn"}));

        // Starts parallel shell to run the command that will hang.
        const awaitShell = startParallelShell(`{
            const testDB = db.getSiblingDB("currentop_yield");
            const bulkRemove = testDB.test.initializeOrderedBulkOp();
            for(let doc of ${tojsononeline(docsToRemove)}) {
                bulkRemove.find(doc).removeOne();
            }
            bulkRemove.execute();
        }`,
                                              testDB.getMongo().port);

        let childOpId = null;
        let childYields = 0;

        // Get child operations and sum yields. Each child op encounters two failpoints while
        // running: 'hangBeforeChildRemoveOpFinishes' followed by 'hangBeforeChildRemoveOpIsPopped'.
        // We use these two failpoints as an 'airlock', hanging at the first while we enable the
        // second, then hanging at the second while we enable the first, to ensure that each child
        // op is caught and their individual 'numYields' recorded.
        for (let childCount = 0; childCount < docsToRemove.length; childCount++) {
            // Wait for the child op to hit the first of two failpoints.
            let childCurOp = awaitMatchingCurrentOp(
                {$match: {ns: testColl.getFullName(), msg: "hangBeforeChildRemoveOpFinishes"}});

            // Add the child's yield count to the running total, and record the opid.
            assert(childOpId === null || childOpId === childCurOp.opid);
            assert.gt(childCurOp.numYields, 0);
            childYields += childCurOp.numYields;
            childOpId = childCurOp.opid;

            // Enable the subsequent 'hangBeforeChildRemoveOpIsPopped' failpoint, just after the
            // child op finishes but before it is popped from the stack.
            assert.commandWorked(testDB.adminCommand(
                {configureFailPoint: "hangBeforeChildRemoveOpIsPopped", mode: "alwaysOn"}));

            // Let the operation proceed to the 'hangBeforeChildRemoveOpIsPopped' failpoint.
            assert.commandWorked(testDB.adminCommand(
                {configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "off"}));
            awaitMatchingCurrentOp(
                {$match: {ns: testColl.getFullName(), msg: "hangBeforeChildRemoveOpIsPopped"}});

            // If this is not the final child op, re-enable the 'hangBeforeChildRemoveOpFinishes'
            // failpoint from earlier so that we don't miss the next child.
            if (childCount + 1 < docsToRemove.length) {
                assert.commandWorked(testDB.adminCommand(
                    {configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "alwaysOn"}));
            }

            // Finally, allow the operation to continue.
            assert.commandWorked(testDB.adminCommand(
                {configureFailPoint: "hangBeforeChildRemoveOpIsPopped", mode: "off"}));
        }

        // Wait for the operation to hit the 'hangAfterAllChildRemoveOpsArePopped' failpoint, then
        // take the total number of yields recorded by the parent op.
        const parentCurOp = awaitMatchingCurrentOp(
            {$match: {opid: childOpId, op: "command", msg: "hangAfterAllChildRemoveOpsArePopped"}});

        // Verify that the parent's yield count equals the sum of the child ops' yields.
        assert.eq(parentCurOp.numYields, childYields);
        assert.eq(parentCurOp.opid, childOpId);

        // Allow the parent operation to complete.
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "hangAfterAllChildRemoveOpsArePopped", mode: "off"}));

        // Wait for the parallel shell to complete.
        awaitShell();
    }

    // Test that a parent remove op inherits the sum of its children's yields for a single remove.
    assert.commandWorked(testDB.test.insert({a: 2}));
    runYieldTest([{a: 2}]);

    // Test that a parent remove op inherits the sum of its children's yields for multiple removes.
    const docsToTest = [{a: 1}, {a: 2}, {a: 3}, {a: 4}, {a: 5}];
    assert.commandWorked(testDB.test.insert(docsToTest));
    runYieldTest(docsToTest);

    MongoRunner.stopMongod(conn);
})();
