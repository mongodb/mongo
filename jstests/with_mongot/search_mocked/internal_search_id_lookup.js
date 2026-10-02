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

// Create an internal client connection to exercise the internal-only stage, leaving the main
// connection as a normal client so that setup writes and shutdown-time validation hooks are
// unaffected.
const internalConn = new Mongo(db.getMongo().host);
assert.commandWorked(
    internalConn.getDB("admin").runCommand({
        hello: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
    }),
);
const internalDB = internalConn.getDB("test");

// Runs an aggregate on the internal client connection. $_internalSearchIdLookup is an internal-only
// stage: it is rejected in user requests and only accepted from an internal client. internalClient
// connections must specify an explicit writeConcern on every command that accepts one (getMores are
// exempt, and DBCommandCursor omits it for us), so include an empty writeConcern.
function idLookupAgg(collectionName, pipeline) {
    const res = assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            readConcern: {},
            writeConcern: {},
        }),
    );
    return new DBCommandCursor(internalDB, res);
}

// Demonstrate that $_internalSearch skips `_id`s it cannot find.
assert.eq(
    4,
    idLookupAgg(collName, [
        {$match: {}},
        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
        {$_internalSearchIdLookup: {}},
    ]).itcount(),
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
    idLookupAgg(collName, [
        {$match: {}},
        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
        {$_internalSearchIdLookup: {}},
        {$sort: {"_id": 1}},
    ]).toArray(),
);

// Check that $_internalSearchIdLookup works as expected when the collection does not exist.
assert.eq(
    [],
    idLookupAgg("nonexistentColl", [
        {$match: {}},
        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
        {$_internalSearchIdLookup: {}},
    ]).toArray(),
);

// $_internalSearch should uassert when a collection is unspecified and the source stage
// provides documents with `_id` populated.
assert.commandFailedWithCode(
    internalDB.runCommand({
        aggregate: 1,
        pipeline: [
            {$listLocalSessions: {allUsers: true}},
            {$addFields: {"_id": ObjectId("5ab9cbfa31c2ab715d42129e")}},
            {$_internalSearchIdLookup: {}},
        ],
        cursor: {},
        readConcern: {},
        writeConcern: {},
    }),
    11140100,
);

// $_internalSearchIdLookup is internal-only, so it must be rejected in any user request, with or
// without the internal 'viewPipeline' field.
const kNotAllowedInUserRequest = 5491300;
assert.throwsWithCode(() => db[collName].aggregate([{$_internalSearchIdLookup: {}}]), kNotAllowedInUserRequest);
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
assert.throwsWithCode(
    () => db[collName].aggregate([{$_internalSearchIdLookup: {viewPipeline: []}}]),
    kNotAllowedInUserRequest,
);

// The stage must also be rejected when stored in a view definition, since view creation parses the
// pipeline without an operation context and would otherwise bypass the 'viewPipeline' check.
for (const viewPipeline of [undefined, [{$merge: {into: {db: "targetdb", coll: "pwned"}}}]]) {
    const spec = viewPipeline ? {viewPipeline} : {};
    assert.commandFailedWithCode(
        db.createView("idLookupView", collName, [{$_internalSearchIdLookup: spec}]),
        kNotAllowedInUserRequest,
    );
}
assert.eq(0, db.getSiblingDB("targetdb").pwned.find().itcount());

internalConn.close();
MongoRunner.stopMongod(conn);
