/*
 * Confirms that the explain command includes the command object that was run.
 *
 * @tags: [requires_fcv_49, sbe_incompatible]
 */
(function() {
"use strict";
const collName = "explain_includes_command";

db[collName].drop();
assert.commandWorked(db[collName].insert({_id: 0}));
assert.commandWorked(db[collName].insert({_id: 1}));
assert.commandWorked(db[collName].insert({_id: 2}));

/*
 * Runs explain on 'cmdToExplain' and ensures that the explain output contains the expected
 * 'command' field.
 */
function testExplainContainsCommand(cmdToExplain) {
    const verbosity = ["queryPlanner", "executionStats", "allPlansExecution"];
    for (const option of verbosity) {
        const explainOutput = db.runCommand({explain: cmdToExplain, verbosity: option});
        assert("command" in explainOutput);
        const explainCmd = explainOutput["command"];
        for (let key of Object.keys(cmdToExplain)) {
            assert.eq(explainCmd[key], cmdToExplain[key]);
        }
    }
}

// Test 'find'.
testExplainContainsCommand({find: collName, filter: {}});
testExplainContainsCommand({find: collName, filter: {_id: 1}});

// Test 'aggregate'.
testExplainContainsCommand({aggregate: collName, pipeline: [{$match: {_id: 1}}], cursor: {}});

// Test 'update'.
testExplainContainsCommand({update: collName, updates: [{q: {_id: 1}, u: {_id: 10}}]});

// Test 'delete'.
testExplainContainsCommand({delete: collName, deletes: [{q: {_id: 1}, limit: 0}]});

// Test 'findAndModify'.
testExplainContainsCommand({findAndModify: collName, query: {_id: 10}, update: {_id: 1}});
testExplainContainsCommand({findAndModify: collName, query: {_id: 1}, remove: true});

// Test 'count'.
testExplainContainsCommand({count: collName, query: {_id: 1}});

// Test 'distinct'.
testExplainContainsCommand({distinct: collName, key: "a"});
})();
