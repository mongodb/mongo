/*
 * Fast-check models for workloads. A workload is a collection model and an aggregation model.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

function typeCheckSingleAggModel(aggregation) {
    // Should be a list of objects.
    assert(Array.isArray(aggregation), 'Each aggregation pipeline should be an array.');
    for (const aggStage of aggregation) {
        assert.eq(typeof aggStage, 'object', 'Each aggregation stage should be an object.');
    }
}

// Sample once from the aggsModel to do some type checking. This can prevent accidentally passing
// models to the wrong parameters.
function typeCheckManyAggsModel(aggsModel) {
    const aggregations = fc.sample(aggsModel, {numRuns: 1})[0];
    // Should be a list of aggregation pipelines.
    assert(Array.isArray(aggregations), 'aggsModel should generate an array');
    assert.gt(aggregations.length, 0, 'aggsModel should generate a non-empty array');
    aggregations.forEach(agg => typeCheckSingleAggModel(agg));
}

/*
 * Creates a workload model from the given collection model and aggregation model.
 * Can be passed:
 *    - `aggsModel` which generates multiple aggregation pipelines at a time or
 *    - `aggModel` and `numQueriesPerRun` which will be used to create an `aggsModel`
 */
export function makeWorkloadModel({collModel, aggModel, aggsModel, numQueriesPerRun} = {}) {
    assert(!aggsModel || !aggModel, 'Cannot  specify both `aggsModel` and `aggModel`');
    assert(
        !aggsModel || !numQueriesPerRun,
        'Cannot specify `aggsModel` and `numQueriesPerRun`, since `numQueriesPerRun` is only used when provided `aggModel`.');
    if (aggModel) {
        aggsModel = fc.array(aggModel, {minLength: numQueriesPerRun, maxLength: numQueriesPerRun});
    }
    typeCheckManyAggsModel(aggsModel);
    return fc.record({collSpec: collModel, queries: aggsModel});
}
