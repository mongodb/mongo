/*
 * Tests that coerceToBool is not a valid expression.
 */

const coll = db.expr_invalid;
coll.drop();

assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {$expr: {$coerceToBool: {$eq: ["$_id", 15]}}}}),
    168,
    "Unrecognized expression '$coerceToBool'");

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [{$project: {x: {$coerceToBool: {$eq: ["$_id", 15]}}}}]
}),
                             31325,
                             "Unknown expression $coerceToBool");
