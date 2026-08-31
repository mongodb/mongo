/**
 * Test to verify that the schema validator works correctly in a multi-threaded environment, when
 * $expr uses expressions which mutate variable values while executing ($let, $map etc).
 *
 * Marked as 'requires_persistence' to prevent the test from running on 'inMemory' variant, because
 * the test generates a large oplog and 'inMemory' instances have limited resources to accommodate
 * all nodes in the replica set (which all run in the same instance), so it may fail with the OOM
 * error.
 * @tags: [
 *  requires_non_retryable_writes,
 *  requires_persistence,
 *  catches_command_failures,
 *  # This workload can use a lot of memory and cause an OOM on hosts if run against a larger
 *  # topology.
 *  requires_standalone,
 * ]
 */
const kObj = {
    one: 1,
    nested: {
        s0: {deep: 0, pad: {}},
        s1: {deep: 1, pad: [null, {}, []]},
        s2: {deep: 2, pad: {a: {b: {c: [1, 2, 3]}}}},
        s3: {deep: 3, pad: [{k: 1}, {k: 2}]},
        s4: {deep: 4, pad: {"dotted.name": 1, empty: [], nil: null}},
        unusedA: {deep: 99},
        unusedB: {deep: 98, pad: {deep: 97}},
    },
    other: {z: null},
    extra: "field",
};

const kSlots = 5;

export const $config = (function () {
    function setup(db, collName) {
        for (let i = 0; i < 200; ++i) {
            assert.commandWorked(
                db[collName].insert({
                    _id: i,
                    a: i,
                    one: 1,
                    counter: 0,
                    array: [0, i],
                    obj: {slot: "s" + (i % kSlots), deep: i % kSlots},
                }),
            );
        }

        // Add a validator which checks that field 'a' has value 5 and sum of the elements in field
        // 'array' is 5. The expression is purposefully complex so that it can create a stress on
        // expressions with variables.
        assert.commandWorked(
            db.runCommand({
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
                                            in: {$multiply: ["$$item", "$one"]},
                                        },
                                    },
                                ],
                            },
                            {
                                $eq: [
                                    5,
                                    {
                                        $sum: {
                                            $map: {"input": "$array", "as": "item", "in": "$$item"},
                                        },
                                    },
                                ],
                            },
                            {
                                $let: {
                                    vars: {slot: "$obj.slot"},
                                    in: {
                                        $eq: [
                                            "$obj.deep",
                                            {
                                                $getField: {
                                                    field: "deep",
                                                    input: {
                                                        $getField: {
                                                            field: "$$slot",
                                                            input: {
                                                                $getField: {
                                                                    field: "nested",
                                                                    input: {$literal: kObj},
                                                                },
                                                            },
                                                        },
                                                    },
                                                },
                                            },
                                        ],
                                    },
                                },
                            },
                        ],
                    },
                },
            }),
        );
    }

    const states = {
        applyValidator: function (db, collName) {
            assert.commandWorked(db[collName].update({_id: 5}, {$inc: {counter: 1}}));
            assert.commandFailedWithCode(
                db[collName].update({_id: 4}, {$set: {a: 4}, $inc: {counter: 1}}),
                ErrorCodes.DocumentValidationFailure,
            );

            // Update all the documents in the collection.
            retryOnRetryableError(
                () => {
                    assert.commandWorked(
                        db[collName].update(
                            {},
                            {$set: {a: 5, array: [2, 3]}, $inc: {counter: 1}},
                            {multi: true},
                        ),
                    );
                },
                100,
                undefined,
                TestData.runningWithBalancer ? [ErrorCodes.QueryPlanKilled] : [],
            );

            // Validation fails when elements of 'array' doesn't add up to 5.
            assert.commandFailedWithCode(
                db[collName].update({_id: 4}, {$set: {a: 5, array: [2, 2]}}),
                ErrorCodes.DocumentValidationFailure,
            );

            const slot = this.tid % kSlots;
            assert.commandWorked(
                db[collName].update({_id: 5}, {$set: {"obj.slot": "s" + slot, "obj.deep": slot}}),
            );
            assert.commandFailedWithCode(
                db[collName].update(
                    {_id: 5},
                    {$set: {"obj.slot": "s" + slot, "obj.deep": (slot + 1) % kSlots}},
                ),
                ErrorCodes.DocumentValidationFailure,
            );
        },
    };

    let transitions = {applyValidator: {applyValidator: 1}};

    return {
        threadCount: 30,
        iterations: 50,
        states: states,
        startState: "applyValidator",
        transitions: transitions,
        setup: setup,
    };
})();
