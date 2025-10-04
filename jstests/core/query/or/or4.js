// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   requires_getmore,
//   requires_non_retryable_writes,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   # Uses mapReduce command.
//   requires_scripting,
// ]

import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.or4;
coll.drop();
db.getCollection("mrOutput").drop();

coll.createIndex({a: 1});
coll.createIndex({b: 1});

assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({b: 3}));
assert.commandWorked(coll.insert({b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 3}));

assert.eq(4, coll.count({$or: [{a: 2}, {b: 3}]}));
assert.eq(2, coll.count({$or: [{a: 2}, {a: 2}]}));

assert.eq(2, coll.find({}).skip(2).count(true));
assert.eq(
    2,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .skip(2)
        .count(true),
);
assert.eq(
    1,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .skip(3)
        .count(true),
);

assert.eq(2, coll.find({}).limit(2).count(true));
assert.eq(
    1,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .limit(1)
        .count(true),
);
assert.eq(
    2,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .limit(2)
        .count(true),
);
assert.eq(
    3,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .limit(3)
        .count(true),
);
assert.eq(
    4,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .limit(4)
        .count(true),
);

coll.remove({$or: [{a: 2}, {b: 3}]});
assert.eq(0, coll.count());

assert.commandWorked(coll.insert({b: 3}));
coll.remove({$or: [{a: 2}, {b: 3}]});
assert.eq(0, coll.count());

assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 3}));

coll.update({$or: [{a: 2}, {b: 3}]}, {$set: {z: 1}}, false, true);
assert.eq(3, coll.count({z: 1}));

assert.eq(3, coll.find({$or: [{a: 2}, {b: 3}]}).toArray().length);
assert.eq(
    coll.find().sort({_id: 1}).toArray(),
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .sort({_id: 1})
        .toArray(),
);
assert.eq(
    2,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .skip(1)
        .toArray().length,
);

assert.eq(
    3,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .batchSize(2)
        .toArray().length,
);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 4}));
assert.commandWorked(coll.insert({a: 2}));

assert.eq(
    4,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .batchSize(2)
        .toArray().length,
);

assert.commandWorked(coll.insert({a: 1, b: 3}));
assert.eq(
    4,
    coll
        .find({$or: [{a: 2}, {b: 3}]})
        .limit(4)
        .toArray().length,
);

assert.eq([1, 2], coll.distinct("a", {$or: [{a: 2}, {b: 3}]}).sort());

assert.commandWorked(
    coll.mapReduce(
        function () {
            if (!this.hasOwnProperty("a")) {
                emit("a", 0);
            } else {
                emit("a", this.a);
            }
        },
        function (key, vals) {
            return vals.reduce((a, b) => a + b, 0);
        },
        {out: {merge: "mrOutput"}, query: {$or: [{a: 2}, {b: 3}]}},
    ),
);
assert(
    resultsEq([{"_id": "a", "value": 7}], db.getCollection("mrOutput").find().toArray()),
    db.getCollection("mrOutput").find().toArray(),
);

coll.remove({});

assert.commandWorked(coll.insert({a: [1, 2]}));
assert.eq(1, coll.find({$or: [{a: 1}, {a: 2}]}).toArray().length);
assert.eq(1, coll.count({$or: [{a: 1}, {a: 2}]}));
assert.eq(1, coll.find({$or: [{a: 2}, {a: 1}]}).toArray().length);
assert.eq(1, coll.count({$or: [{a: 2}, {a: 1}]}));
