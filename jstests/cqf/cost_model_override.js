/**
 * Tests that when we modify Cost Model Coefficents using `internalCostModelCoefficients` parameter
 * the cost of produced query plan changed.
 */

(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cost_model_override;
coll.drop();

const nDocuments = 100;
assert.commandWorked(coll.insert(Array.from({length: nDocuments}, (_, i) => {
    return {a: 3, b: 3, c: i};
})));

function executeAndGetScanCost(scanIncrementalCost) {
    try {
        assert.commandWorked(db.adminCommand({
            'setParameter': 1,
            'internalCostModelCoefficients': `{"scanIncrementalCost": ${scanIncrementalCost}}`
        }));

        const explain = coll.explain("executionStats").aggregate([]);
        assert.eq(nDocuments, explain.executionStats.nReturned);

        const scanNode = navigateToPlanPath(explain, "child");
        assertValueOnPath("PhysicalScan", scanNode, "nodeType");

        return scanNode.properties.cost;
    } finally {
        assert.commandWorked(
            db.adminCommand({'setParameter': 1, 'internalCostModelCoefficients': ''}));
    }
}

const scanCost1 = executeAndGetScanCost(0.2);
const scanCost2 = executeAndGetScanCost(0.4);
assert.lt(scanCost1, scanCost2);
}());
