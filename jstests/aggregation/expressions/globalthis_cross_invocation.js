// Tests that BSON wrappers retained in globalThis by user JavaScript are
// invalidated across JS invocations within a single aggregation, rather than
// remaining readable as freed memory in a later invocation (SECBUG-5387,
// SECBUG-5396).
//
// @tags: [
//   requires_scripting,
//   requires_fcv_80,
// ]
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 1, x: 1, text: "hello"},
        {_id: 2, x: 2, text: "world"},
        {_id: 3, x: 3, text: "mongo"},
    ]),
);

// A $function evaluation retains its argument wrapper in globalThis; the stack-local
// BSONObjBuilder backing it is destroyed when the evaluation returns.
const stashFn = function(arg) {
    globalThis._savedWrapper = arg;
    return 1;
};

// SECBUG-5387: The $accumulator init re-enters the same operation-cached scope that
// ran the $function, so the retained wrapper must be rejected there rather than
// read as freed memory.
(function() {
    const res = db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$sort: {_id: 1}},
            {$limit: 1},
            {
                $project: {
                    seed: {
                        $function: {
                            body: stashFn,
                            args: ["$$ROOT"],
                            lang: "js",
                        },
                    },
                },
            },
            {
                $group: {
                    _id: null,
                    result: {
                        $accumulator: {
                            initArgs: [1],
                            init: function() {
                                return globalThis._savedWrapper.x;
                            },
                            accumulate: function(state, seed) {
                                return state;
                            },
                            accumulateArgs: ["$seed"],
                            merge: function(left, right) {
                                return left;
                            },
                            lang: "js",
                        },
                    },
                },
            },
        ],
        cursor: {},
    });
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);
    assert(/Attempt to access an invalidated BSON Object in JS scope/.test(res.errmsg), res);
})();

// SECBUG-5396: createFunction() is not a parse; a body beginning with an anonymous
// function(){} is evaluated verbatim, so the side effects of a comma expression run
// during compilation, before the barrier that protects the function body. The
// retained wrapper must be invalid there too.
(function() {
    const res = db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$sort: {_id: 1}},
            {$limit: 1},
            {
                $project: {
                    seed: {
                        $function: {
                            body: stashFn,
                            args: ["$$ROOT"],
                            lang: "js",
                        },
                    },
                },
            },
            {
                $project: {
                    result: {
                        $function: {
                            body:
                                "function(){}, globalThis._savedWrapper.x, function() { return 0; }",
                            args: [1],
                            lang: "js",
                        },
                    },
                },
            },
        ],
        cursor: {},
    });
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);
    assert(/Attempt to access an invalidated BSON Object in JS scope/.test(res.errmsg), res);
})();

coll.drop();
