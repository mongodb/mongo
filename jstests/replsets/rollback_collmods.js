/**
 * Tests that collMod commands during every stage of rollback are tracked correctly.
 * This especially targets collection validators that begin partially or fully uninitialized.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    const testName = "rollback_collmods";
    const dbName = testName;

    var coll1Name = "NoInitialValidationAtAll";
    var coll2Name = "NoInitialValidationAction";
    var coll3Name = "NoInitialValidator";
    var coll4Name = "NoInitialValidationLevel";

    function printCollectionOptionsForNode(node, time) {
        let opts = assert.commandWorked(node.getDB(dbName).runCommand({"listCollections": 1}));
        jsTestLog("Collection options " + time + " on " + node.host + ": " + tojson(opts));
    }

    function printCollectionOptions(rollbackTest, time) {
        printCollectionOptionsForNode(rollbackTest.getPrimary(), time);
        printCollectionOptionsForNode(rollbackTest.getSecondary(), time);
    }

    // Operations that will be present on both nodes, before the common point.
    let CommonOps = (node) => {
        let testDb = node.getDB(dbName);
        assert.writeOK(testDb[coll1Name].insert({a: 1, b: 1}));
        assert.writeOK(testDb[coll2Name].insert({a: 2, b: 2}));
        assert.writeOK(testDb[coll3Name].insert({a: 3, b: 3}));
        assert.writeOK(testDb[coll4Name].insert({a: 4, b: 4}));

        // Start with no validation action.
        assert.commandWorked(testDb.runCommand({
            collMod: coll2Name,
            validator: {a: 1},
            validationLevel: "moderate",
        }));

        // Start with no validator.
        assert.commandWorked(testDb.runCommand(
            {collMod: coll3Name, validationLevel: "moderate", validationAction: "warn"}));

        // Start with no validation level.
        assert.commandWorked(
            testDb.runCommand({collMod: coll4Name, validator: {a: 1}, validationAction: "warn"}));
    };

    // Operations that will be performed on the rollback node past the common point.
    let RollbackOps = (node) => {
        let testDb = node.getDB(dbName);

        // Set everything on the rollback node.
        assert.commandWorked(testDb.runCommand({
            collMod: coll1Name,
            validator: {a: 1},
            validationLevel: "moderate",
            validationAction: "warn"
        }));

        // Only modify the action, and never modify it again so it needs to be reset to empty.
        assert.commandWorked(testDb.runCommand({collMod: coll2Name, validationAction: "error"}));

        // Only modify the validator, and never modify it again so it needs to be reset to empty.
        assert.commandWorked(testDb.runCommand({collMod: coll3Name, validator: {b: 1}}));

        // Only modify the level, and never modify it again so it needs to be reset to empty.
        assert.commandWorked(testDb.runCommand({
            collMod: coll4Name,
            validationLevel: "moderate",
        }));
    };

    // Operations that will be performed on the sync source node after rollback.
    let SteadyStateOps = (node) => {
        let testDb = node.getDB(dbName);

        assert.commandWorked(testDb.runCommand({collMod: coll2Name, validator: {b: 1}}));
        assert.commandWorked(testDb.runCommand({collMod: coll3Name, validationAction: "error"}));
        assert.commandWorked(testDb.runCommand({collMod: coll4Name, validationAction: "error"}));
    };

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName);
    CommonOps(rollbackTest.getPrimary());

    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    printCollectionOptions(rollbackTest, "before branch");
    RollbackOps(rollbackNode);

    rollbackTest.transitionToSyncSourceOperations();
    printCollectionOptions(rollbackTest, "before rollback");
    // No ops on the sync source.

    // Wait for rollback to finish.
    rollbackTest.transitionToSteadyStateOperations({waitForRollback: true});
    printCollectionOptions(rollbackTest, "after rollback");

    SteadyStateOps(rollbackTest.getPrimary());
    printCollectionOptions(rollbackTest, "at completion");

    rollbackTest.stop();
})();
