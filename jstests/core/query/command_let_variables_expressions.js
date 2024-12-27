// Tests that commands like find, aggregate and update accept a 'let' parameter which defines
// variables for use in expressions within the command. Specifically covers cases where the
// parameter definitions contain expressions.
// @tags: [
//   requires_non_retryable_writes,
//   does_not_support_transactions,
//   requires_getmore,
// ]
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.getCollection(jsTestName());

function setupColl() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 2, a: {}},
        {_id: 3, b: 1},
        {_id: 4, a: "$notAFieldPath"},
    ]));
}
setupColl();

const missingLetParam = {
    $getField: {field: "c", input: {a: 1}}
};
const literalLetParam = {
    $literal: "$notAFieldPath"
};

// Run find commands with 'let' parameters containing expressions.
{
    // 'let' parameter expression that resolves to a missing value should not cause a query error.
    let result = assert.commandWorked(db.runCommand({
        find: coll.getName(),
        filter: {$expr: {$eq: ["$a", "$$c"]}},
        let : {c: missingLetParam},
    }));
    assertArrayEq({
        actual: result.cursor.firstBatch,
        expected: [{_id: 3, b: 1}],
    });

    // 'let' parameter expression that includes $literal and a $-prefixed string should not be
    // misinterpreted as a field path.
    result = assert.commandWorked(db.runCommand({
        find: coll.getName(),
        filter: {$expr: {$eq: ["$a", "$$c"]}},
        let : {c: literalLetParam},
    }));
    assertArrayEq({
        actual: result.cursor.firstBatch,
        expected: [{_id: 4, a: "$notAFieldPath"}],
    });
}

// Run aggregate commands with 'let' parameters containing expressions.
{
    let result = coll.aggregate(
                         [{$match: {$expr: {$eq: ["$a", "$$c"]}}}],
                         {let : {c: missingLetParam}},
                         )
                     .toArray();
    assertArrayEq({
        actual: result,
        expected: [{_id: 3, b: 1}],
    });

    result = coll.aggregate(
                     [{$match: {$expr: {$eq: ["$a", "$$c"]}}}],
                     {let : {c: literalLetParam}},
                     )
                 .toArray();
    assertArrayEq({
        actual: result,
        expected: [{_id: 4, a: "$notAFieldPath"}],
    });
}

// Same thing but for update.
{
    assert.commandWorked(db.runCommand({
        update: coll.getName(),
        updates: [{
            q: {$expr: {$eq: ["$a", "$$c"]}},
            u: [{$set: {c: "updated"}}],
            multi: true,
        }],
        let : {c: missingLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [{_id: 2, a: {}}, {_id: 3, b: 1, c: "updated"}, {_id: 4, a: "$notAFieldPath"}],
    });

    // Undo changes.
    setupColl();

    assert.commandWorked(db.runCommand({
        update: coll.getName(),
        updates: [{
            q: {$expr: {$eq: ["$a", "$$c"]}},
            u: [{$set: {c: "updated"}}],
            multi: true,
        }],
        let : {c: literalLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [
            {_id: 2, a: {}},
            {_id: 3, b: 1},
            {_id: 4, a: "$notAFieldPath", c: "updated"},
        ],
    });
}

// Run findAndModify commands with 'let' parameters containing expressions.
{
    setupColl();

    assert.commandWorked(db.runCommand({
        findAndModify: coll.getName(),
        query: {_id: 3, $expr: {$eq: ["$a", "$$c"]}},
        update: [{$set: {c: "updated"}}],
        let : {c: missingLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [{_id: 2, a: {}}, {_id: 3, b: 1, c: "updated"}, {_id: 4, a: "$notAFieldPath"}],
    });

    // Undo changes.
    setupColl();

    assert.commandWorked(db.runCommand({
        findAndModify: coll.getName(),
        query: {_id: 4, $expr: {$eq: ["$a", "$$c"]}},
        update: [{$set: {c: "updated"}}],
        let : {c: literalLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [
            {_id: 2, a: {}},
            {_id: 3, b: 1},
            {_id: 4, a: "$notAFieldPath", c: "updated"},
        ],
    });
}

// Run delete commands with 'let' parameters containing expressions.
{
    setupColl();

    assert.commandWorked(db.runCommand({
        delete: coll.getName(),
        deletes: [{
            q: {$expr: {$eq: ["$a", "$$c"]}},
            limit: 0,  // multi
        }],
        let : {c: missingLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [{_id: 2, a: {}}, {_id: 4, a: "$notAFieldPath"}],
    });

    // Undo changes.
    setupColl();

    assert.commandWorked(db.runCommand({
        delete: coll.getName(),
        deletes: [{
            q: {$expr: {$eq: ["$a", "$$c"]}},
            limit: 0,  // multi
        }],
        let : {c: literalLetParam}
    }));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: [{_id: 2, a: {}}, {_id: 3, b: 1}],
    });
}

// Run a $lookup with let/pipeline syntax. In sharded environments, this will require serializing
// 'let' variables to send to the shards. One of the documents in the collection is missing the
// local field ("$a"), which will cause a missing 'let' variable to be serialized.
{
    setupColl();

    const result = coll.aggregate([
         {
            $lookup: {
                from: coll.getName(),
                as: "res",
                let: {local_a: "$a"},
                pipeline: [{$match: {$expr: {$eq: ["$$local_a", "$a"]},}}, {$project: {_id: 1}}]
            }
        }
    ]).toArray();
    assertArrayEq({
        actual: result,
        expected: [
            {_id: 2, a: {}, res: [{_id: 2}]},
            {_id: 3, b: 1, res: [{_id: 3}]},
            {_id: 4, a: "$notAFieldPath", res: [{_id: 4}]},
        ],
    });
}
