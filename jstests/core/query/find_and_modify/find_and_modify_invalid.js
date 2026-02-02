// Verifies that findAndModify commands with an invalid command object failed as expected.

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
const coll = assertDropAndRecreateCollection(db, jsTestName());

// Runs the actual command and expects it to fail with 'FailedToParse' error.
const assertFailedToParse = (cmd, callback) => {
    try {
        callback();
        assert(false, `findAndModifyCommand is supposed to fail: ${tojson(cmd)}`);
    } catch (err) {
        assert.commandFailedWithCode(err, ErrorCodes.FailedToParse);
    }
};

// Runs the findAndModify command with an (invalid) operation description as specified
// in 'cmd', once with and once without explain.
// Expects both runs to fail because the operation description is invalid.
const assertFindAndModifyFails = (cmd) => {
    // Run findAndModify command without explain.
    assertFailedToParse(cmd, () => coll.findAndModify(cmd));

    // Run findAndModify command with explain.
    assertFailedToParse(cmd, () => coll.explain("queryPlanner").findAndModify(cmd));
};

// Fully invalid findAndModify commands should trigger 'FailedToParse' errors.
[undefined, {}, false].forEach((cmd) => {
    assertFindAndModifyFails(cmd);
});

// Triggers "Cannot specify both an update and remove=true".
assertFindAndModifyFails({remove: true, update: {}});

// Triggers "Cannot specify both upsert=true and remove=true".
assertFindAndModifyFails({remove: true, upsert: true});

// Triggers "Cannot specify both new=true and remove=true; 'remove' always returns the deleted document".
assertFindAndModifyFails({remove: true, new: true});

// Triggers "Cannot specify arrayFilters and remove=true".
assertFindAndModifyFails({remove: true, arrayFilters: [{"element": {$gte: 100}}]});

// Triggers "Cannot specify arrayFilters and a pipeline update".
assertFindAndModifyFails({update: [{$match: {}}], arrayFilters: [{"element": {$gte: 100}}]});
