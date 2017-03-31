// Tests for the arrayFilters option to update and findAndModify.
// TODO SERVER-28576: Implement these tests in terms of the shell helpers.
(function() {
    "use strict";

    let coll = db.update_arrayFilters;
    coll.drop();

    //
    // Update.
    //

    // Non-array arrayFilters fail to parse.
    let res = db.runCommand(
        {update: coll.getName(), updates: [{q: {}, u: {$set: {a: 5}}, arrayFilters: {i: 0}}]});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    // Non-object array filter fail to parse.
    res = db.runCommand(
        {update: coll.getName(), updates: [{q: {}, u: {$set: {a: 5}}, arrayFilters: ["bad"]}]});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    // Bad array filter fails to parse.
    res = db.runCommand({
        update: coll.getName(),
        updates: [{q: {}, u: {$set: {a: 5}}, arrayFilters: [{i: 0, j: 0}]}]
    });
    assert(res.hasOwnProperty("writeErrors"), tojson(res));
    assert.eq(res.writeErrors.length, 1, tojson(res.writeErrors));
    assert.writeError(res.writeErrors[0], ErrorCodes.FailedToParse);
    assert.neq(-1,
               res.writeErrors[0].errmsg.indexOf(
                   "Each array filter must use a single top-level field name"),
               "update failed for a reason other than failing to parse array filters");

    // Multiple array filters with the same id fails to parse.
    res = db.runCommand({
        update: coll.getName(),
        updates: [{q: {}, u: {$set: {a: 5}}, arrayFilters: [{i: 0}, {j: 0}, {i: 1}]}]
    });
    assert(res.hasOwnProperty("writeErrors"), tojson(res));
    assert.eq(res.writeErrors.length, 1, tojson(res.writeErrors));
    assert.writeError(res.writeErrors[0], ErrorCodes.FailedToParse);
    assert.neq(
        -1,
        res.writeErrors[0].errmsg.indexOf(
            "Found multiple array filters with the same top-level field name"),
        "update failed for a reason other than multiple array filters with the same top-level field name");

    // Good value for arrayFilters succeeds.
    res = db.runCommand({
        update: coll.getName(),
        updates: [{q: {}, u: {$set: {a: 5}}, arrayFilters: [{i: 0}, {j: 0}]}]
    });
    assert(!res.hasOwnProperty("writeErrors"), tojson(res));

    //
    // FindAndModify.
    //

    // Non-array arrayFilters fail to parse.
    res = db.runCommand(
        {findAndModify: coll.getName(), query: {}, update: {$set: {a: 5}}, arrayFilters: {i: 0}});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    // Non-object array filter fail to parse.
    res = db.runCommand(
        {findAndModify: coll.getName(), query: {}, update: {$set: {a: 5}}, arrayFilters: ["bad"]});
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch);

    // arrayFilters option not allowed with remove=true.
    res = db.runCommand(
        {findAndModify: coll.getName(), query: {}, remove: true, arrayFilters: [{i: 0}]});
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
    assert.neq(
        -1,
        res.errmsg.indexOf("Cannot specify arrayFilters and remove=true"),
        "findAndModify failed for a reason other than specifying arrayFilters with remove=true");

    // Bad array filter fails to parse.
    res = db.runCommand({
        findAndModify: coll.getName(),
        query: {},
        update: {$set: {a: 5}},
        arrayFilters: [{i: 0, j: 0}]
    });
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
    assert.neq(-1,
               res.errmsg.indexOf("Each array filter must use a single top-level field name"),
               "findAndModify failed for a reason other than failing to parse array filters");

    // Multiple array filters with the same id fails to parse.
    res = db.runCommand({
        findAndModify: coll.getName(),
        query: {},
        update: {$set: {a: 5}},
        arrayFilters: [{i: 0}, {j: 0}, {i: 1}]
    });
    assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
    assert.neq(
        -1,
        res.errmsg.indexOf("Found multiple array filters with the same top-level field name"),
        "findAndModify failed for a reason other than multiple array filters with the same top-level field name");

    // Good value for arrayFilters succeeds.
    res = db.runCommand({
        findAndModify: coll.getName(),
        query: {},
        update: {$set: {a: 5}},
        arrayFilters: [{i: 0}, {j: 0}]
    });
    assert.commandWorked(res);
})();