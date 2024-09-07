/*
 * FLE-supported commands that contain an invalid 'jsonSchema' field should return to the user a
 * more specific error message for diagnostic purposes.
 *
 * @tags: [requires_non_retryable_writes]
 */
const coll = db.command_json_schema_field;
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));

function assertCommandFailsWithCorrectError(command, code) {
    let res = db.runCommand(command);
    assert.commandFailedWithCode(res, code);
    assert(res.errmsg.includes("This command may be meant for a mongocryptd process"));
}

// Aggregate
assertCommandFailsWithCorrectError(
    {aggregate: coll.getName(), pipeline: [], cursor: {}, jsonSchema: {}},
    [ErrorCodes.FailedToParse, ErrorCodes.IDLUnknownFieldPossibleMongocryptd]);

// Find
assertCommandFailsWithCorrectError(
    {find: coll.getName(), jsonSchema: {}},
    [ErrorCodes.FailedToParse, ErrorCodes.IDLUnknownFieldPossibleMongocryptd]);

// FindAndModify
assertCommandFailsWithCorrectError(
    {findAndModify: coll.getName(), query: {_id: 0}, remove: true, jsonSchema: {}},
    [ErrorCodes.FailedToParse, ErrorCodes.IDLUnknownFieldPossibleMongocryptd]);

// Count
assertCommandFailsWithCorrectError({count: coll.getName(), jsonSchema: {}},
                                   ErrorCodes.IDLUnknownFieldPossibleMongocryptd);

// Distinct
assertCommandFailsWithCorrectError({distinct: coll.getName(), key: "a", jsonSchema: {}},
                                   ErrorCodes.IDLUnknownFieldPossibleMongocryptd);

// Write Commands
assertCommandFailsWithCorrectError({insert: coll.getName(), documents: [{}], jsonSchema: {}},
                                   ErrorCodes.IDLUnknownFieldPossibleMongocryptd);
assertCommandFailsWithCorrectError(
    {update: coll.getName(), updates: [{q: {}, u: {$inc: {a: 1}}}], jsonSchema: {}},
    ErrorCodes.IDLUnknownFieldPossibleMongocryptd);
assertCommandFailsWithCorrectError(
    {delete: coll.getName(), deletes: [{q: {}, limit: 0}], jsonSchema: {}},
    ErrorCodes.IDLUnknownFieldPossibleMongocryptd);

// Explain
assertCommandFailsWithCorrectError({explain: {count: coll.getName()}, jsonSchema: {}}, [
    ErrorCodes.FailedToParse,
    ErrorCodes.IDLUnknownField,
    ErrorCodes.IDLUnknownFieldPossibleMongocryptd
]);
