/*
 * SERVER 5782 : need a $literal operator to help prevent injection attacks
 *
 * This test validates part of SERVER 5872 ticket. Return a literal string
 * instead of the evaluated pipeline when $literal operator is used. Previously
 * there was no way to prevent evaluation of things like $fieldname.
 */

/*
 * 1) Load the articles db
 * 2) Load the testing functions
 * 3) Add another article with $author as the author name
 * 4) Run two aggregations: one with $literal and one without
 * 5) Assert that the two results are what we expected
 */
// Load the test documents
load('jstests/aggregation/data/articles.js');

// Make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSiblingDB("aggdb");

// Make an article where the author is $author
db.article.save( {
    _id : 4,
    title : "this is the fourth title" ,
    author : "$author" ,
    posted : new Date(1079895594000) ,
    pageViews : 123 ,
    tags : [ "bad" , "doesnt" , "matter" ] ,
    comments : [
        { author :"billy" , text : "i am the one" } ,
        { author :"jean" , text : "kid is not my son" }
    ],
    other : { foo : 9 }
});

// Create a var to compare against in the aggregation
var name = "$author";

// Aggregate checking against the field $author
var l1 = db.article.aggregate(
    { $project : {
        author : 1,
        authorWroteIt : { $eq:["$author", name] }
    }}
);

// All should be true since we are comparing a field to itself
var l1result = [
    {
        "_id" : 1,
        "author" : "bob",
        "authorWroteIt" : true
    },
    {
        "_id" : 2,
        "author" : "dave",
        "authorWroteIt" : true
    },
    {
        "_id" : 3,
        "author" : "jane",
        "authorWroteIt" : true
    },
    {
        "_id" : 4,
        "author" : "$author",
        "authorWroteIt" : true
    }
];

// Aggregate checking against the literal string $author
var l2 = db.article.aggregate(
    { $project : {
        author : 1,
        authorWroteIt : { $eq:["$author", { $literal: name } ] }
    }}
);

// Only the one written by $author should be true
var l2result = [
    {
        "_id" : 1,
        "author" : "bob",
        "authorWroteIt" : false
    },
    {
        "_id" : 2,
        "author" : "dave",
        "authorWroteIt" : false
    },
    {
        "_id" : 3,
        "author" : "jane",
        "authorWroteIt" : false
    },
    {
        "_id" : 4,
        "author" : "$author",
        "authorWroteIt" : true
    }
];

// Asserts
assert.eq(l1.result, l1result, 'l1 failed');
assert.eq(l2.result, l2result, 'l2 failed');
