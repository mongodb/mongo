/**
 * Tests counters for match expressions.
 * @tags: [requires_fcv_42]
 */

(function() {
"use strict";
const mongod = MongoRunner.runMongod();
const db = mongod.getDB(jsTest.name());
const collName = jsTest.name();
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 0, b: "foo", c: [10, 50]}));
assert.commandWorked(coll.insert({_id: 1, a: 1, b: "foo", c: [10, 20, 30]}));
assert.commandWorked(coll.insert({_id: 2, a: 2, b: "bar"}));
assert.commandWorked(coll.insert({_id: 3, a: 3, c: [20]}));

/**
 * Run the command and check that the specified match expression counters are increased by the given
 * value.
 * - countersToIncrease: document with elements <match expression> : < increment value>.
 */
function checkCountersWithValues(command, countersToIncrease) {
    let beforeMetrics = {};
    const metrics = db.serverStatus().metrics.operatorCounters.match;
    for (let [exprName, inc] of Object.entries(countersToIncrease)) {
        beforeMetrics[exprName] = metrics[exprName];
    }

    command();

    const afterMetrics = db.serverStatus().metrics.operatorCounters.match;
    for (let [exprName, inc] of Object.entries(countersToIncrease)) {
        assert.eq(afterMetrics[exprName], beforeMetrics[exprName] + inc, exprName);
    }
}

/**
 * Simplified version of the above function where the counters are expected to increase by 1.
 * - countersToIncrease: array of match expression names or a single expression name.
 */
function checkCounters(command, countersToIncrease) {
    if (!Array.isArray(countersToIncrease)) {
        countersToIncrease = [countersToIncrease];
    }
    checkCountersWithValues(command, countersToIncrease.reduce((acc, val) => {
        acc[val] = 1;
        return acc;
    }, {}));
}

/**
 * Run a command expected to fail and check that the specified match expression counters are not
 * increased.
 * - countersNotToIncrease: array of match expression names.
 */
function checkCountersWithError(command, errorCode, countersNotToIncrease) {
    let beforeMetrics = {};
    const metrics = db.serverStatus().metrics.operatorCounters.match;
    countersNotToIncrease.forEach((exprName) => {
        beforeMetrics[exprName] = metrics[exprName];
    });

    const error = assert.throws(command);
    assert.commandFailedWithCode(error, errorCode);

    const afterMetrics = db.serverStatus().metrics.operatorCounters.match;
    countersNotToIncrease.forEach((exprName) => {
        assert.eq(afterMetrics[exprName], beforeMetrics[exprName], exprName);
    });
}

// Match expression counters in find.
checkCounters(() => assert.eq(1, coll.find({a: 1}).itcount()), "$eq");

checkCounters(() => assert.eq(2, coll.find({b: {$eq: "foo"}}).itcount()), "$eq");

checkCounters(() => assert.eq(2, coll.find({a: {$gt: 1}}).itcount()), "$gt");

checkCounters(() => assert.eq(3, coll.find({a: {$gte: 1}}).itcount()), "$gte");

checkCounters(() => assert.eq(1, coll.find({a: {$lt: 1}}).itcount()), "$lt");

checkCounters(() => assert.eq(2, coll.find({a: {$lte: 1}}).itcount()), "$lte");

checkCounters(() => assert.eq(3, coll.find({a: {$ne: 1}}).itcount()), "$ne");

checkCounters(() => assert.eq(2, coll.find({a: {$in: [1, 2]}}).itcount()), "$in");

checkCounters(() => assert.eq(2, coll.find({a: {$nin: [0, 2]}}).itcount()), "$nin");

checkCounters(() => assert.eq(1, coll.find({a: {$gt: 1, $in: [1, 2]}}).itcount()), ["$in", "$gt"]);

checkCounters(() => assert.eq(3, coll.find({c: {$exists: true}}).itcount()), "$exists");

checkCounters(() => assert.eq(4, coll.find({a: {$type: 1}}).itcount()), "$type");

checkCounters(() => assert.eq(2, coll.find({a: {$mod: [2, 0]}}).itcount()), "$mod");

checkCounters(() => assert.eq(2, coll.find({b: {$regex: /^f/}}).itcount()), "$regex");

checkCounters(() => assert.eq(2, coll.find({b: /^f/}).itcount()), "$regex");

checkCounters(() => assert.eq(2, coll.find({b: {$not: /^f/}}).itcount()), ["$regex", "$not"]);

checkCounters(() => assert.eq(2, coll.find({$jsonSchema: {required: ["a", "b", "c"]}}).itcount()),
              "$jsonSchema");

checkCounters(() => assert.eq(4, coll.find({$alwaysTrue: 1}).itcount()), "$alwaysTrue");

checkCounters(() => assert.eq(0, coll.find({$alwaysFalse: 1}).itcount()), "$alwaysFalse");

// Increments  only the $expr counter, counters for $lt, and $add are recorded in
// operatorCounters.expressions metrics.
checkCounters(() => assert.eq(4, coll.find({$expr: {$lt: ["$a", {$add: [1, 10]}]}}).itcount()),
              "$expr");

checkCounters(
    () => assert.eq(2, coll.find({a: {$mod: [2, 0]}, $comment: "Find even values."}).itcount()),
    ["$mod", "$comment"]);

checkCounters(() => assert.eq(1,
                              coll.find({
                                      $where: function() {
                                          return (this.a == 1);
                                      }
                                  })
                                  .itcount()),
              "$where");

// Array query operators.
checkCounters(() => assert.eq(2, coll.find({c: {$elemMatch: {$gt: 10, $lt: 50}}}).itcount()),
              ["$elemMatch", "$gt", "$lt"]);

checkCounters(() => assert.eq(1, coll.find({c: {$size: 2}}).itcount()), "$size");

checkCounters(() => assert.eq(1, coll.find({c: {$all: [10, 20]}}).itcount()), "$all");

// Logical operators.
checkCountersWithValues(() => assert.eq(1, coll.find({$and: [{c: 10}, {c: 20}]}).itcount()),
                        {"$and": 1, "$eq": 2});

checkCounters(() => assert.eq(2, coll.find({$and: [{c: 10}, {a: {$lt: 2}}]}).itcount()),
              ["$and", "$lt", "$eq"]);

checkCountersWithValues(() => assert.eq(2, coll.find({$or: [{c: 50}, {a: 2}]}).itcount()),
                        {"$or": 1, "$eq": 2});

checkCounters(() => assert.eq(1, coll.find({$nor: [{a: {$gt: 1}}, {c: 50}]}).itcount()),
              ["$nor", "$eq", "$gt"]);

checkCounters(() => assert.eq(2, coll.find({b: {$not: {$eq: "foo"}}}).itcount()), ["$not", "$eq"]);

// Bitwise query operators.
checkCounters(() => assert.eq(2, coll.find({a: {$bitsAllClear: [0]}}).itcount()), "$bitsAllClear");

checkCounters(() => assert.eq(1, coll.find({a: {$bitsAllSet: [0, 1]}}).itcount()), "$bitsAllSet");

checkCounters(() => assert.eq(3, coll.find({a: {$bitsAnyClear: [0, 1]}}).itcount()),
              "$bitsAnyClear");

checkCounters(() => assert.eq(3, coll.find({a: {$bitsAnySet: [0, 1]}}).itcount()), "$bitsAnySet");

// Invalid expressions do not increment any counter.
checkCountersWithError(() => coll.find({$or: [{c: {$size: 'a'}}, {a: 2}]}).itcount(),
                       ErrorCodes.BadValue,
                       ["$or", "$eq", "$size"]);

// Match expression counters in aggregation pipelines.

let pipeline = [{$match: {_id: 1}}];
checkCounters(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$eq");

pipeline = [{$match: {_id: {$gt: 0}}}];
checkCounters(() => assert.eq(3, coll.aggregate(pipeline).itcount()), "$gt");

pipeline = [{$match: {$and: [{c: 10}, {a: {$lt: 2}}]}}];
checkCounters(() => assert.eq(2, coll.aggregate(pipeline).itcount()), ["$and", "$eq", "$lt"]);

pipeline = [{$match: {c: {$type: 4}}}];
checkCounters(() => assert.eq(3, coll.aggregate(pipeline).itcount()), "$type");

pipeline = [{$match: {c: {$exists: false}}}];
checkCounters(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$exists");

pipeline = [{$match: {c: {$size: 1}}}];
checkCounters(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$size");

pipeline = [{$match: {a: {$bitsAllSet: [0, 1]}}}];
checkCounters(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$bitsAllSet");

// Invalid expressions do not increment any counter.
checkCountersWithError(
    () => coll.aggregate([{$match: {$and: [{b: /^fo/}, {a: {$mod: ['z', 2]}}]}}]).itcount(),
    ErrorCodes.BadValue,
    ["$and", "$regex", "$mod"]);

// Text search.
const textCollName = "myTextCollection";
const textColl = db[textCollName];

textColl.drop();
assert.commandWorked(textColl.insert({_id: 0, title: "coffee"}));
assert.commandWorked(textColl.insert({_id: 1, title: "Bake a cake"}));
assert.commandWorked(textColl.insert({_id: 2, title: "Cake with coffee"}));

assert.commandWorked(textColl.createIndex({title: "text"}));

checkCounters(
    () => assert.eq(1, textColl.find({$text: {$search: "cake", $caseSensitive: true}}).itcount()),
    "$text");

// Geospatial expressions.
const geoCollName = "myGeoCollection";
const geoColl = db[geoCollName];

geoColl.drop();
assert.commandWorked(geoColl.insert({_id: 0, location: {type: "Point", coordinates: [10, 20]}}));
assert.commandWorked(geoColl.createIndex({location: "2dsphere"}));

checkCounters(() => assert.eq(1,
                              geoColl
                                  .find({
                                      location: {
                                          $near: {
                                              $geometry: {type: "Point", coordinates: [10, 20]},
                                              $minDistance: 0,
                                              $maxDistance: 100
                                          }
                                      }
                                  })
                                  .itcount()),
              "$near");

checkCounters(() => assert.eq(1,
                              geoColl
                                  .find({
                                      location: {
                                          $nearSphere: {
                                              $geometry: {type: "Point", coordinates: [10.001, 20]},
                                              $minDistance: 100,
                                              $maxDistance: 500
                                          }
                                      }
                                  })
                                  .itcount()),
              "$nearSphere");

checkCounters(
    () => assert.eq(
        1,
        geoColl
            .find({
                location: {
                    $geoWithin: {
                        $geometry: {
                            type: "Polygon",
                            coordinates: [[[12, 18], [12, 21], [8, 21], [8, 18], [12, 18]]]
                        },
                    }
                }
            })
            .itcount()),
    "$geoWithin");

checkCounters(
    () => assert.eq(
        1,
        geoColl
            .find({
                location: {
                    $geoIntersects: {
                        $geometry: {
                            type: "Polygon",
                            coordinates: [[[12, 18], [12, 21], [8, 21], [8, 18], [12, 18]]]
                        },
                    }
                }
            })
            .itcount()),
    "$geoIntersects");

MongoRunner.stopMongod(mongod);
})();
