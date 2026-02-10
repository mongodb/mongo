import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getDatasetModel, getDocModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {
    addFieldsConstArb,
    addFieldsVarArb,
    computedProjectArb,
    simpleProjectArb,
    getAggPipelineArb,
    getQueryAndOptionsModel,
    getTrySbeRestrictedPushdownEligibleAggPipelineArb,
    getTrySbeEnginePushdownEligibleAggPipelineArb,
    getSbeFullPushdownEligibleAggPipelineArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getNestedProperties} from "jstests/libs/query/analyze_plan.js";

export function createStabilityWorkload(numQueriesPerRun) {
    // TODO SERVER-108077: when this ticket is complete, remove filters and allow ORs.
    // TODO SERVER-119019: $elemMatch is not supported by histogramCE (error code 9808601).
    const aggModel = getQueryAndOptionsModel({allowOrs: false, deterministicBag: false}).filter((q) => {
        const asStr = JSON.stringify(q);
        // The query cannot contain any of these strings, as they are linked to the issues above.
        return ["$not", "$exists", "array", "$elemMatch"].every((expr) => !asStr.includes(expr));
    });

    return makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getDatasetModel(
                // TODO SERVER-100515 reenable unicode.
                {
                    maxNumDocs: 2000,
                    docModel: getDocModel({allowUnicode: false, allowNullBytes: false}),
                },
            ),
        }),
        aggModel,
        numQueriesPerRun,
        // Include one extra param representing the number of buckets in the analyze command.
        // Use 5 as the minimum to avoid an error about the number of buckets needing to be at least
        // the number of types in the dataset.
        extraParamsModel: fc.record({numberBuckets: fc.integer({min: 5, max: 2000})}),
    });
}

export function addFieldsFirstStageAggModel({isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        addFieldsStage: fc.oneof(addFieldsConstArb, addFieldsVarArb),
        restOfPipeline: getAggPipelineArb({isTS: isTS}),
    });

    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({_, restOfPipeline}) => getNestedProperties(restOfPipeline, "$elemMatch").length == 0);
    }

    return aggArb.map(({addFieldsStage, restOfPipeline}) => {
        return {"pipeline": [addFieldsStage, ...restOfPipeline], "options": {}};
    });
}

export function matchFirstStageAggModel({isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        matchStage: getMatchArb(),
        restOfPipeline: getAggPipelineArb({isTS: isTS}),
    });

    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(
            ({matchStage, restOfPipeline}) =>
                getNestedProperties(matchStage, "$elemMatch").length == 0 &&
                getNestedProperties(restOfPipeline, "$elemMatch").length == 0,
        );
    }

    return aggArb.map(({matchStage, restOfPipeline}) => {
        return {"pipeline": [matchStage, ...restOfPipeline], "options": {}};
    });
}

export function groupThenMatchAggModel({isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        matchStage: getMatchArb(),
        groupStage: groupArb,
    });

    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({matchStage, groupStage}) => getNestedProperties(matchStage, "$elemMatch").length == 0);
    }

    return aggArb.map(({matchStage, groupStage}) => {
        return {"pipeline": [groupStage, matchStage], "options": {}};
    });
}

export function trySbeRestrictedPushdownEligibleAggModel(foreignName, {isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        pipeline: getTrySbeRestrictedPushdownEligibleAggPipelineArb(foreignName, {isTS: isTS}),
    });
    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({pipeline}) => getNestedProperties(pipeline, "$elemMatch").length == 0);
    }
    return aggArb.map(({pipeline}) => {
        return {pipeline, "options": {}};
    });
}

export function trySbeEnginePushdownEligibleAggModel(foreignName, {isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        pipeline: getTrySbeEnginePushdownEligibleAggPipelineArb(foreignName, {isTS: isTS}),
    });
    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({pipeline}) => getNestedProperties(pipeline, "$elemMatch").length == 0);
    }
    return aggArb.map(({pipeline}) => {
        return {pipeline, "options": {}};
    });
}

export function sbeFullPushdownEligibleAggModel(foreignName, {isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        pipeline: getSbeFullPushdownEligibleAggPipelineArb(foreignName, {isTS: isTS}),
    });
    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({pipeline}) => getNestedProperties(pipeline, "$elemMatch").length == 0);
    }
    return aggArb.map(({pipeline}) => {
        return {pipeline, "options": {}};
    });
}

export function projectFirstStageAggModel({isTS = false, is83orAbove = true} = {}) {
    let aggArb = fc.record({
        projectStage: fc.oneof(simpleProjectArb, computedProjectArb),
        restOfPipeline: getAggPipelineArb({isTS: isTS}),
    });

    // Older versions suffer from SERVER-101007
    // TODO SERVER-114269 remove this check.
    if (!is83orAbove) {
        aggArb = aggArb.filter(({_, restOfPipeline}) => getNestedProperties(restOfPipeline, "$elemMatch").length == 0);
    }

    return aggArb.map(({projectStage, restOfPipeline}) => {
        return {"pipeline": [projectStage, ...restOfPipeline], "options": {}};
    });
}
