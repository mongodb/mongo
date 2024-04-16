/**
 * Verify that the LatchAnalyzer is working to expectations
 *
 * @tags: [
 *   multiversion_incompatible,
 *   no_selinux,
 *   requires_latch_analyzer,
 *   multi_clients_incompatible,
 * ]
 */

const failPointName = "enableLatchAnalysis";

function getLatchAnalysis() {
    let serverStatus = assert.commandWorked(db.serverStatus({latchAnalysis: 1}));
    return serverStatus.latchAnalysis;
}

function verifyField(fieldValue) {
    // Every field in the latchAnalysis object is an integer that is greater than zero
    // This function is meant to verify those conditions

    assert.neq(fieldValue, null);
    assert(typeof fieldValue == 'number');
    assert(fieldValue >= 0);
}

function verifyLatchAnalysis({analysis, shouldHaveHierarchy}) {
    assert(analysis);

    if (shouldHaveHierarchy) {
        jsTestLog("Failpoint is on; latch analysis: " + tojson(analysis));
    } else {
        jsTestLog("Failpoint is off; should be only basic stats: " + tojson(analysis));
    }

    for (var key in analysis) {
        let singleLatch = analysis[key];
        verifyField(singleLatch.created);
        verifyField(singleLatch.destroyed);
        verifyField(singleLatch.acquired);
        verifyField(singleLatch.released);
        verifyField(singleLatch.contended);

        const acquiredAfter = singleLatch.acquiredAfter;
        const releasedBefore = singleLatch.releasedBefore;
        if (!shouldHaveHierarchy) {
            assert(!acquiredAfter);
            assert(!releasedBefore);
            continue;
        }

        for (var otherKey in acquiredAfter) {
            verifyField(acquiredAfter[otherKey]);
        }

        for (var otherKey in releasedBefore) {
            verifyField(releasedBefore[otherKey]);
        }
    }
}

try {
    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: false});
    }

    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "alwaysOn",
    }));

    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: true});
    }

    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "off",
    }));

    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: false});
    }
} finally {
    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "off",
    }));
}