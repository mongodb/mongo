// Tests that JavaScript functions stored in system.js are loaded on a per-expression basis.
// @tags: [assumes_unsharded_collection, requires_non_retryable_writes, requires_fcv_44]

(function() {
"use strict";

const coll = db.system_js_access;
coll.drop();

assert.commandWorked(coll.insert([
    {"name": "Alice", "age": 68},
    {"name": "Bowen", "age": 20},
    {"name": "Carlos", "age": 24},
    {"name": "Daniel", "age": 47},
    {"name": "Eric", "age": 14},
    {"name": "France", "age": 1}
]));

// Store JS function in database.
assert.commandWorked(db.system.js.insert({
    _id: "isAdult",
    value: function(age) {
        return age >= 21;
    }
}));

// $where can access stored functions.
assert.commandWorked(db.runCommand({find: coll.getName(), filter: {$where: "isAdult(this.age)"}}));

// $function cannot access stored functions.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline:
        [{$addFields: {isAdult: {$function: {body: "isAdult(age)", args: ["$age"], lang: "js"}}}}],
    cursor: {}
}),
                             ErrorCodes.JSInterpreterFailure);

// The same function specified in-line can be executed by $function.
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $match: {
            $expr: {
                $function: {
                    body: function(age) {
                        return age >= 21;
                    },
                    args: ["$age"],
                    lang: "js"
                }
            }
        }
    }],
    cursor: {}
}));

// Mixed queries with both $where and $function should fail, because $where has to provide system.js
// to user code, and $function has to not provide it.
assert.commandFailedWithCode(db.runCommand({
    find: coll.getName(),
    filter: {
        $and: [
            {
                $expr: {
                    $function: {
                        body: function(age) {
                            return age >= 21;
                        },
                        args: ["$age"],
                        lang: "js"
                    }
                }
            },
            {$where: "isAdult(this.age)"}
        ]
    }
}),
                             4649200);

// Queries with both $function and $accumulator should succeed, because both of these operators
// provide system.js to user code.
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $match: {
                $expr: {
                    $function: {
                        body: function(age) {
                            return age >= 21;
                        },
                        args: ["$age"],
                        lang: "js"
                    }
                }
            }
        },
        {
            $group: {
                _id: "$name",
                age: {
                    $accumulator: {
                        init: function() {
                            return 0;
                        },
                        accumulate: function(state, value) {
                            return state + value;
                        },
                        accumulateArgs: ["$age"],
                        merge: function(state1, state2) {
                            return state1 + state2;
                        },
                        lang: 'js',
                    }
                }
            }
        }
    ],
    cursor: {}
}));

assert.commandWorked(db.system.js.remove({_id: "isAdult"}));
}());
