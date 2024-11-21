/**
 * Test that $concatArrays works as a window function.
 * @tags: [requires_fcv_81]
 */

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db["set_window_fields_concatArrays"];
coll.drop();

const bookData = [
    {_id: 0, author: "Hana", publisher: "Pub1", books: ["Hana Book 0", "Hana Book 1"]},
    {_id: 1, author: "Militsa", publisher: "Pub1", books: ["Militsa Book 0"]},
    {_id: 2, author: "Ben", publisher: "Pub2", books: ["Ben Book 0", "Ben Book 1", "Ben Book 2"]},
    {_id: 3, author: "Adrian", publisher: "Pub1", books: ["Adrian Book 0"]}
];

assert.commandWorked(coll.insert(bookData));

// Test that $concatArrays correctly concatenates arrays, preserves sort order and preserves order
// of the elements inside the concatenated arrays.
let result = coll.aggregate([
                     {$match: {_id: {$in: [0, 1, 2]}}},
                     {
                         $setWindowFields: {
                             partitionBy: '$publisher',
                             sortBy: {_id: 1},
                             output: {booksFromPublisher: {$concatArrays: '$books'}}
                         }
                     },
                     {$sort: {_id: 1}}
                 ])
                 .toArray();

assert.eq(result, [
    {
        "_id": 0,
        "author": "Hana",
        "publisher": "Pub1",
        "books": ["Hana Book 0", "Hana Book 1"],
        "booksFromPublisher": ["Hana Book 0", "Hana Book 1", "Militsa Book 0"]
    },
    {
        "_id": 1,
        "author": "Militsa",
        "publisher": "Pub1",
        "books": ["Militsa Book 0"],
        "booksFromPublisher": ["Hana Book 0", "Hana Book 1", "Militsa Book 0"]
    },
    {
        "_id": 2,
        "author": "Ben",
        "publisher": "Pub2",
        "books": ["Ben Book 0", "Ben Book 1", "Ben Book 2"],
        "booksFromPublisher": ["Ben Book 0", "Ben Book 1", "Ben Book 2"]
    }
]);

// Test that $concatArrays respects the window boundaries (i.e. that removal works)
result = coll.aggregate([
                 {
                     $setWindowFields: {
                         partitionBy: '$publisher',
                         sortBy: {_id: 1},
                         output: {
                             adjacentBooksFromPublisher:
                                 {$concatArrays: '$books', window: {documents: [-1, 0]}}
                         }
                     }
                 },
                 {$sort: {_id: 1}},
                 {$project: {_id: 1, author: 1, adjacentBooksFromPublisher: 1}}
             ])
             .toArray();
assert.eq(result, [
    {"_id": 0, "author": "Hana", "adjacentBooksFromPublisher": ["Hana Book 0", "Hana Book 1"]},
    {
        "_id": 1,
        "author": "Militsa",
        "adjacentBooksFromPublisher": ["Hana Book 0", "Hana Book 1", "Militsa Book 0"]
    },
    {
        "_id": 2,
        "author": "Ben",
        "adjacentBooksFromPublisher": ["Ben Book 0", "Ben Book 1", "Ben Book 2"]
    },
    {
        "_id": 3,
        "author": "Adrian",
        "adjacentBooksFromPublisher": ["Militsa Book 0", "Adrian Book 0"]
    }
]);

// Test for errors on non-array types ($concatArrays only supports arrays).
assertErrorCode(coll,
                [{$setWindowFields: {output: {willFail: {$concatArrays: '$publisher'}}}}],
                ErrorCodes.TypeMismatch);
