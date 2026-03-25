/**
 * Tests for $unionWith cross-database functionality. Exercises both the short-form ({db, coll}) and
 * pipeline syntax ({db, coll, pipeline}), nonexistent databases, nonexistent collections, empty
 * local collections, and views as the foreign collection.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_83,
 * ]
 */
import {anyEq} from "jstests/aggregation/extras/utils.js";

const localDB = db.getSiblingDB(jsTestName() + "_local");
const foreignDB = db.getSiblingDB(jsTestName() + "_foreign");
localDB.dropDatabase();
foreignDB.dropDatabase();

const localColl = localDB.local_coll;
const foreignColl = foreignDB.foreign_coll;

const localDocs = [
    {_id: 1, x: "a"},
    {_id: 2, x: "b"},
    {_id: 3, x: "c"},
];
const foreignDocs = [
    {_id: 4, x: "d"},
    {_id: 5, x: "e"},
    {_id: 6, x: "f"},
];

assert.commandWorked(localColl.insert(localDocs));
assert.commandWorked(foreignColl.insert(foreignDocs));

// ------------------------------------------------------------
// Basic cross-db $unionWith without a pipeline.
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([{$unionWith: {db: foreignDB.getName(), coll: foreignColl.getName()}}, {$sort: {_id: 1}}])
        .toArray();
    assert.eq(result, localDocs.concat(foreignDocs));
}

// ------------------------------------------------------------
// Cross-db $unionWith with an empty pipeline.
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([
            {$unionWith: {db: foreignDB.getName(), coll: foreignColl.getName(), pipeline: []}},
            {$sort: {_id: 1}},
        ])
        .toArray();
    assert.eq(result, localDocs.concat(foreignDocs));
}

// ------------------------------------------------------------
// Cross-db $unionWith with a pipeline containing stages.
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([
            {
                $unionWith: {
                    db: foreignDB.getName(),
                    coll: foreignColl.getName(),
                    pipeline: [{$match: {x: "d"}}],
                },
            },
            {$sort: {_id: 1}},
        ])
        .toArray();
    const expected = localDocs.concat([{_id: 4, x: "d"}]);
    assert.eq(result, expected);
}

// ------------------------------------------------------------
// Cross-db $unionWith with a $project in the sub-pipeline.
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([
            {
                $unionWith: {
                    db: foreignDB.getName(),
                    coll: foreignColl.getName(),
                    pipeline: [{$project: {x: 1, _id: 0}}],
                },
            },
            {$sort: {_id: 1, x: 1}},
        ])
        .toArray();
    const expectedForeign = [{x: "d"}, {x: "e"}, {x: "f"}];
    assert(
        anyEq(result, localDocs.concat(expectedForeign)),
        "Expected: " + tojson(localDocs.concat(expectedForeign)) + " Got: " + tojson(result),
    );
}

// ------------------------------------------------------------
// Cross-db $unionWith where the foreign database does not exist.
// Should return only the local documents (empty foreign side).
// ------------------------------------------------------------
{
    const nonExistentDB = "unionWith_cross_db_nonexistent_db_" + ObjectId().str;
    const result = localColl
        .aggregate([{$unionWith: {db: nonExistentDB, coll: "any_collection"}}, {$sort: {_id: 1}}])
        .toArray();
    assert.eq(result, localDocs);
}

// ------------------------------------------------------------
// Cross-db $unionWith where the foreign database exists but
// the collection does not.
// Should return only the local documents (empty foreign side).
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([{$unionWith: {db: foreignDB.getName(), coll: "nonexistent_collection"}}, {$sort: {_id: 1}}])
        .toArray();
    assert.eq(result, localDocs);
}

// ------------------------------------------------------------
// Cross-db $unionWith where the local collection is empty.
// Should return only the foreign documents.
// ------------------------------------------------------------
{
    const emptyColl = localDB.empty_coll;
    emptyColl.drop();
    assert.commandWorked(localDB.createCollection(emptyColl.getName()));

    const result = emptyColl
        .aggregate([{$unionWith: {db: foreignDB.getName(), coll: foreignColl.getName()}}, {$sort: {_id: 1}}])
        .toArray();
    assert.eq(result, foreignDocs);
}

// ------------------------------------------------------------
// Cross-db $unionWith where the local collection is empty and
// the foreign database does not exist.
// Should return an empty result set.
// ------------------------------------------------------------
{
    const emptyColl = localDB.empty_coll2;
    emptyColl.drop();
    assert.commandWorked(localDB.createCollection(emptyColl.getName()));
    const nonExistentDB = "unionWith_cross_db_nonexistent_db2_" + ObjectId().str;

    const result = emptyColl.aggregate([{$unionWith: {db: nonExistentDB, coll: "any_collection"}}]).toArray();
    assert.eq(result, []);
}

// ------------------------------------------------------------
// Cross-db $unionWith where the foreign collection is a view.
// ------------------------------------------------------------
{
    assert.commandWorked(
        foreignDB.createView("foreign_view", foreignColl.getName(), [{$match: {x: {$in: ["d", "f"]}}}]),
    );

    const result = localColl
        .aggregate([{$unionWith: {db: foreignDB.getName(), coll: "foreign_view"}}, {$sort: {_id: 1}}])
        .toArray();
    const expected = localDocs.concat([
        {_id: 4, x: "d"},
        {_id: 6, x: "f"},
    ]);
    assert.eq(result, expected);
}

// ------------------------------------------------------------
// Cross-db $unionWith on a view with an additional sub-pipeline.
// The sub-pipeline should compose with the view's pipeline.
// ------------------------------------------------------------
{
    const result = localColl
        .aggregate([
            {
                $unionWith: {
                    db: foreignDB.getName(),
                    coll: "foreign_view",
                    pipeline: [{$match: {x: "f"}}],
                },
            },
            {$sort: {_id: 1}},
        ])
        .toArray();
    const expected = localDocs.concat([{_id: 6, x: "f"}]);
    assert.eq(result, expected);
}

// ------------------------------------------------------------
// Cross-db $unionWith referencing the same database as the local
// collection (db is explicitly specified but matches local).
// ------------------------------------------------------------
{
    const sameDBColl = localDB.same_db_foreign;
    sameDBColl.drop();
    const sameDBDocs = [{_id: 10, x: "z"}];
    assert.commandWorked(sameDBColl.insert(sameDBDocs));

    const result = localColl
        .aggregate([{$unionWith: {db: localDB.getName(), coll: sameDBColl.getName()}}, {$sort: {_id: 1}}])
        .toArray();
    assert.eq(result, localDocs.concat(sameDBDocs));
}

// ------------------------------------------------------------
// Multiple cross-db $unionWith stages chained sequentially.
// ------------------------------------------------------------
{
    const thirdDB = db.getSiblingDB(jsTestName() + "_third");
    thirdDB.dropDatabase();
    const thirdColl = thirdDB.third_coll;
    const thirdDocs = [
        {_id: 7, x: "g"},
        {_id: 8, x: "h"},
    ];
    assert.commandWorked(thirdColl.insert(thirdDocs));

    const result = localColl
        .aggregate([
            {$unionWith: {db: foreignDB.getName(), coll: foreignColl.getName()}},
            {$unionWith: {db: thirdDB.getName(), coll: thirdColl.getName()}},
            {$sort: {_id: 1}},
        ])
        .toArray();
    assert.eq(result, localDocs.concat(foreignDocs).concat(thirdDocs));

    thirdDB.dropDatabase();
}

// ------------------------------------------------------------
// Cleanup.
// ------------------------------------------------------------
localDB.dropDatabase();
foreignDB.dropDatabase();
