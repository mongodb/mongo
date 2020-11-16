/**
 * Test that the 'findAndModify' command throws the expected errors for invalid query, sort and
 * projection parameters. This test exercises the fix for SERVER-41829.
 *
 * @tags: [assumes_unsharded_collection, requires_fcv_49]
 */
(function() {
"use strict";

const coll = db.find_and_modify_invalid_inputs;
coll.drop();
coll.insert({_id: 0});
coll.insert({_id: 1});

function assertFailedWithCode(cmd, errorCode) {
    assert.commandFailedWithCode(
        coll.runCommand(Object.merge({findAndModify: coll.getName()}, cmd)), errorCode);
}

function assertWorked(cmd, expectedValue) {
    const out = assert.doesNotThrow(() => coll.findAndModify(cmd));
    assert.eq(out.value, expectedValue);
}

// Verify that the findAndModify command works when we supply a valid query.
let out = coll.findAndModify({query: {_id: 1}, update: {$set: {value: "basic"}}, new: true});
assert.eq(out, {_id: 1, value: "basic"});
assertWorked({query: null, update: {value: 2}, new: true}, 2);

// Verify that invalid 'query' object fails.
assertFailedWithCode({query: 1, update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({query: "{_id: 1}", update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({query: false, update: {value: 2}}, ErrorCodes.TypeMismatch);

// Verify that missing and empty query object is allowed.
assertWorked({update: {$set: {value: "missingQuery"}}, new: true}, "missingQuery");
assertWorked({query: {}, update: {$set: {value: "emptyQuery"}}, new: true}, "emptyQuery");

// Verify that command works when we supply a valid sort specification.
assertWorked({sort: {_id: -1}, update: {$set: {value: "sort"}}, new: true}, "sort");
assertWorked({sort: null, update: {value: 2}, new: true}, 2);

// Verify that invaid 'sort' object fails.
assertFailedWithCode({sort: 1, update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({sort: "{_id: 1}", update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({sort: false, update: {value: 2}}, ErrorCodes.TypeMismatch);

// Verify that missing and empty 'sort' object is allowed.
assertWorked({update: {$set: {value: "missingSort"}}, new: true}, "missingSort");
assertWorked({sort: {}, update: {$set: {value: "emptySort"}}, new: true}, "emptySort");

// Verify that the 'fields' projection works.
assertWorked({fields: {_id: 0}, update: {$set: {value: "project"}}, new: true}, "project");
assertWorked({fields: null, update: {value: 2}, new: true}, 2);

// Verify that invaid 'fields' object fails.
assertFailedWithCode({fields: 1, update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({fields: "{_id: 1}", update: {value: 2}}, ErrorCodes.TypeMismatch);
assertFailedWithCode({fields: false, update: {value: 2}}, ErrorCodes.TypeMismatch);

// Verify that missing and empty 'fields' object is allowed. Also verify that the command
// projects all the fields.
assertWorked({update: {$set: {value: "missingFields"}}, new: true}, "missingFields");
assertWorked({fields: {}, update: {$set: {value: "emptyFields"}}, new: true}, "emptyFields");

// Verify that findOneAndDelete() shell helper throws the same errors as findAndModify().
let err = assert.throws(() => coll.findOneAndDelete("{_id: 1}"));
assert(err.code == ErrorCodes.TypeMismatch, err);
err = assert.throws(() => coll.findOneAndDelete(null, {sort: 1}));
assert(err.code == ErrorCodes.TypeMismatch, err);

// Verify that findOneAndReplace() shell helper throws the same errors as findAndModify().
err = assert.throws(() => coll.findOneAndReplace("{_id: 1}", {}));
assert(err.code == ErrorCodes.TypeMismatch, err);
err = assert.throws(() => coll.findOneAndReplace(null, {}, {sort: 1}));
assert(err.code == ErrorCodes.TypeMismatch, err);

// Verify that findOneAndUpdate() shell helper throws the same errors as findAndModify().
err = assert.throws(() => coll.findOneAndUpdate("{_id: 1}", {$set: {value: "new"}}));
assert(err.code == ErrorCodes.TypeMismatch, err);
err = assert.throws(() => coll.findOneAndUpdate(null, {$set: {value: "new"}}, {sort: 1}));
assert(err.code == ErrorCodes.TypeMismatch, err);

// Verify that find and modify shell helpers allow null query object.
out = coll.findOneAndUpdate(null, {$set: {value: "findOneAndUpdate"}}, {returnNewDocument: true});
assert.eq(out.value, "findOneAndUpdate");

out = coll.findOneAndReplace(null, {value: "findOneAndReplace"}, {returnNewDocument: true});
assert.eq(out.value, "findOneAndReplace");

out = coll.findOneAndDelete(null);
assert.eq(out.value, "findOneAndReplace");

// Incompatiable parameters with 'remove'.
coll.drop();
coll.insert({_id: 0});
assertFailedWithCode({update: {value: 2}, remove: true}, ErrorCodes.FailedToParse);
assertFailedWithCode({query: {_id: 1}, upsert: true, remove: true}, ErrorCodes.FailedToParse);
assertFailedWithCode({new: true, remove: true}, ErrorCodes.FailedToParse);
assertFailedWithCode({update: {}, arrayFilters: [{"x.a": {$gt: 85}}], remove: true},
                     ErrorCodes.FailedToParse);

// Verify that the above updates succeed when the boolean values are set to 'false' or '0'.
assertWorked({update: {value: "newVal"}, new: true, remove: false}, "newVal");
assertWorked({update: {value: "newerVal"}, new: false, remove: 0}, "newVal");
assertWorked({query: {_id: 1}, update: {value: "newerVal"}, new: 1, remove: false, upsert: 1},
             "newerVal");
assertWorked(
    {update: {value: "newestVal"}, new: false, arrayFilters: [{"x.a": {$gt: 85}}], remove: 0.0},
    "newerVal");

out = coll.findAndModify(
    {query: {_id: 2}, update: {value: "newVal"}, new: false, remove: false, upsert: true});
assert.eq(out, null);

// Pipeline update with 'arrayFilters'.
assertFailedWithCode({update: [], arrayFilters: [{"x.a": {$gt: 85}}]}, ErrorCodes.FailedToParse);

// Invalid value for hint.
assertFailedWithCode({query: {_id: 0}, update: {value: "usedHint"}, hint: 1},
                     ErrorCodes.FailedToParse);

// Hint with string and object.
coll.createIndex({value: 1}, {name: 'value'});
assertWorked({query: {_id: 0}, update: {value: "usedHint"}, new: 1.0, hint: "value", remove: 0.0},
             "usedHint");
assertWorked({
    query: {_id: 0, value: "usedHint"},
    update: {value: "newValue"},
    new: 0,
    hint: {_id: 1},
    remove: 0
},
             "usedHint");
})();
