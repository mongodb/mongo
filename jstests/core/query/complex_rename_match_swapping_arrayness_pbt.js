/**
 * A property-based test that enables "featureFlagImprovedDepsAnalysis" and asserts correctness
 * of pushing down $match on a complex rename when there is multikeyness metadata that proves
 * there are no arrays on the renamed path. Correctness is asserted by running the generated query
 * with and without optimizations.
 *
 * @tags: [
 * query_intensive_pbt,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * featureFlagImprovedDepsAnalysis,
 * featureFlagPathArrayness,
 * # Time series collections do not support indexing array values in measurement fields.
 * exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {
    assignableFieldArb,
    dottedDollarFieldArb,
    dottedFieldArb,
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getComputedProjectArb} from "jstests/libs/property_test_helpers/models/project_models.js";
import {getAddFieldsVarArb} from "jstests/libs/property_test_helpers/models/query_models.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 75;
const numQueriesPerRun = 50;

const controlColl = db.complex_rename_arrayness_pbt_control;
const experimentColl = db.complex_rename_arrayness_pbt_experiment;

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl);

// We don't want the $match to be too selective because empty results don't help in detecting an
// invalid pushdown. Use the same small scalar set in the $set stage and the document model so
// that $set-introduced arrays frequently contain values the $match is looking for.
const scalarArb = fc.constantFrom("equal", "non-equal");
const leafArb = fc.oneof(
    {arbitrary: scalarArb, weight: 3},
    {arbitrary: fc.array(scalarArb, {minLength: 1, maxLength: 3}), weight: 1},
);

// {$project: {[dest]: "$src"}} using dotted paths on both sides.
const projectRenameArb = getComputedProjectArb(dottedFieldArb, dottedDollarFieldArb);

// {$addFields: {[dest]: "$src"}} using dotted paths on both sides.
const addFieldsRenameArb = getAddFieldsVarArb(dottedFieldArb, dottedDollarFieldArb);

// {$set: {[field]: <array>}}.
const setFieldArb = fc.oneof({arbitrary: assignableFieldArb, weight: 3}, {arbitrary: dottedFieldArb, weight: 1});
const literalArraySetArb = fc
    .tuple(
        setFieldArb,
        fc.array(
            fc.dictionary(assignableFieldArb, fc.oneof(scalarArb, fc.array(scalarArb, {minLength: 1, maxLength: 2})), {
                minKeys: 1,
                maxKeys: 3,
            }),
            {minLength: 1, maxLength: 3},
        ),
    )
    .map(([field, arr]) => ({$set: {[field]: arr}}));

// The reshaping done by a complex rename affects $eq, so it's redundant to model a larger subset
// of MatchExpressions.
const dottedMatchArb = fc.record({field: dottedFieldArb}).map(({field}) => ({$match: {[field]: {$eq: "equal"}}}));
// The top-level field is never an array, but subfields might be.
const nestedObjArb = fc.record({a: leafArb, b: leafArb, t: leafArb, m: leafArb});
const docModel = fc.record({
    a: nestedObjArb,
    b: nestedObjArb,
    m: nestedObjArb,
    t: nestedObjArb,
    array: fc.array(nestedObjArb, {minLength: 0, maxLength: 3}),
});

// Only single-field indexes to avoid CannotIndexParallelArrays errors.
const dottedIndexDefArb = fc
    .record({field: dottedFieldArb.filter((f) => f !== "_id"), dir: fc.constantFrom(1, -1)})
    .map(({field, dir}) => ({[field]: dir}));
const indexModel = fc.record({def: dottedIndexDefArb, options: fc.constant({})});

function getWorkloadModelForArraynessComplexRenameMatchSwap() {
    const stageArb = fc.oneof(
        {arbitrary: literalArraySetArb, weight: 3},
        {arbitrary: addFieldsRenameArb, weight: 3},
        {arbitrary: projectRenameArb, weight: 1},
    );
    const aggModel = fc
        .tuple(fc.array(stageArb, {minLength: 1, maxLength: 5}), dottedMatchArb)
        .map(([stages, match]) => ({pipeline: [...stages, match], options: {}}));
    const docsModel = getDatasetModel({docModel});

    // Always include indexes on subpaths of all base fields so PathArrayness API has non-multikey
    // metadata for all base field prefixes.
    const guaranteedIndexes = fc.constant([
        {def: {"a.b": 1}, options: {}},
        {def: {"a.t": 1}, options: {}},
        {def: {"b.a": 1}, options: {}},
        {def: {"m.a": 1}, options: {}},
        {def: {"m.t": 1}, options: {}},
        {def: {"t.m": 1}, options: {}},
    ]);
    const extraIndexes = fc.array(indexModel, {minLength: 0, maxLength: 6});
    const indexesModel = fc.tuple(guaranteedIndexes, extraIndexes).map(([base, extra]) => [...base, ...extra]);

    return fc.record({
        collSpec: getCollectionModel({docsModel, indexesModel}),
        queries: fc.array(aggModel, {minLength: 1, maxLength: numQueriesPerRun}),
    });
}

testProperty(
    correctnessProperty,
    {controlColl, experimentColl},
    getWorkloadModelForArraynessComplexRenameMatchSwap(),
    numRuns,
);
