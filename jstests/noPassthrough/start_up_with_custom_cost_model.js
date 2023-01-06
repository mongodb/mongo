/**
 * Tests that 'internalCostModelCoefficients' can be set on startup.
 */
(function() {
'use strict';

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.

function getScanCostWith(customScanCost) {
    const costStr = `{"scanIncrementalCost": ${customScanCost}}`;
    const conn = MongoRunner.runMongod({
        setParameter: {
            'internalCostModelCoefficients': costStr,
            'internalQueryFrameworkControl': "forceBonsai"
        }
    });

    const db = conn.getDB(jsTestName());
    const coll = db.start_up_with_custom_cost_model;

    if (!checkCascadesOptimizerEnabled(db)) {
        jsTestLog("Skipping test because the optimizer is not enabled");
        MongoRunner.stopMongod(conn);
        return;
    }

    const nDocuments = 100;
    assert.commandWorked(coll.insert(Array.from({length: nDocuments}, (_, i) => {
        return {a: 3, b: 3, c: i};
    })));

    // Get scan cost.
    const explain = coll.explain("executionStats").aggregate([]);
    assert.eq(nDocuments, explain.executionStats.nReturned);
    const internalCost =
        assert.commandWorked(db.adminCommand({getParameter: 1, internalCostModelCoefficients: 1}));

    assert(internalCost.hasOwnProperty("internalCostModelCoefficients"));
    assert.eq(internalCost['internalCostModelCoefficients'], costStr);

    const scanNode = navigateToPlanPath(explain, "child");
    assertValueOnPath("PhysicalScan", scanNode, "nodeType");

    const scanCost = scanNode.properties.cost;

    MongoRunner.stopMongod(conn);

    return scanCost;
}

const scanCost1 = getScanCostWith(0.2);
const scanCost2 = getScanCostWith(0.4);
if (scanCost1 === undefined) {
    return;
}

assert.lt(scanCost1, scanCost2);
}());
