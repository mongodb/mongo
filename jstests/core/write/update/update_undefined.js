/*
 * This jstest demonstrates some simple ways to remove undefined values from user data.
 * @tags: [
 *   requires_multi_updates,
 *   requires_non_retryable_writes
 * ]
 */

import {documentEq} from "jstests/aggregation/extras/utils.js";

const coll = db.remove_undefined;
const docs = [
    // No "a.b" path
    {_id: 0, a: 1},
    {_id: 1, a: undefined},
    {_id: 2, a: {}},
    {_id: 3, a: []},
    {_id: 4, a: [undefined, 1, [undefined, 1]]},
    {_id: 5, b: undefined},

    // No arrays at a or a.b
    {_id: 10, a: {b: 1}},
    {_id: 11, a: {b: {}}},
    {_id: 12, a: {b: {c: 1}}},
    {_id: 13, a: {b: undefined, c: 1}},

    // a is an array, a.b non-array
    {_id: 20, a: [{b: undefined, c: 1}, {c: 1}, 1, [{b: undefined, c: 1}]]},

    // a.b is array
    {_id: 30, a: {b: []}},
    {_id: 31, a: {b: [1, {c: 1}]}},
    {_id: 32, a: {b: [undefined, 1, [undefined]], c: 1}},

    // a and a.b arrays
    {_id: 33, a: [{b: [undefined]}, {b: 1}, {b: [1, undefined]}, {c: 1}]},
];

function runUpdate(update, expected, documents = docs) {
    coll.drop();
    assert.commandWorked(coll.insert(documents));

    const command = Object.assign({update: coll.getName()}, update);
    assert.commandWorked(db.runCommand(command));

    const collContents = coll.find().sort({_id: 1}).toArray();
    assert.eq(collContents.length,
              expected.length,
              "Expected result and actual result do not have the same length.");
    for (let i = 0; i < collContents.length; i++) {
        assert(
            documentEq(collContents[i], expected[i]),
            "Collection contents after update are not what was expected. Did you sort your expected results by _id?\n" +
                "actualDoc=" + tojson(collContents[i]) + "\nexpectedDoc=" + tojson(expected[i]) +
                "\ncollContents=" + tojson(collContents) + "\nexpected=" + tojson(expected) +
                "\nupdate=" + tojson(command));
    }
}

// If you know the name of the field containing the undefined values, you can target it specifically
// by matching on undefined with $expr $type and using $set to null.

// Note that something like coll.update({a: {$type: "undefined"}}, {$set: {a: null}}, {multi: true})
// may not work if the dataset has arrays. Match expression $type does implicit array traversal
// after the last element, so this will replace [undefined, 1] with null. We need to use $expr here.
runUpdate(
    {
        updates:
            [{q: {$expr: {$eq: [{$type: "$a"}, "undefined"]}}, u: {$set: {"a": null}}, multi: true}]
    },
    [
        {_id: 0, a: 1},
        {_id: 1, a: null},  // This is the only document that changed.
        {_id: 2, a: {}},
        {_id: 3, a: []},
        {_id: 4, a: [undefined, 1, [undefined, 1]]},  // Note that this document does not change.
        {_id: 5, b: undefined},

        {_id: 10, a: {b: 1}},
        {_id: 11, a: {b: {}}},
        {_id: 12, a: {b: {c: 1}}},
        {_id: 13, a: {b: undefined, c: 1}},

        {_id: 20, a: [{b: undefined, c: 1}, {c: 1}, 1, [{b: undefined, c: 1}]]},

        {_id: 30, a: {b: []}},
        {_id: 31, a: {b: [1, {c: 1}]}},
        {_id: 32, a: {b: [undefined, 1, [undefined]], c: 1}},

        {_id: 33, a: [{b: [undefined]}, {b: 1}, {b: [1, undefined]}, {c: 1}]},
    ]);

// We can do the same thing with dotted paths. As above, this does not do implicit array traversal.
runUpdate({
    updates:
        [{q: {$expr: {$eq: [{$type: "$a.b"}, "undefined"]}}, u: {$set: {"a.b": null}}, multi: true}]
},
          [
              {_id: 0, a: 1},
              {_id: 1, a: undefined},
              {_id: 2, a: {}},
              {_id: 3, a: []},
              {_id: 4, a: [undefined, 1, [undefined, 1]]},
              {_id: 5, b: undefined},

              {_id: 10, a: {b: 1}},
              {_id: 11, a: {b: {}}},
              {_id: 12, a: {b: {c: 1}}},
              {_id: 13, a: {b: null, c: 1}},  // This is the only document that changed.

              {
                  _id: 20,
                  a: [{b: undefined, c: 1}, {c: 1}, 1, [{b: undefined, c: 1}]]
              },  // This one does not.

              {_id: 30, a: {b: []}},
              {_id: 31, a: {b: [1, {c: 1}]}},
              {_id: 32, a: {b: [undefined, 1, [undefined]], c: 1}},

              {_id: 33, a: [{b: [undefined]}, {b: 1}, {b: [1, undefined]}, {c: 1}]},
          ]);

// If you don't know the name of the fields where the undefined values are, you can still remove
// or replace undefined values in top-level fields. This example doesn't handle dotted paths or
// array traversals.
runUpdate({
    updates: [{
        q: {},
        u: [{
            $replaceWith: {
                $arrayToObject: {
                    $filter: {
                        input: {$objectToArray: "$$ROOT"},
                        // This condition removes undefined values. See example below for replacing
                        // these values with null, instead.
                        cond: {$not: {$eq: [{$type: "$$this.v"}, "undefined"]}}
                    }
                }
            }
        }],
        multi: true
    }]
},
          [
              {_id: 0, a: 1},
              {_id: 1},  // We removed field "a" here.
              {_id: 2, a: {}},
              {_id: 3, a: []},
              {_id: 4, a: [undefined, 1, [undefined, 1]]},
              {_id: 5},  // We removed field "b" here.

              {_id: 10, a: {b: 1}},
              {_id: 11, a: {b: {}}},
              {_id: 12, a: {b: {c: 1}}},
              {_id: 13, a: {b: undefined, c: 1}},

              {_id: 20, a: [{b: undefined, c: 1}, {c: 1}, 1, [{b: undefined, c: 1}]]},

              {_id: 30, a: {b: []}},
              {_id: 31, a: {b: [1, {c: 1}]}},
              {_id: 32, a: {b: [undefined, 1, [undefined]], c: 1}},

              {_id: 33, a: [{b: [undefined]}, {b: 1}, {b: [1, undefined]}, {c: 1}]},
          ]);

// Here's a similar example, but instead of removing the undefined values, we replace them with
// null. This example focuses on top-level field "a". Unlike above, it will replace undefined under
// dotted paths and arrays (but only one level).
runUpdate({
    updates: [{
        q: {},
        u: [{
            $set: {
                "a": {
                    $cond: {
                        // When "a" is an array, we convert {a: [undefined, 1]} --> {a: [null, 1]}
                        if: {$eq: [{$type: "$a"}, "array"]},
                        then: {
                            $map: {
                                    input: "$a",
                                    in: { 
                                        $cond: {if: {$eq: [{$type: "$$this"}, "undefined"]}, then: null, else: "$$this" }
                                    }
                            },
                        },
                        else: { 
                            $cond: {
                                // When "a" is an object, we convert {a: {b: undefined, c: 1}} -->
                                // {a: {b: null, c: 1}}
                                if: {$eq: [{$type: "$a"}, "object"]},
                                then: {
                                    $arrayToObject: {
                                        $map: {
                                            input: {$objectToArray: "$a"},
                                            in: { 
                                                $cond: {if: {$eq: [{$type: "$$this.v"}, "undefined"]}, then: {k: "$$this.k", v: null}, else: "$$this" }
                                            }
                                        }
                                    }
                                },
                                // When "a" is a scalar, we convert undefined -> null.
                                else: {$cond: {if: {$eq: [{$type: "$a"}, "undefined"]}, then: null, else: "$a" }}
                        }
                    }
                }
            }

            }
        }],
        multi: true
    }]
},
          [
              {_id: 0, a: 1},
              {_id: 1, a: null}, // Changed
              {_id: 2, a: {}},
              {_id: 3, a: []},
              {_id: 4, a: [null, 1, [undefined, 1]]}, // Changed
              {_id: 5, b: undefined},

              {_id: 10, a: {b: 1}},
              {_id: 11, a: {b: {}}},
              {_id: 12, a: {b: {c: 1}}},
              {_id: 13, a: {b: null, c: 1}},  // Changed

              {_id: 20, a: [{b: undefined, c: 1}, {c: 1}, 1, [{b: undefined, c: 1}]]},

              {_id: 30, a: {b: []}},
              {_id: 31, a: {b: [1, {c: 1}]}},
              {_id: 32, a: {b: [undefined, 1, [undefined]], c: 1}},

              {_id: 33, a: [{b: [undefined]}, {b: 1}, {b: [1, undefined]}, {c: 1}]},
          ]);
