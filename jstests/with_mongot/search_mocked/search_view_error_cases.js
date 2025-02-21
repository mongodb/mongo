/**
 * This test creates view pipelines containing various stages that involve unreported external
 * namespaces. mongot stages are then ran on these views to ensure that errors are correctly thrown
 * in such situations. The point of this test is to ensure we correctly validate Atlas Search
 * features on views before executing queries.
 *
 * Currently, mongot doesn't support indexing views that are not on a $match.expr or $addFields view
 * pipeline, meaning we must mock the behavior of mongot.
 * @tags: [ featureFlagMongotIndexedViews, featureFlagRankFusionBasic,
 * featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {mongotHost: mockConn.host}}});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");
const teamColl = assertDropAndRecreateCollection(db, `${jsTest.name()}_teams`);
const playerColl = assertDropAndRecreateCollection(db, `${jsTest.name()}_players`);

assert.commandWorked(teamColl.insertMany([{
    _id: 1,
    name: "Sacramento Kings",
}]));

assert.commandWorked(playerColl.insertMany([{
    _id: 4,
    teamId: 1,
    name: "De'Aaron Fox",
}]));

const runTest = function({name, pipeline, errorCode}) {
    assert.commandWorked(db.createView(name, teamColl.getName(), pipeline));
    const nestedView = `${name}_nested_view`;
    assert.commandWorked(db.createView(nestedView, name, [{$project: {_id: 0}}]));

    // Test each view configuration against $search, $vectorSearch, and $searchMeta.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: name, pipeline: [{$search: {}}], cursor: {}}), [errorCode]);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: name, pipeline: [{$vectorSearch: {}}], cursor: {}}), [errorCode]);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: name, pipeline: [{$searchMeta: {}}], cursor: {}}), [errorCode]);

    // Run a mongot query on a valid nested view to ensure failure on the invalid parent view.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: nestedView, pipeline: [{$search: {}}], cursor: {}}), [errorCode]);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: nestedView, pipeline: [{$vectorSearch: {}}], cursor: {}}),
        [errorCode]);
    assert.commandFailedWithCode(
        db.runCommand({aggregate: nestedView, pipeline: [{$searchMeta: {}}], cursor: {}}),
        [errorCode]);
};

runTest({
    name: "begins_with_lookup_view",
    pipeline: [
        {
            $lookup: {
                from: playerColl.getName(),
                localField: "_id",
                foreignField: "teamId",
                as: "players"
            }
        },
        { $match: {} }
    ],
    errorCode: 9475802
});

runTest({
    name: "contains_lookup_view",
    pipeline: [
        { $match: {} },
        {
            $lookup: {
                from: playerColl.getName(),
                localField: "_id",
                foreignField: "teamId",
                as: "players"
            }
        }
    ],
    errorCode: 9475802
});

runTest({
    name: "nested_lookup_view",
    pipeline: [
        { $match: {} },
        {
            $facet: {
                "pipeline0": [{$match: {}}],
                "pipeline1": [
                    { $match: {} },
                    {
                        $lookup: {
                            from: playerColl.getName(),
                            localField: "_id",
                            foreignField: "teamId",
                            as: "players"
                        }
                    }
                ]
            }
        }
    ],
    errorCode: 9475802
});

runTest({name: "search_view", pipeline: [{$search: {}}], errorCode: 9475801});

runTest({name: "vectorSearch_view", pipeline: [{$vectorSearch: {}}], errorCode: 9475801});

runTest({name: "searchMeta_view", pipeline: [{$searchMeta: {}}], errorCode: 9475801});

runTest({
    name: "begins_with_unionWith_view",
    pipeline: [{$unionWith: {coll: playerColl.getName(), pipeline: []}}],
    errorCode: 9475802
});

runTest({
    name: "contains_unionWith_view",
    pipeline: [{$match: {}}, {$unionWith: {coll: playerColl.getName(), pipeline: []}}],
    errorCode: 9475802
});

runTest({
    name: "nested_unionWith_view",
    pipeline: [
        {$match: {}},
        {
            $facet: {
                pipeline0: [{$match: {}}],
                pipeline1: [{$match: {}}, {$unionWith: {coll: playerColl.getName(), pipeline: []}}]
            }
        }
    ],
    errorCode: 9475802
});

runTest({
    name: "rankFusion_search_view",
    pipeline: [{$rankFusion: {input: {pipelines: {search: [{$search: {}}]}}}}],
    errorCode: 9475801
});

runTest({
    name: "rankFusion_vectorSearch_view",
    pipeline: [{$rankFusion: {input: {pipelines: {search: [{$vectorSearch: {}}]}}}}],
    errorCode: 9475801
});

runTest({
    name: "scoreFusion_search_view",
    pipeline:
        [{$scoreFusion: {input: {pipelines: {search1: [{$search: {}}]}, normalization: "none"}}}],
    errorCode: 9475801
});

runTest({
    name: "scoreFusion_vectorSearch_view",
    pipeline: [{
        $scoreFusion:
            {input: {pipelines: {vectorSearch1: [{$vectorSearch: {}}]}, normalization: "none"}}
    }],
    errorCode: 9475801
});

// Make sure the server is still running.
assert.commandWorked(db.runCommand("ping"));

mongotMock.stop();
rst.stopSet();
