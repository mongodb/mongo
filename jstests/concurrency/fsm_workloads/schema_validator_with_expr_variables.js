/**
 * Test to verify that the schema validator works correctly in a multi-threaded environment, when
 * $expr uses expressions which mutate variable values while executing ($let, $map etc).
 *
 * Marked as 'requires_persistence' to prevent the test from running on 'inMemory' variant, because
 * the test generates a large oplog and 'inMemory' instances have limited resources to accommodate
 * all nodes in the replica set (which all run in the same instance), so it may fail with the OOM
 * error.
 * @tags: [requires_non_retryable_writes, requires_persistence]
 */

"use strict";

var $config = (function() {
    function setup(db, collName) {
        for (let i = 0; i < 200; ++i) {
            assertAlways.commandWorked(
                db[collName].insert({_id: i, a: i, one: 1, counter: 0, array: [0, i]}));
        }

        // Add a validator which checks that field 'a' has value 5 and sum of the elements in field
        // 'array' is 5. The expression is purposefully complex so that it can create a stress on
        // expressions with variables.
        assertAlways.commandWorked(db.runCommand({
            collMod: collName,
            validator: {
                $expr: {
                    $and: [
                        {
                          $eq: [
                              5,
                              {
                                $let: {
                                    vars: {item: {$multiply: ["$a", "$one"]}},
                                    in : {$multiply: ["$$item", "$one"]}
                                }
                              }
                          ]
                        },
                        {
                          $eq: [
                              5,
                              {
                                $sum: {
                                    $map:
                                        {"input": "$array", "as": "item", "in": "$$item"}
                                }
                              }
                          ]
                        }
                    ]
                }
            }
        }));
    }

    const states = {
        applyValidator: function(db, collName) {
            assertAlways.commandWorked(db[collName].update({_id: 5}, {$inc: {counter: 1}}));
            assertAlways.commandFailedWithCode(
                db[collName].update({_id: 4}, {$set: {a: 4}, $inc: {counter: 1}}),
                ErrorCodes.DocumentValidationFailure);

            // Update all the documents in the collection.
            assertAlways.commandWorked(db[collName].update(
                {}, {$set: {a: 5, array: [2, 3]}, $inc: {counter: 1}}, {multi: true}));

            // Validation fails when elements of 'array' doesn't add up to 5.
            assertAlways.commandFailedWithCode(
                db[collName].update({_id: 4}, {$set: {a: 5, array: [2, 2]}}),
                ErrorCodes.DocumentValidationFailure);
        }
    };

    let transitions = {applyValidator: {applyValidator: 1}};

    return {
        threadCount: 50,
        iterations: 100,
        states: states,
        startState: "applyValidator",
        transitions: transitions,
        setup: setup
    };
})();
