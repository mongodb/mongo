import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    getDatasetModel,
    getDocModel,
} from "jstests/libs/property_test_helpers/models/document_models.js";
import {
    addFieldsConstArb,
    addFieldsVarArb,
    getAggPipelineArb,
    getQueryAndOptionsModel,
    getTrySbeRestrictedPushdownEligibleAggPipelineArb,
    getTrySbeEnginePushdownEligibleAggPipelineArb,
    getSbeFullPushdownEligibleAggPipelineArb,
    getSortArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {
    simpleProjectArb,
    multipleFieldProjectArb,
    computedProjectArb,
} from "jstests/libs/property_test_helpers/models/project_models.js";
import {
    getMatchArb,
    getMatchPredicateSpec,
} from "jstests/libs/property_test_helpers/models/match_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";

export function createStabilityWorkload(numQueriesPerRun) {
    // TODO SERVER-108077: when this ticket is complete, remove filters and allow ORs.
    // TODO SERVER-119019: $elemMatch is not supported by histogramCE (error code 9808601).
    const aggModel = getQueryAndOptionsModel({allowOrs: false, deterministicBag: false}).filter(
        (q) => {
            const asStr = JSON.stringify(q);
            // The query cannot contain any of these strings, as they are linked to the issues above.
            return ["$not", "$exists", "array", "$elemMatch"].every(
                (expr) => !asStr.includes(expr),
            );
        },
    );

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

export function addFieldsFirstStageAggModel({isTS = false} = {}) {
    return fc
        .record({
            addFieldsStage: fc.oneof(addFieldsConstArb, addFieldsVarArb),
            restOfPipeline: getAggPipelineArb({isTS: isTS}),
        })
        .map(({addFieldsStage, restOfPipeline}) => {
            return {"pipeline": [addFieldsStage, ...restOfPipeline], "options": {}};
        });
}

export function matchFirstStageAggModel({isTS = false} = {}) {
    return fc
        .record({
            matchStage: getMatchArb(),
            restOfPipeline: getAggPipelineArb({isTS: isTS}),
        })
        .map(({matchStage, restOfPipeline}) => {
            return {"pipeline": [matchStage, ...restOfPipeline], "options": {}};
        });
}

export function groupThenMatchAggModel({isTS = false} = {}) {
    return fc
        .record({
            matchStage: getMatchArb(),
            groupStage: groupArb,
        })
        .map(({matchStage, groupStage}) => {
            return {"pipeline": [groupStage, matchStage], "options": {}};
        });
}

export function trySbeRestrictedPushdownEligibleAggModel(foreignName, {isTS = false} = {}) {
    const pipelineArb = getTrySbeRestrictedPushdownEligibleAggPipelineArb(foreignName, {isTS});
    return pipelineArb.map((pipeline) => ({pipeline, "options": {}}));
}

export function trySbeEnginePushdownEligibleAggModel(foreignName, {isTS = false} = {}) {
    const pipelineArb = getTrySbeEnginePushdownEligibleAggPipelineArb(foreignName, {isTS});
    return pipelineArb.map((pipeline) => ({pipeline, "options": {}}));
}

export function sbeFullPushdownEligibleAggModel(foreignName, {isTS = false} = {}) {
    const pipelineArb = getSbeFullPushdownEligibleAggPipelineArb(foreignName, {isTS});
    return pipelineArb.map((pipeline) => ({pipeline, "options": {}}));
}

export function projectFirstStageAggModel({isTS = false} = {}) {
    return fc
        .record({
            projectStage: fc.oneof(simpleProjectArb, multipleFieldProjectArb, computedProjectArb),
            restOfPipeline: getAggPipelineArb({isTS: isTS}),
        })
        .map(({projectStage, restOfPipeline}) => {
            return {"pipeline": [projectStage, ...restOfPipeline], "options": {}};
        });
}

// Creates a model for {$match: {$or: ...}} expressions.
export function topLevelOrAggModel() {
    const matchWithTopLevelOrArb = getMatchPredicateSpec()
        .singleCompoundPredicate.filter((pred) => {
            // This filter will pass 1/3rd of the time. Since generating
            // queries is quick, this isn't a concern.
            return Object.keys(pred).includes("$or");
        })
        .map((pred) => {
            return {$match: pred};
        });

    const aggModel = fc
        .record({
            orMatch: matchWithTopLevelOrArb,
            query: getQueryAndOptionsModel(),
        })
        .map(({orMatch, query}) => {
            return {
                "pipeline": [orMatch, ...query.pipeline],
                "options": query.options,
            };
        });

    return aggModel;
}

// Creates a model where all stages are eligible to be pushed to the find layer.
export function findLayerOnlyPipeline() {
    // Limit/skip are not included so results are deterministic.
    const findLayerStageArb = oneof(
        simpleProjectArb,
        multipleFieldProjectArb,
        getMatchArb(),
        getSortArb(),
    );
    return fc.array(findLayerStageArb, {minLength: 0, maxLength: 4});
}
