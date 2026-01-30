/**
 * Test that $graphLookup can run on views containing extension stages, either directly in the
 * view definition or within a $unionWith subpipeline.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {describe, it} from "jstests/libs/mochalite.js";

function getSocialMediaUserData() {
    // Static, deterministic dataset with 8 users forming a connected graph.
    // Each user has at most 2 friends, with some having only 1 friend.
    // Friendships are bidirectional (1-1 relation).
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
    assert.gte(viewCount, expectedUsersInView, view.find().toArray());

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
    assert.sameMembers(results.connections.map((c) => c.name).sort(), expectedUsers.map((u) => u.name).sort(), results);

    assert.commandWorked(coll.getDB().runCommand({drop: fromViewName}));
}

describe("$graphLookup with extension stages in view definition", function () {
    it("should run $graphLookup on a view with a desugar/source stage in definition", function () {
        runGraphLookup(collName + "_read_n_docs_view", [{$readNDocuments: {numDocs: expectedUsersInView}}]);
    });

    it("should run $graphLookup on a view with a 'transform' stage in definition", function () {
        runGraphLookup(collName + "_extension_limit_view", [{$sort: {_id: 1}}, {$extensionLimit: expectedUsersInView}]);
    });

    it("should run $graphLookup on a nested view with extension stage in inner view definition", function () {
        const nestedViewName = collName + "_nested_extension_view";
        assert.commandWorked(
            db.createView(nestedViewName, collName, [{$readNDocuments: {numDocs: expectedUsersInView}}]),
        );
        runGraphLookup(collName + "_nested_view", [], nestedViewName);
        assert.commandWorked(coll.getDB().runCommand({drop: nestedViewName}));
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
            db.createView(nestedViewName, collName, [{$readNDocuments: {numDocs: expectedUsersInView}}]),
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
