/**
 * Test that an $expr predicated which is eligible for being indexed with an $or pushdown executes
 * as expected.
 */
(function() {
    "use strict";

    const coll = db.expr_or_pushdown;
    coll.drop();
    assert.commandWorked(coll.createIndex({"a": 1, "b": 1}));
    assert.writeOK(coll.insert({_id: 0, a: "a", b: "b", d: "d"}));
    assert.writeOK(coll.insert({_id: 1, a: "a", b: "c", d: "d"}));
    assert.writeOK(coll.insert({_id: 2, a: "a", b: "x", d: "d"}));
    assert.writeOK(coll.insert({_id: 3, a: "x", b: "b", d: "d"}));
    assert.writeOK(coll.insert({_id: 4, a: "a", b: "b", d: "x"}));

    const results = coll.find({
                            $expr: {$and: [{$eq: ["$d", "d"]}, {$eq: ["$a", "a"]}]},
                            $or: [{"b": "b"}, {"b": "c"}]
                        })
                        .sort({_id: 1})
                        .toArray();

    assert.eq(results, [{_id: 0, a: "a", b: "b", d: "d"}, {_id: 1, a: "a", b: "c", d: "d"}]);
}());
