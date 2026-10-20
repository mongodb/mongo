/**
 * $graphLookup's internal-only '$_internalFromPipeline' field must be rejected in a view definition and in a user
 * aggregation, including when it appears inside a sub-pipeline.
 *
 * The router sets this field when it rewrites a $graphLookup whose 'from' is a view. A client must never be able to
 * supply it.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const viewOn = "coll";
const rejectedViewName = "rejectedView";

// Error code changes based on topology.
const kRejectionCodes = [5491300, ErrorCodes.OptionNotSupportedOnView];

const kGraphLookupSpec = {
    from: viewOn,
    startWith: "$connect",
    connectFromField: "connect",
    connectToField: "connect",
    as: "g",
    maxDepth: 0,
};

const kSubPipelineWrappers = [
    {name: "$lookup", wrap: (p) => [{$lookup: {from: viewOn, pipeline: p, as: "wrapped"}}]},
    {name: "$facet", wrap: (p) => [{$facet: {wrapped: p}}]},
    {name: "$unionWith", wrap: (p) => [{$unionWith: {coll: viewOn, pipeline: p}}]},
];

function wrapInEachSubPipeline(cases) {
    return kSubPipelineWrappers.flatMap(({name: wrapperName, wrap}) =>
        cases
            // $facet cannot be nested inside another $facet.
            .filter(({pipeline}) => !(wrapperName === "$facet" && "$facet" in pipeline[0]))
            .map(({name, pipeline}) => ({
                name: `${name} wrapped in ${wrapperName}`,
                pipeline: wrap(pipeline),
            })),
    );
}

const kTopLevelCase = {
    name: "$graphLookup's $_internalFromPipeline",
    pipeline: [{$graphLookup: {...kGraphLookupSpec, $_internalFromPipeline: [{$match: {a: 1}}]}}],
};
const kNestedOnceCases = wrapInEachSubPipeline([kTopLevelCase]);
const kAllInternalOnlyPipelines = [
    kTopLevelCase,
    ...kNestedOnceCases,
    ...wrapInEachSubPipeline(kNestedOnceCases),
];

describe("internal-only fields in a view definition", function () {
    before(function () {
        assert.commandWorked(testDB.dropDatabase());
        assert.commandWorked(testDB[viewOn].insert({_id: 1, a: 1, b: 1, connect: 1}));
    });

    after(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    for (const {name, pipeline} of kAllInternalOnlyPipelines) {
        it(`rejects a view definition containing ${name}`, function () {
            assert.commandFailedWithCode(
                testDB.runCommand({create: rejectedViewName, viewOn, pipeline}),
                kRejectionCodes,
                `view definition containing ${name} was accepted`,
            );
            assert(
                !testDB.getCollectionNames().includes(rejectedViewName),
                `view definition containing ${name} was stored`,
                {name},
            );
        });
    }
});

describe("internal-only fields in a user aggregation", function () {
    before(function () {
        assert.commandWorked(testDB.dropDatabase());
        assert.commandWorked(testDB[viewOn].insert({_id: 1, a: 1, b: 1, connect: 1}));
    });

    after(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    for (const {name, pipeline} of kAllInternalOnlyPipelines) {
        it(`rejects an aggregation containing ${name}`, function () {
            assert.commandFailedWithCode(
                testDB.runCommand({aggregate: viewOn, pipeline, cursor: {}}),
                5491300,
                `aggregation containing ${name} was accepted`,
            );
        });
    }
});
