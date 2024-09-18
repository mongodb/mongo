/**
 * Utility functions for explain() results of search + non-search queries run on a view.
 */

function assertIdLookupContainsViewPipeline(explainStages, viewPipeline) {
    assert(explainStages[1].hasOwnProperty("$_internalSearchIdLookup"));
    assert(explainStages[1]["$_internalSearchIdLookup"].hasOwnProperty("subPipeline"));
    let idLookupFullSubPipe = explainStages[1]["$_internalSearchIdLookup"]["subPipeline"];
    // The _idLookup subPipeline should be a $match on _id followed by the view stages.
    let idLookupStage = {"$match": {"_id": {"$eq": "_id placeholder"}}};
    assert.eq(idLookupFullSubPipe[0], idLookupStage);
    // Make sure that idLookup subpipeline contains all of the view stages.
    let idLookupViewStages = idLookupFullSubPipe.slice(-(idLookupFullSubPipe.length - 1));
    assert.eq(idLookupViewStages.length, viewPipeline.length);
    for (let i = 0; i < idLookupViewStages.length; i++) {
        let stageName = Object.keys(viewPipeline[i])[0];
        assert(idLookupViewStages[i].hasOwnProperty(stageName));
    }
}

function assertToplevelAggContainsView(explainStages, viewPipeline) {
    for (let i = 0; i < viewPipeline.length; i++) {
        let stageName = Object.keys(viewPipeline[i])[0];
        assert(explainStages[i].hasOwnProperty(stageName));
    }
}

/**
 * If the top-level aggregation contains a mongot stage, it asserts that the view transforms are
 * contained in _idLookup's subpipeline.  If the top-level aggregation doesn't have a mongot stage,
 * it asserts that the view stages were applied to the beginning of the top-level pipeline.
 * @param {Array} explainStages The list of stages returned from explain().
 * @param {Array} userPipeline The request/query that was run on the view.
 * @param {Object} viewPipeline The pipeline used to define the view.
 */
export function assertViewAppliedCorrectly(explainStages, userPipeline, viewPipeline) {
    if (userPipeline[0].hasOwnProperty("$search") ||
        userPipeline[0].hasOwnProperty("$vectorSearch")) {
        return assertIdLookupContainsViewPipeline(explainStages, viewPipeline);
    }
    return assertToplevelAggContainsView(explainStages, viewPipeline);
}