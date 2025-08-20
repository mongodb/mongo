/**
 * Test that references to "scoreDetails" are correctly validated.
 *
 * featureFlagRankFusionFull is required to enable use of "scoreDetails".
 * @tags: [
 *   featureFlagRankFusionFull,
 *  ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, a: 3},
    {_id: 1, a: 4, mood: "content hippo"},
    {_id: 2, a: 5, mood: "very hungry hippo"}
]));
createSearchIndex(coll, {name: "search-index", definition: {"mappings": {"dynamic": true}}});

const kUnavailableMetadataErrCode = 40218;

const searchStageWithDetails = {
    $search:
        {index: "search-index", text: {query: "hungry hippo", path: ["mood"]}, scoreDetails: true}
};
const searchStageNoDetails = {
    $search: {
        index: "search-index",
        text: {query: "hungry hippo", path: ["mood"]},
    }
};
const metaProjectScoreDetailsStage = {
    $project: {scoreDetails: {$meta: "scoreDetails"}}
};
const matchStage = {
    $match: {a: {$gt: 0}}
};
const skipStage = {
    $skip: 1
};
const limitStage = {
    $limit: 2
};
const groupStage = {
    $group: {_id: null, myField: {$max: "$a"}}
};

// Project'ing "scoreDetails" immediately after $search works.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [searchStageWithDetails, metaProjectScoreDetailsStage],
    cursor: {}
}));

// Project'ing "scoreDetails" many stages after $search works.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline:
        [searchStageWithDetails, matchStage, skipStage, limitStage, metaProjectScoreDetailsStage],
    cursor: {}
}));

// Project'ing "scoreDetails" after a $group is rejected since $group drops the per-document
// metadata.
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [searchStageWithDetails, groupStage, metaProjectScoreDetailsStage],
    cursor: {}
}),
                             kUnavailableMetadataErrCode);

// Project'ing "scoreDetails" when it is not generated is rejected.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [searchStageNoDetails, metaProjectScoreDetailsStage],
    cursor: {}
}),
                             kUnavailableMetadataErrCode);

dropSearchIndex(coll, {name: "search-index"});
