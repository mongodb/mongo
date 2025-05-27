/**
 * This test verifies that a nested $lookup with $search queries on both the outer, inner, and
 * top-level view applies the view definitions and returns results as expected.
 *
 * @tags: [ requires_fcv_81, featureFlagMongotIndexedViews ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const movies = testDb.movies;
const users = testDb.users;
const ratings = testDb.ratings;

movies.drop();
users.drop();
ratings.drop();

assert.commandWorked(movies.insertMany([
    {_id: 1, title: "The Shawshank Redemption", genres: ["Drama"], year: 1994},
    {_id: 2, title: "The Godfather", genres: ["Crime", "Drama"], year: 1972},
    {_id: 3, title: "Pulp Fiction", genres: ["Crime", "Drama"], year: 1994},
    {_id: 4, title: "The Dark Knight", genres: ["Action", "Crime", "Drama"], year: 2008},
    {_id: 5, title: "Fight Club", genres: ["Drama"], year: 1999}
]));

assert.commandWorked(users.insertMany([
    {_id: 101, name: "Alice", favorite_genres: ["Drama", "Crime"], age: 35},
    {_id: 102, name: "Bob", favorite_genres: ["Action"], age: 42},
    {_id: 103, name: "Charlie", favorite_genres: ["Drama"], age: 28},
    {_id: 104, name: "Diana", favorite_genres: ["Crime", "Action"], age: 31}
]));

assert.commandWorked(ratings.insertMany([
    {user_id: 101, movie_id: 1, rating: 5},
    {user_id: 101, movie_id: 2, rating: 4},
    {user_id: 102, movie_id: 4, rating: 5},
    {user_id: 103, movie_id: 1, rating: 3},
    {user_id: 103, movie_id: 5, rating: 5},
    {user_id: 104, movie_id: 3, rating: 4},
    {user_id: 104, movie_id: 4, rating: 4}
]));

// Create views on all three collections.
const moviesViewPipeline = [{
    $addFields: {
        display_title: {$concat: ["$title", " (", {$toString: "$year"}, ")"]},
        decade:
            {$concat: [{$toString: {$subtract: [{$trunc: {$divide: ["$year", 10]}}, 0]}}, "0s"]},
        runtime_minutes: {$cond: {if: {$eq: ["$title", "The Godfather"]}, then: 175, else: 142}}
    }
}];
assert.commandWorked(testDb.createView("moviesView", movies.getName(), moviesViewPipeline));
const moviesView = testDb.moviesView;

const usersViewPipeline = [{
    $addFields: {
        age_group: {
            $switch: {
                branches: [
                    {case: {$lte: ["$age", 30]}, then: "young adult"},
                    {case: {$lte: ["$age", 50]}, then: "middle-aged"}
                ],
                default: "senior"
            }
        },
        primary_genre: {$arrayElemAt: ["$favorite_genres", 0]},
        full_name: {$concat: [{$toUpper: "$name"}, ", viewer"]}
    }
}];
assert.commandWorked(testDb.createView("usersView", users.getName(), usersViewPipeline));
const usersView = testDb.usersView;

const ratingsViewPipeline = [{
    $addFields: {
        rating_text: {
            $switch: {
                branches: [
                    {case: {$eq: ["$rating", 5]}, then: "excellent"},
                    {case: {$eq: ["$rating", 4]}, then: "good"},
                    {case: {$eq: ["$rating", 3]}, then: "average"}
                ],
                default: "poor"
            }
        },
        is_recommended: {$gte: ["$rating", 4]}
    }
}];
assert.commandWorked(testDb.createView("ratingsView", ratings.getName(), ratingsViewPipeline));
const ratingsView = testDb.ratingsView;

// Create search indexes on all collections.
const moviesIndexName = "moviesIndex";
const usersIndexName = "usersIndex";
const ratingsIndexName = "ratingsIndex";
const indexConfigs = [
    {
        coll: moviesView,
        definition: {name: moviesIndexName, definition: {mappings: {dynamic: true}}}
    },
    {coll: usersView, definition: {name: usersIndexName, definition: {mappings: {dynamic: true}}}},
    {
        coll: ratingsView,
        definition: {name: ratingsIndexName, definition: {mappings: {dynamic: true}}}
    }
];

const lookupNestedTestCases = (isStoredSource) => {
    // This pipeline demonstrates a nested lookup structure with search queries at each level:
    // 1. The top level searches for movies with "Drama" in their genres
    // 2. First $lookup finds ratings with "excellent" or "good" rating_text
    // 3. Second $lookup finds users who have "Drama" as a favorite genre
    //
    // The pipeline applies $match stages to filter out results where the inner lookups returned
    // empty arrays. This ensures we only get movies that:
    // - Have "Drama" in their genres
    // - Have at least one "excellent" or "good" rating
    // - That rating was made by a user who has "Drama" as a favorite genre
    //
    // The expected results are movies 1 ("The Shawshank Redemption"), 2 ("The Godfather"),
    // and 5 ("Fight Club") - each with their complete nested document structure showing the
    // relevant ratings and user information. Finally, the view definitions are applied to each
    // respective document.
    const pipeline = [
            {
                $search: {
                    index: moviesIndexName,
                    text: {
                        query: "Drama",
                        path: "genres"
                    },
                    returnStoredSource: isStoredSource
                }
            },
            {
                $lookup: {
                    from: ratingsView.getName(),
                    localField: "_id",
                    foreignField: "movie_id",
                    as: "user_ratings",
                    pipeline: [
                        {
                            $search: {
                                index: ratingsIndexName,
                                text: {
                                    query: "excellent good",
                                    path: "rating_text"
                                },
                                returnStoredSource: isStoredSource
                            }
                        },
                        {
                            $lookup: {
                                from: usersView.getName(),
                                localField: "user_id",
                                foreignField: "_id",
                                as: "user_info",
                                pipeline: [
                                    {
                                        $search: {
                                            index: usersIndexName,
                                            text: {
                                                query: "Drama",
                                                path: "favorite_genres"
                                            },
                                            returnStoredSource: isStoredSource
                                        }
                                    }
                                ]
                            }
                        },
                        {
                            $match: {
                                "user_info": { $ne: [] }
                            }
                        },
                        {
                            $project: {
                                _id: 0
                            }
                        }
                    ]
                }
            },
            {
                $match: {
                    "user_ratings": { $ne: [] }
                }
            },
            {$sort: {_id: 1}}
        ];

    const expectedResults = [
        {
            _id: 1,
            title: "The Shawshank Redemption",
            genres: ["Drama"],
            year: 1994,
            display_title: "The Shawshank Redemption (1994)",
            decade: "1990s",
            runtime_minutes: 142,
            user_ratings: [{
                user_id: 101,
                movie_id: 1,
                rating: 5,
                rating_text: "excellent",
                is_recommended: true,
                user_info: [{
                    _id: 101,
                    name: "Alice",
                    favorite_genres: ["Drama", "Crime"],
                    age: 35,
                    age_group: "middle-aged",
                    primary_genre: "Drama",
                    full_name: "ALICE, viewer"
                }]
            }]
        },
        {
            _id: 2,
            title: "The Godfather",
            genres: ["Crime", "Drama"],
            year: 1972,
            display_title: "The Godfather (1972)",
            decade: "1970s",
            runtime_minutes: 175,
            user_ratings: [{
                user_id: 101,
                movie_id: 2,
                rating: 4,
                rating_text: "good",
                is_recommended: true,
                user_info: [{
                    _id: 101,
                    name: "Alice",
                    favorite_genres: ["Drama", "Crime"],
                    age: 35,
                    age_group: "middle-aged",
                    primary_genre: "Drama",
                    full_name: "ALICE, viewer"
                }]
            }]
        },
        {
            _id: 5,
            title: "Fight Club",
            genres: ["Drama"],
            year: 1999,
            display_title: "Fight Club (1999)",
            decade: "1990s",
            runtime_minutes: 142,
            user_ratings: [{
                user_id: 103,
                movie_id: 5,
                rating: 5,
                rating_text: "excellent",
                is_recommended: true,
                user_info: [{
                    _id: 103,
                    name: "Charlie",
                    favorite_genres: ["Drama"],
                    age: 28,
                    age_group: "young adult",
                    primary_genre: "Drama",
                    full_name: "CHARLIE, viewer"
                }]
            }]
        }
    ];

    validateSearchExplain(moviesView, pipeline, isStoredSource, moviesViewPipeline);

    const results = moviesView.aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfigs, lookupNestedTestCases);
