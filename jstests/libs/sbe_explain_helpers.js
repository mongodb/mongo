/**
 * Helpers for checking correctness of generated SBE plans when expected explain() output differs.
 */

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

// SBE does not support building IDHACK plans. As such, if SBE is being used, we assert that the
// generated plan is an 'expectedParentStageForIxScan' + IXSCAN over the _id index.
function assertIdHackPlan(db, root, expectedParentStageForIxScan, isSBEEnabled) {
    if (isSBEEnabled) {
        const parentStage = getPlanStage(root, expectedParentStageForIxScan);
        assert.neq(parentStage, null, root);

        const ixscan = parentStage.inputStage;
        assert.neq(ixscan, null, root);
        assert.eq(ixscan.stage, "IXSCAN", root);
        assert(!ixscan.hasOwnProperty("filter"), root);
        assert.eq(ixscan.indexName, "_id_", root);
    } else {
        assert(isIdhack(db, root), root);
    }
}

// When SBE is enabled, we assert that the generated plan used an IXSCAN. Otherwise, we assert that
// 'isIdhack' returns false.
function assertNonIdHackPlan(db, root, isSBEEnabled) {
    if (isSBEEnabled) {
        assert(isIxscan(db, root), root);
    } else {
        assert(!isIdhack(db, root), root);
    }
}
