/**
 * Test that $graphLookup can run on views containing extension stages, either directly in the
 * view definition or within a $unionWith subpipeline.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";

function getSocialMediaUserData() {
    // Static, deterministic dataset of 8 users forming a connected graph, with bidirectional
    // friendships and at most 2 friends each.
    return [
        {_id: 0, name: "Alice", friends: ["Bob", "Charlie"]},
        {_id: 1, name: "Bob", friends: ["Alice", "Henry"]},
        {_id: 2, name: "Charlie", friends: ["Alice", "Eve"]},
        {_id: 3, name: "Diana", friends: ["Eve", "Frank"]},
        {_id: 4, name: "Eve", friends: ["Charlie", "Diana"]},
        {_id: 5, name: "Frank", friends: ["Diana", "Grace"]},
        {_id: 6, name: "Grace", friends: ["Frank"]},
        {_id: 7, name: "Henry", friends: ["Bob"]},
    ];
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const users = getSocialMediaUserData();
const numUsers = users.length;
// We'll exclude Henry (user 7) from the view, so we expect numUsers - 1 users.
const expectedUsersInView = numUsers - 1;
coll.insertMany(users);

function runGraphLookup(fromViewName, fromViewPipeline, source = collName) {
    assert.commandWorked(db.createView(fromViewName, source, fromViewPipeline));

    // Sanity check to make sure the view returns what we expect.
    const view = db[fromViewName];
    const viewCount = view.count();
    assert.eq(
        viewCount,
        expectedUsersInView,
        `view should return exactly ${expectedUsersInView} users`,
        view.find().toArray(),
    );

    const results = coll
        .aggregate([
            {$limit: 1},
            {
                $graphLookup: {
                    from: fromViewName,
                    startWith: "Grace",
                    connectFromField: "friends",
                    connectToField: "name",
                    as: "connections",
                },
            },
        ])
        .toArray()[0];

    // It should retrieve users from the view.
    assert.eq(results.connections.length, expectedUsersInView, results);

    // Verify the results are the same members as the users minus Henry.
    const expectedUsers = users.filter((u) => u._id !== 7);
    assert.sameMembers(
        results.connections.map((c) => c.name).sort(),
        expectedUsers.map((u) => u.name).sort(),
        results,
    );

    assert.commandWorked(coll.getDB().runCommand({drop: fromViewName}));
}

describe("$graphLookup with extension stages in view definition", function () {
    it("should run $graphLookup on a view with a desugar/source stage in definition", function () {
        runGraphLookup(collName + "_read_n_docs_view", [
            {$readNDocuments: {numDocs: expectedUsersInView}},
        ]);
    });

    it("should run $graphLookup on a view with a 'transform' stage in definition", function () {
        runGraphLookup(collName + "_extension_limit_view", [
            {$sort: {_id: 1}},
            {$extensionLimit: expectedUsersInView},
        ]);
    });

    it("should run $graphLookup on a nested view with extension stage in inner view definition", function () {
        const nestedViewName = collName + "_nested_extension_view";
        assert.commandWorked(
            db.createView(nestedViewName, collName, [
                {$readNDocuments: {numDocs: expectedUsersInView}},
            ]),
        );
        runGraphLookup(collName + "_nested_view", [], nestedViewName);
        assert.commandWorked(coll.getDB().runCommand({drop: nestedViewName}));
    });

    it("should run $graphLookup on a view whose definition starts with a kDoNothing desugaring extension", function () {
        // $desugarAddViewName expands to a kDoNothing first stage. In a view *definition* there is
        // no outer view to bind to, so no viewName is stamped and the traversal still sees every
        // document.
        const viewName = collName + "_graph_donothing_view";
        assert.commandWorked(db.createView(viewName, collName, [{$desugarAddViewName: {}}]));
        try {
            const results = coll
                .aggregate([
                    {$limit: 1},
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "Grace",
                            connectFromField: "friends",
                            connectToField: "name",
                            as: "connections",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 1, results);
            assert.eq(
                results[0].connections.length,
                numUsers,
                "traversal should reach all users through the view",
                results,
            );
            for (const doc of results[0].connections) {
                assert(
                    !doc.hasOwnProperty("viewName") || doc.viewName === "",
                    `a desugarer in a view definition must not stamp viewName: ${tojson(doc)}`,
                    results,
                );
            }
        } finally {
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    });
});

describe("$graphLookup with $unionWith and extension stages", function () {
    it("should run $graphLookup on a view with a desugar/source stage in subpipeline", function () {
        runGraphLookup(collName + "_union_with_read_n_docs_view", [
            // Skip all users to only return the $unionWith results.
            {$skip: numUsers},
            {
                $unionWith: {
                    coll: collName,
                    pipeline: [{$readNDocuments: {numDocs: expectedUsersInView}}],
                },
            },
        ]);
    });

    it("should run $graphLookup on a view with a 'transform' stage in subpipeline", function () {
        runGraphLookup(collName + "_union_with_extension_limit_view", [
            // Skip all users to only return the $unionWith results.
            {$skip: numUsers},
            {
                $unionWith: {
                    coll: collName,
                    pipeline: [{$sort: {_id: 1}}, {$extensionLimit: expectedUsersInView}],
                },
            },
        ]);
    });

    it("should run $graphLookup on a nested view with extension stage in subpipeline", function () {
        const nestedViewName = collName + "_nested_extension_view";
        assert.commandWorked(
            db.createView(nestedViewName, collName, [
                {$readNDocuments: {numDocs: expectedUsersInView}},
            ]),
        );
        runGraphLookup(collName + "_union_with_nested_view", [
            // Skip all users to only return the $unionWith results.
            {$skip: numUsers},
            {
                $unionWith: {
                    coll: nestedViewName,
                    // Empty pipeline to only get results from the view.
                    pipeline: [],
                },
            },
        ]);
        assert.commandWorked(coll.getDB().runCommand({drop: nestedViewName}));
    });
});

describe("$graphLookup with extensions in the outer pipeline and restrictSearchWithMatch", function () {
    it("should run an extension stage in the outer pipeline alongside $graphLookup on an extension view", function () {
        const viewName = collName + "_outer_ext_view";
        assert.commandWorked(
            db.createView(viewName, collName, [{$readNDocuments: {numDocs: expectedUsersInView}}]),
        );
        try {
            const results = coll
                .aggregate([
                    {$limit: 1},
                    {$testBar: {outer: 1}},
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "Grace",
                            connectFromField: "friends",
                            connectToField: "name",
                            as: "connections",
                        },
                    },
                    {$testBar: {alsoOuter: 1}},
                ])
                .toArray();
            assert.eq(results.length, 1, results);
            assert.eq(results[0].connections.length, expectedUsersInView, results);
        } finally {
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    });

    it("should apply restrictSearchWithMatch when $graphLookup targets a desugaring-extension view", function () {
        // The view desugars $matchTopN, then restrictSearchWithMatch filters the traversal down to
        // Grace alone, so it cannot expand past her.
        const viewName = collName + "_restrict_view";
        assert.commandWorked(
            db.createView(viewName, collName, [
                {$matchTopN: {filter: {}, sort: {_id: 1}, limit: numUsers}},
            ]),
        );
        try {
            const results = coll
                .aggregate([
                    {$limit: 1},
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "Grace",
                            connectFromField: "friends",
                            connectToField: "name",
                            as: "connections",
                            restrictSearchWithMatch: {name: "Grace"},
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 1, results);
            assert.eq(
                results[0].connections.length,
                1,
                "restrictSearchWithMatch should admit only Grace",
                results,
            );
            assert.eq(results[0].connections[0].name, "Grace", results);
        } finally {
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    });

    // The allowedInLookup constraint is enforced only in DocumentSourceLookup and LiteParsedLookup;
    // $graphLookup has no equivalent check, so allowedInLookup=false in a targeted view's
    // definition is NOT rejected. These two cases pin that asymmetry down.
    it("should allow $graphLookup on a view whose definition holds a stage with allowedInLookup=false", function () {
        const viewName = collName + "_graph_allowed_view";
        assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
        try {
            const results = coll
                .aggregate([
                    {$limit: 1},
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "Grace",
                            connectFromField: "friends",
                            connectToField: "name",
                            as: "connections",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 1, results);
            // $testFoo is a no-op passthrough, so the traversal reaches every user.
            assert.eq(
                results[0].connections.length,
                numUsers,
                "allowedInLookup does not constrain $graphLookup, so the full traversal is returned",
                results,
            );
        } finally {
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    });

    it("should allow $graphLookup on a view whose definition holds a desugar-into-disallowed stage", function () {
        // $desugarFoo expands into $testFoo (allowedInLookup=false), which $graphLookup likewise
        // does not constrain.
        const viewName = collName + "_graph_desugar_allowed_view";
        assert.commandWorked(db.createView(viewName, collName, [{$desugarFoo: {}}]));
        try {
            const results = coll
                .aggregate([
                    {$limit: 1},
                    {
                        $graphLookup: {
                            from: viewName,
                            startWith: "Grace",
                            connectFromField: "friends",
                            connectToField: "name",
                            as: "connections",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 1, results);
            assert.eq(
                results[0].connections.length,
                numUsers,
                "desugared allowedInLookup=false stage is likewise unconstrained under $graphLookup",
                results,
            );
        } finally {
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    });
});
