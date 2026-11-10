/**
 * Internal-only aggregation stages and fields must be rejected in a view definition.
 *
 * The fields covered here are set by the server when it rewrites a pipeline. A client must never be able to supply
 * one.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const viewOn = "coll";
const rejectedViewName = "rejectedView";

// Error code changes based on topology.
const kRejectionCodes = [5491300, ErrorCodes.OptionNotSupportedOnView];

const kInternalOnlyPipelines = [
    {
        name: "$sort's $_internalLimit",
        pipeline: [{$sort: {a: 1, $_internalLimit: 5}}],
    },
    {
        name: "$sort's $_internalOutputSortKeyMetadata",
        pipeline: [{$sort: {a: 1, $_internalOutputSortKeyMetadata: true}}],
    },
    {
        name: "$lookup's $_internalFromIsAView",
        pipeline: [
            {
                $lookup: {
                    from: viewOn,
                    localField: "a",
                    foreignField: "b",
                    as: "joined",
                    $_internalFromIsAView: true,
                },
            },
        ],
    },
    {
        name: "$lookup's $_internalFieldMatchPipelineIdx",
        pipeline: [
            {
                $lookup: {
                    from: viewOn,
                    localField: "a",
                    foreignField: "b",
                    as: "joined",
                    $_internalFieldMatchPipelineIdx: NumberLong(0),
                },
            },
        ],
    },
    {
        name: "$lookup's $_internalIsHybridSearch",
        pipeline: [
            {
                $lookup: {
                    from: viewOn,
                    localField: "a",
                    foreignField: "b",
                    as: "joined",
                    $_internalIsHybridSearch: true,
                },
            },
        ],
    },
    {
        name: "$lookup's $_internalUnwind",
        pipeline: [
            {
                $lookup: {
                    from: viewOn,
                    localField: "a",
                    foreignField: "b",
                    as: "joined",
                    $_internalUnwind: {$unwind: "$joined"},
                },
            },
        ],
    },
    {
        name: "$unionWith's $_internalIsHybridSearch",
        pipeline: [
            {
                $unionWith: {
                    coll: viewOn,
                    pipeline: [],
                    $_internalIsHybridSearch: true,
                },
            },
        ],
    },
    {
        name: "$graphLookup's $_internalFromPipeline",
        pipeline: [
            {
                $graphLookup: {
                    from: viewOn,
                    startWith: "$connect",
                    connectFromField: "connect",
                    connectToField: "connect",
                    as: "g",
                    maxDepth: 0,
                    $_internalFromPipeline: [{$match: {a: 1}}],
                },
            },
        ],
    },
    {
        name: "an internal-only field inside a $lookup sub-pipeline",
        pipeline: [
            {
                $lookup: {
                    from: viewOn,
                    pipeline: [{$sort: {a: 1, $_internalLimit: 5}}],
                    as: "joined",
                },
            },
        ],
    },
    {
        name: "an internal-only field inside a $unionWith sub-pipeline",
        pipeline: [
            {
                $unionWith: {
                    coll: viewOn,
                    pipeline: [{$sort: {a: 1, $_internalLimit: 5}}],
                },
            },
        ],
    },
    {
        name: "an internal-only field inside a $graphLookup sub-pipeline",
        pipeline: [
            {
                $graphLookup: {
                    from: viewOn,
                    startWith: "$connect",
                    connectFromField: "connect",
                    connectToField: "connect",
                    as: "g",
                    maxDepth: 0,
                    $_internalFromPipeline: [{$sort: {a: 1, $_internalLimit: 5}}],
                },
            },
        ],
    },
    {
        name: "an internal-only field inside a $facet sub-pipeline",
        pipeline: [{$facet: {inner: [{$sort: {a: 1, $_internalLimit: 5}}]}}],
    },
];

const kSubPipelineWrappers = [
    {name: "$lookup", wrap: (p) => [{$lookup: {from: viewOn, pipeline: p, as: "wrapped"}}]},
    {name: "$facet", wrap: (p) => [{$facet: {wrapped: p}}]},
    {name: "$unionWith", wrap: (p) => [{$unionWith: {coll: viewOn, pipeline: p}}]},
];

const kAllInternalOnlyPipelines = [
    ...kInternalOnlyPipelines,
    ...kSubPipelineWrappers.flatMap(({name: wrapperName, wrap}) =>
        kInternalOnlyPipelines
            // $facet cannot be nested inside another $facet.
            .filter(({pipeline}) => !(wrapperName === "$facet" && "$facet" in pipeline[0]))
            .map(({name, pipeline}) => ({
                name: `${name} wrapped in ${wrapperName}`,
                pipeline: wrap(pipeline),
            })),
    ),
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
