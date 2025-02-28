/**
 * Test the $uuid expression.
 * @tags: [
 *  featureFlagUUIDExpression,
 *  requires_fcv_81
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = db.expression_uuid;
const collName = "expression_uuid";
const docCount = 100;

function setupCollection() {
    assertDropCollection(db, collName);
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());
}

// Validate the $uuid expression works and returns a different
// value each time it is executed.
function basicTest() {
    setupCollection();

    const pipeline = [
        {$addFields: {uuidField: {$uuid: {}}}},
        {$project: {uuidStrField: {$toString: "$uuidField"}, uuidField: 1}},
        {$project: {_id: 0}}
    ];

    const resultArray = coll.aggregate(pipeline).toArray();
    assert.eq(resultArray.length, docCount);

    let lastStr = null;
    for (let result of resultArray) {
        const fields = Object.getOwnPropertyNames(result);
        assert.eq(["uuidField", "uuidStrField"], fields);
        const uuidField = result["uuidField"];
        const uuidStrField = result["uuidStrField"];
        assert.eq("object", typeof uuidField);
        assert.eq(`UUID("${uuidStrField}")`, uuidField.toString());
        assert.eq("string", typeof uuidStrField);
        assert.eq("867dee52-c331-484e-92d1-c56479b8e67e".length, uuidStrField.length);
        assert(lastStr == null || uuidStrField != lastStr);
        lastStr = uuidStrField;
    }
}

// Validate $uuid inside a lookup pipeline. The $lookup pipeline
// result should not be cached and each $uuid evaluation inside it should
// return a unique value.
function lookupTest() {
    setupCollection();

    const pipeline = [
        {$lookup: {
            from: collName,
            let: {
                docId: "$_id"
            },
            pipeline: [
                // {$match: {_id: "$$docId"}},
                {$addFields: {uuid: {$uuid: {}}}}
            ],
            as: "result"
        }},
    ];

    const resultArray = coll.aggregate(pipeline).toArray();
    assert.eq(resultArray.length, docCount);
    const s = resultArray.flatMap(doc => doc.result).map(doc => doc.uuid);
    const set = new Set(s);
    assert.eq(set.size, docCount * docCount);
}

basicTest();
lookupTest();
