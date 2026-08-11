/**
 * A table of view definitions spanning the shapes that matter for extension/view interaction.
 * Every definition preserves `name` and `joinKey`, which lets one table drive $lookup (both
 * syntaxes), $unionWith, and $graphLookup from a single set of expectations. Each entry declares
 * either `expectedNames` (the exact sorted `name` values) or, where output is order-dependent,
 * `expectedCount` alone.
 */

// Foreign documents. All share joinKey "K" so every operator's joinKey-based match sees whatever
// the view yields.
export const FOREIGN_DOCS = [
    {_id: 1, name: "a", joinKey: "K", val: 10},
    {_id: 2, name: "b", joinKey: "K", val: 20},
    {_id: 3, name: "c", joinKey: "K", val: 30},
    {_id: 4, name: "d", joinKey: "K", val: 40},
];

// Local documents. The single doc's `key` matches every foreign doc's joinKey.
export const LOCAL_DOCS = [{_id: 100, key: "K"}];

// A small side collection so one view definition can contain a plain $lookup.
export const SIDE_DOCS = [
    {_id: 1, name: "a", tag: "side-a"},
    {_id: 2, name: "b", tag: "side-b"},
    {_id: 3, name: "c", tag: "side-c"},
    {_id: 4, name: "d", tag: "side-d"},
];

/**
 * Returns the view-definition table. `sideCollName` is the name of the collection holding
 * SIDE_DOCS, needed by the definition that contains a $lookup.
 */
export function getViewDefs(sideCollName) {
    return [
        {
            label: "no extension",
            pipeline: [{$addFields: {fromView: true}}],
            expectedNames: ["a", "b", "c", "d"],
        },
        {
            label: "non-desugaring extension only",
            pipeline: [{$testBar: {noop: true}}],
            expectedNames: ["a", "b", "c", "d"],
        },
        {
            label: "desugaring extension only",
            pipeline: [{$matchTopN: {filter: {val: {$gte: 20}}, sort: {val: 1}, limit: 2}}],
            expectedNames: ["b", "c"],
        },
        {
            label: "non-desugaring then desugaring then plain",
            pipeline: [
                {$testBar: {noop: true}},
                {$matchTopN: {filter: {}, sort: {val: 1}, limit: 3}},
                {$addFields: {mixed: true}},
            ],
            expectedNames: ["a", "b", "c"],
        },
        {
            label: "desugaring then non-desugaring",
            pipeline: [
                {$matchTopN: {filter: {val: {$lte: 30}}, sort: {val: 1}, limit: 3}},
                {$testBar: {noop: true}},
            ],
            expectedNames: ["a", "b", "c"],
        },
        {
            label: "two desugaring extensions in sequence",
            pipeline: [
                {$matchTopN: {filter: {val: {$gte: 10}}, sort: {val: 1}, limit: 3}},
                {$matchTopN: {filter: {val: {$gte: 20}}, sort: {val: 1}, limit: 2}},
            ],
            expectedNames: ["b", "c"],
        },
        {
            label: "transform extension",
            pipeline: [{$sort: {_id: 1}}, {$extensionLimit: 2}],
            expectedNames: ["a", "b"],
        },
        {
            label: "source extension",
            // $readNDocuments desugars to [$produceIds, $_internalSearchIdLookup], emitting _ids
            // [0, numDocs) and fetching those that exist. numDocs:3 therefore yields "a" and "b".
            //
            // Known 'lookupGap': $lookup-on-a-view prepends a foreign source that displaces
            // $produceIds from position 0, failing with 40602 for both $lookup syntaxes on a
            // standalone or replica set. On a sharded cluster, pipeline syntax resolves the view
            // correctly but localField/foreignField silently yields no documents (TODO
            // SERVER-133203). $unionWith and $graphLookup always succeed.
            lookupFailsWithCode: 40602,
            pipeline: [{$readNDocuments: {numDocs: 3}}],
            expectedNames: ["a", "b"],
        },
        {
            label: "kDoNothing view-policy desugarer",
            pipeline: [{$desugarAddViewName: {}}],
            expectedNames: ["a", "b", "c", "d"],
        },
        {
            label: "server-side (non-extension) desugarer",
            // $setWindowFields desugars server-side and is result-preserving. This is the shape
            // implicated in SERVER-133203.
            pipeline: [
                {
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {rank: {$denseRank: {}}},
                    },
                },
            ],
            expectedNames: ["a", "b", "c", "d"],
        },
        {
            label: "server desugarer combined with extension desugarer",
            pipeline: [
                {$setWindowFields: {sortBy: {_id: 1}, output: {rank: {$denseRank: {}}}}},
                {$matchTopN: {filter: {val: {$lte: 20}}, sort: {val: 1}, limit: 2}},
            ],
            expectedNames: ["a", "b"],
        },
        {
            label: "view definition containing a plain $lookup",
            pipeline: [
                {
                    $lookup: {
                        from: sideCollName,
                        localField: "name",
                        foreignField: "name",
                        as: "side",
                    },
                },
            ],
            expectedNames: ["a", "b", "c", "d"],
        },
        {
            label: "view definition containing a $lookup with an extension in its subpipeline",
            pipeline: [
                {
                    $lookup: {
                        from: sideCollName,
                        pipeline: [{$testBar: {inner: true}}],
                        as: "side",
                    },
                },
                {$matchTopN: {filter: {}, sort: {val: 1}, limit: 3}},
            ],
            expectedNames: ["a", "b", "c"],
        },
        {
            label: "view definition containing a $unionWith with an extension in its subpipeline",
            // The unioned side docs have no joinKey, so they are invisible to every operator's
            // joinKey-based match and the expectations stay the foreign docs.
            pipeline: [{$unionWith: {coll: sideCollName, pipeline: [{$testBar: {inner: true}}]}}],
            expectedNames: ["a", "b", "c", "d"],
        },
        // Explicitly-sized view definitions carrying a 'marker' field, so a dropped view pipeline
        // is caught directly rather than inferred from document counts.
        {
            label: "2-stage view: non-desugaring ext then plain",
            pipeline: [{$testBar: {noop: true}}, {$addFields: {v1: true}}],
            expectedNames: ["a", "b", "c", "d"],
            marker: "v1",
        },
        {
            label: "3-stage view: desugaring ext, non-desugaring ext, plain",
            // limit: 4 is >= the doc count, so this stays result-preserving.
            pipeline: [
                {$matchTopN: {filter: {}, sort: {val: 1}, limit: 4}},
                {$testBar: {noop: true}},
                {$addFields: {v2: true}},
            ],
            expectedNames: ["a", "b", "c", "d"],
            marker: "v2",
        },
    ];
}

/**
 * User subpipelines of varying size, for the operators that accept one ($lookup pipeline syntax and
 * $unionWith). All are result-preserving and keep 'name' and 'joinKey', so every view definition's
 * 'expectedNames' stays valid at every size. They differ in what stage comes FIRST, since that
 * determines the view-application policy.
 */
export function getUserPipelines() {
    return [
        {label: "0-stage", pipeline: [], marker: null},
        {
            // Desugaring extension first, so an expanded stage makes the policy decision. The limit
            // must exceed the largest view output — which is more than the foreign collection's
            // size, since the $unionWith definitions emit twice as many docs — to stay
            // result-preserving.
            label: "2-stage",
            pipeline: [
                {$matchTopN: {filter: {}, sort: {val: 1}, limit: 100}},
                {$addFields: {u1: true}},
            ],
            marker: "u1",
        },
        {
            // Plain stage first, non-desugaring extension in the middle, $sort last.
            label: "3-stage",
            pipeline: [{$addFields: {u1: true}}, {$testBar: {mid: true}}, {$sort: {_id: 1}}],
            marker: "u1",
        },
    ];
}
