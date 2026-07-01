/**
 * Test the basic operation of a `$_internalSearchIdLookup` aggregation stage.
 */

let conn = MongoRunner.runMongod();
let db = conn.getDB("test");
const collName = "internal_search_id_lookup";
db[collName].drop();

assert.writeOK(db[collName].insert({_id: 1, x: "ow"}));
assert.writeOK(db[collName].insert({_id: 2, x: "now", y: "lorem"}));
assert.writeOK(db[collName].insert({_id: 3, x: "brown", y: "ipsum"}));
assert.writeOK(db[collName].insert({_id: 4, x: "cow", y: "lorem ipsum"}));
assert.writeOK(db[collName].insert({_id: 5, x: "brown", y: "ipsum"}));
assert.writeOK(db[collName].insert({_id: 6, x: "cow", y: "lorem ipsum"}));
assert.writeOK(db[collName].insert({_id: 7, x: "brown", y: "ipsum"}));
assert.writeOK(db[collName].insert({_id: 8, x: "cow", y: "lorem ipsum"}));

// Demonstrate that $_internalSearch skips `_id`s it cannot find.
assert.eq(
    4,
    db[collName]
        .aggregate([
            {$match: {}},
            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
            {$_internalSearchIdLookup: {}},
        ])
        .itcount(),
);

// Check returned documents are what we expect after skipping some `id`s.
const expectedEvenIdDocs = [
    {_id: 2, x: "now", y: "lorem"},
    {_id: 4, x: "cow", y: "lorem ipsum"},
    {_id: 6, x: "cow", y: "lorem ipsum"},
    {_id: 8, x: "cow", y: "lorem ipsum"},
];
assert.eq(
    expectedEvenIdDocs,
    db[collName]
        .aggregate([
            {$match: {}},
            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
            {$_internalSearchIdLookup: {}},
            {$sort: {"_id": 1}},
        ])
        .toArray(),
);

// Check that $_internalSearchIdLookup works as expected when the collection does not exist.
assert.eq(
    [],
    db["nonexistentColl"]
        .aggregate([
            {$match: {}},
            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
            {$_internalSearchIdLookup: {}},
        ])
        .toArray(),
);

// $_internalSearch should uassert when a collection is unspecified and the source stage
// provides documents with `_id` populated.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: 1,
        pipeline: [
            {$listLocalSessions: {allUsers: true}},
            {$addFields: {"_id": ObjectId("5ab9cbfa31c2ab715d42129e")}},
            {$_internalSearchIdLookup: {}},
        ],
        cursor: {},
    }),
    11140100,
);

// 'viewPipeline' is an internal field injected by the router, so any user-supplied 'viewPipeline'
// must be rejected at parse time, regardless of its contents.
const kNotAllowedInUserRequest = 5491300;
assert.throwsWithCode(
    () =>
        db[collName].aggregate([
            {
                $_internalSearchIdLookup: {
                    viewPipeline: [{$merge: {into: {db: "targetdb", coll: "pwned"}}}],
                },
            },
        ]),
    kNotAllowedInUserRequest,
);
// Any viewPipeline is rejected regardless of its (even benign) contents.
assert.throwsWithCode(
    () => db[collName].aggregate([{$_internalSearchIdLookup: {viewPipeline: [{$match: {x: "ow"}}]}}]),
    kNotAllowedInUserRequest,
);
assert.throwsWithCode(
    () => db[collName].aggregate([{$_internalSearchIdLookup: {viewPipeline: []}}]),
    kNotAllowedInUserRequest,
);

MongoRunner.stopMongod(conn);
