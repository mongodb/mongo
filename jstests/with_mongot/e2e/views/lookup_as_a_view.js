/**
 * This test confirms mongod correctly resolves inner pipelines that are discovered during view
 * resolution. In this test, we create a lookup view where the foreign collection is another view
 * and the subpipeline contains $search.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const testDb = db.getSiblingDB(jsTestName());
const userColl = testDb.users;
const socialMediaPostsColl = testDb.posts;
userColl.drop();
socialMediaPostsColl.drop();

assert.commandWorked(userColl.insertMany([
    {_id: 1, username: "john_doe", email: "john@hotmail.com"},
    {_id: 2, username: "kelly_clarkson", email: "americanidol4ever@aol.com"}
]));

assert.commandWorked(socialMediaPostsColl.insertMany([
    {
        _id: 100,
        userId: 1,
        content: "Behind these hazel eyes is someone who wants to audition for American idol",
        likes: 6,
        comments: 10
    },
    {_id: 102, userId: 2, content: "I WON AMERICAN IDOL!", likes: 300, comments: 100},
    {_id: 103, userId: 2, content: "Did you see me on American Idol?", likes: 30, comments: 10},
    {_id: 104, userId: 1, content: "Anyone know where I put my keys", likes: 0, comments: 2},
]));

// Create an initial view that will be the inner collection for the subsequent $lookup view.
let viewName = "totalSocialMediaReactions";
let viewPipeline = [{"$addFields": {totalReactions: {$add: ["$likes", "$comments"]}}}];
assert.commandWorked(testDb.createView(viewName, socialMediaPostsColl.getName(), viewPipeline));
let totalSocialMediaReactionsView = testDb[viewName];
createSearchIndex(totalSocialMediaReactionsView,
                  {name: "totalReactionsIndex", definition: {"mappings": {"dynamic": true}}});

// This mongot query will retrieve all documents where the content field contains the word idol.
let searchQuery = {index: "totalReactionsIndex", text: {query: "idol", path: "content"}};

/**
 * This will view returns each user and their array of posts that meet the $search criteria. More
 * specifically, each element in the socialMediaPostsAboutIdol array should contain the world 'idol'
 * in its content field and disaply a totalReaction field from the $addFields view created on the
 * socialMediaPostsColl collection. collection.
 */
assert.commandWorked(testDb.createView("usersTotalPostsView", "users", [
    {
        $lookup: {
            from: totalSocialMediaReactionsView.getName(),
            localField: "_id",                // Field from users collection.
            foreignField: "userId",           // Field from totalSocialMediaReactions view.
            as: "socialMediaPostsAboutIdol",  // Name of the output array
            pipeline: [
                {$search: searchQuery},
            ],
        }
    },
]));
let usersTotalPostsViewWithMetrics = testDb["usersTotalPostsView"];
let expectedResults = [
    {
        "_id": 1,
        username: "john_doe",
        email: "john@hotmail.com",
        socialMediaPostsAboutIdol: [{
            _id: 100,
            userId: 1,
            content: "Behind these hazel eyes is someone who wants to audition for American idol",
            likes: 6,
            comments: 10,
            totalReactions: 16
        }]
    },
    {
        _id: 2,
        username: "kelly_clarkson",
        email: "americanidol4ever@aol.com",
        socialMediaPostsAboutIdol: [
            {
                _id: 102,
                userId: 2,
                content: "I WON AMERICAN IDOL!",
                likes: 300,
                comments: 100,
                totalReactions: 400
            },
            {
                _id: 103,
                userId: 2,
                content: "Did you see me on American Idol?",
                likes: 30,
                comments: 10,
                totalReactions: 40
            }
        ]
    }
];
let results = usersTotalPostsViewWithMetrics.aggregate().toArray();
assert.sameMembers(expectedResults, results);

/**
 * Run a $match query to only view one specific user's posts containing the word "idol" with
 * engagement metrics (totalSocialMediaReactions).
 */
expectedResults = [{
    _id: 1,
    username: "john_doe",
    email: "john@hotmail.com",
    socialMediaPostsAboutIdol: [{
        _id: 100,
        userId: 1,
        content: "Behind these hazel eyes is someone who wants to audition for American idol",
        likes: 6,
        comments: 10,
        totalReactions: 16
    }]
}];
results = usersTotalPostsViewWithMetrics
              .aggregate([{
                  $match: {
                      username: "john_doe"  // Match a specific username
                  }
              }])
              .toArray();
assert.sameMembers(expectedResults, results);
dropSearchIndex(totalSocialMediaReactionsView, {name: "totalReactionsIndex"});
