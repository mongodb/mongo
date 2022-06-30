/**
 * Test for $jsonSchema behavior in contexts other than document validation, including:
 * - read commands (count, geoNear, distinct, etc)
 * - queries that specify a collation
 * - $match stage within a view
 * - listCollections
 * - listDatabases
 * - graphLookup
 * - delete
 * - update
 * - findAndModify
 * - applyOps
 * - $elemMatch projection
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   requires_replication,
 *   # This test depends on hardcoded database name equality.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";

// For isWiredTiger.
load("jstests/concurrency/fsm_workload_helpers/server_types.js");
// For isReplSet
load("jstests/libs/fixture_helpers.js");
// For arrayEq.
load("jstests/aggregation/extras/utils.js");

const testName = "json_schema_misc_validation";
const testDB = db.getSiblingDB(testName);
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection(testName));
const coll = testDB.getCollection(testName);
coll.drop();

const isMongos = (testDB.runCommand("hello").msg === "isdbgrid");

// Test that $jsonSchema is rejected in an $elemMatch projection.
assert.throws(function() {
    coll.find({}, {a: {$elemMatch: {$jsonSchema: {}}}}).itcount();
});

// Test that an invalid $jsonSchema fails to parse in a count command.
const invalidSchema = {
    invalid: {}
};
assert.throws(function() {
    coll.count({$jsonSchema: invalidSchema});
});

// Test that an invalid $jsonSchema fails to parse in a $geoNear query.
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
let res = testDB.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [{
        $geoNear: {
            near: [30, 40],
            distanceField: "dis",
            query: {$jsonSchema: invalidSchema},
        }
    }],
});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
assert.neq(-1,
           res.errmsg.indexOf("Unknown $jsonSchema keyword"),
           `$geoNear failed for a reason other than invalid query: ${tojson(res)}`);

// Test that an invalid $jsonSchema fails to parse in a distinct command.
assert.throws(function() {
    coll.distinct("a", {$jsonSchema: invalidSchema});
});

// Test that an invalid $jsonSchema fails to parse in a $match stage within a view.
res = testDB.createView("invalid", coll.getName(), [{$match: {$jsonSchema: invalidSchema}}]);
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

// Test that an invalid $jsonSchema fails to parse in a listCollections command.
res = testDB.runCommand({listCollections: 1, filter: {$jsonSchema: invalidSchema}});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

// Test that a valid $jsonSchema is legal in a count command.
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: "str"}));
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.eq(1, coll.count({$jsonSchema: {properties: {a: {type: "number"}, b: {type: "string"}}}}));

// Test that a valid $jsonSchema is legal in a $geoNear stage.
const point = {
    type: "Point",
    coordinates: [31.0, 41.0]
};
assert.commandWorked(coll.insert({geo: point, a: 1}));
assert.commandWorked(coll.insert({geo: point, a: 0}));
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
res = coll.aggregate({
              $geoNear: {
                  near: [30, 40],
                  spherical: true,
                  query: {$jsonSchema: {properties: {a: {minimum: 1}}}},
                  distanceField: "dis",
                  includeLocs: "loc",
              }
          })
          .toArray();
assert.eq(1, res.length, tojson(res));
assert.eq(res[0].loc, point, tojson(res));

// Test that a valid $jsonSchema is legal in a distinct command.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: "str"}));
assert.commandWorked(coll.insert({a: ["STR", "str"]}));

assert(arrayEq([1, 2], coll.distinct("a", {$jsonSchema: {properties: {a: {type: "number"}}}})));

// Test that $jsonSchema in a query does not respect the collection-default collation.
let schema = {properties: {a: {enum: ["STR"]}}};
const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1
};
coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {collation: caseInsensitiveCollation}));
assert.commandWorked(coll.insert({a: "str"}));
assert.commandWorked(coll.insert({a: ["STR", "sTr"]}));
assert.eq(0, coll.find({$jsonSchema: schema}).itcount());
assert.eq(2, coll.find({$jsonSchema: {properties: {a: {uniqueItems: true}}}}).itcount());
assert.eq(2, coll.find({a: "STR"}).itcount());

// Test that $jsonSchema does not respect the collation set explicitly on a query.
coll.drop();
assert.commandWorked(coll.insert({a: "str"}));
assert.commandWorked(coll.insert({a: ["STR", "sTr"]}));

assert.eq(0, coll.find({$jsonSchema: schema}).collation(caseInsensitiveCollation).itcount());
assert.eq(2,
          coll.find({$jsonSchema: {properties: {a: {uniqueItems: true}}}})
              .collation(caseInsensitiveCollation)
              .itcount());
assert.eq(2, coll.find({a: "STR"}).collation(caseInsensitiveCollation).itcount());

// Test that $jsonSchema can be used in a $match stage within a view.
coll.drop();
let bulk = coll.initializeUnorderedBulkOp();
bulk.insert({name: "Peter", age: 65});
bulk.insert({name: "Paul", age: 105});
bulk.insert({name: "Mary", age: 10});
bulk.insert({name: "John", age: "unknown"});
bulk.insert({name: "Mark"});
bulk.insert({});
assert.commandWorked(bulk.execute());

assert.commandWorked(testDB.createView(
    "seniorCitizens", coll.getName(), [{
        $match: {
            $jsonSchema: {
                required: ["name", "age"],
                properties: {name: {type: "string"}, age: {type: "number", minimum: 65}}
            }
        }
    }]));
assert.eq(2, testDB.seniorCitizens.find().itcount());

// Test that $jsonSchema can be used in the listCollections filter.
res = testDB.runCommand(
    {listCollections: 1, filter: {$jsonSchema: {properties: {name: {enum: [coll.getName()]}}}}});
assert.commandWorked(res);
assert.eq(1, res.cursor.firstBatch.length);

// Test that $jsonSchema can be used in the listDatabases filter.
res = testDB.adminCommand(
    {listDatabases: 1, filter: {$jsonSchema: {properties: {name: {enum: [coll.getName()]}}}}});
assert.commandWorked(res);
assert.eq(1, res.databases.length);

// Test that $jsonSchema can be used in the filter of a $graphLookup stage.
const foreign = testDB.json_schema_foreign;
foreign.drop();
coll.drop();
for (let i = 0; i < 10; i++) {
    assert.commandWorked(foreign.insert({_id: i, n: [i - 1, i + 1]}));
}
assert.commandWorked(coll.insert({starting: 0}));

res = coll.aggregate({
                  $graphLookup: {
                      from: foreign.getName(),
                      startWith: "$starting",
                      connectFromField: "n",
                      connectToField: "_id",
                      as: "integers",
                      restrictSearchWithMatch: {$jsonSchema: {properties: {_id: {maximum: 4}}}}
                  }
              })
              .toArray();
assert.eq(1, res.length);
assert.eq(res[0].integers.length, 5);

// Test that $jsonSchema is legal in a delete command.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: "str"}));
assert.commandWorked(coll.insert({a: [3]}));

schema = {
    properties: {a: {type: "number", maximum: 2}}
};

res = coll.deleteMany({$jsonSchema: schema});
assert.eq(2, res.deletedCount);
assert.eq(0, coll.find({$jsonSchema: schema}).itcount());

// Test that $jsonSchema does not respect the collation specified in a delete command.
res = coll.deleteMany({$jsonSchema: {properties: {a: {enum: ["STR"]}}}},
                      {collation: caseInsensitiveCollation});
assert.eq(0, res.deletedCount);

// Test that $jsonSchema is legal in an update command.
coll.drop();
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));

res = coll.update({$jsonSchema: schema}, {$inc: {a: 1}}, {multi: true});
assert.commandWorked(res);
assert.eq(2, res.nMatched);
assert.eq(1, coll.find({$jsonSchema: schema}).itcount());

// Test that $jsonSchema is legal in a findAndModify command.
coll.drop();
assert.commandWorked(coll.insert({a: "long_string"}));
assert.commandWorked(coll.insert({a: "short"}));

schema = {
    properties: {a: {type: "string", minLength: 6}}
};
res = coll.findAndModify({query: {$jsonSchema: schema}, update: {$set: {a: "extra_long_string"}}});
assert.eq("long_string", res.a);
assert.eq(1, coll.find({$jsonSchema: schema}).itcount());

// Test that $jsonSchema works correctly in the presence of a basic b-tree index.
coll.drop();
assert.commandWorked(coll.insert({_id: 1, a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 2, a: 2, b: 2, point: [5, 5]}));
assert.commandWorked(coll.insert({_id: 3, a: "temp text test"}));

assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(3, coll.find({$jsonSchema: {}}).itcount());
assert.eq(2, coll.find({$jsonSchema: {properties: {a: {type: "number"}}}}).itcount());
assert.eq(2,
          coll.find({$jsonSchema: {required: ["a"], properties: {a: {type: "number"}}}}).itcount());
assert.eq(2, coll.find({$or: [{$jsonSchema: {properties: {a: {minimum: 2}}}}, {b: 2}]}).itcount());

// Test that $jsonSchema works correctly in the presence of a geo index.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({point: "2dsphere"}));
assert.eq(1, coll.find({$jsonSchema: {required: ["point"]}}).itcount());

assert.eq(1,
          coll.find({
                  $jsonSchema: {properties: {point: {minItems: 2}}},
                  point: {$geoNear: {$geometry: {type: "Point", coordinates: [5, 5]}}}
              })
              .itcount());

coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: 1, point: "2dsphere"}));
assert.eq(1, coll.find({$jsonSchema: {required: ["a", "point"]}}).itcount());

assert.eq(1,
          coll.find({
                  $jsonSchema: {required: ["a"], properties: {a: {minLength: 3}}},
                  point: {$geoNear: {$geometry: {type: "Point", coordinates: [5, 5]}}}
              })
              .itcount());

assert.eq(1,
          coll.find({
                  $and: [
                      {$jsonSchema: {properties: {point: {maxItems: 2}}}},
                      {point: {$geoNear: {$geometry: {type: "Point", coordinates: [5, 5]}}}, a: 2}
                  ]
              })
              .itcount());

// Test that $jsonSchema works correctly in the presence of a text index.
coll.dropIndexes();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(3, coll.find({$jsonSchema: {properties: {a: {minLength: 5}}}}).itcount());
assert.eq(1, coll.find({$jsonSchema: {required: ["a"]}, $text: {$search: "test"}}).itcount());
assert.eq(
    3, coll.find({$or: [{$jsonSchema: {required: ["a"]}}, {$text: {$search: "TEST"}}]}).itcount());
assert.eq(1, coll.find({$and: [{$jsonSchema: {}}, {$text: {$search: "TEST"}}]}).itcount());

if (!isMongos) {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, a: true}));

    // Test $jsonSchema in the precondition checking for applyOps.
    res = testDB.adminCommand({
        applyOps: [
            {op: "u", ns: coll.getFullName(), o2: {_id: 0}, o: {$v: 2, diff: {u: {a: false}}}},
        ],
        preCondition: [{
            ns: coll.getFullName(),
            q: {$jsonSchema: {properties: {a: {type: "boolean"}}}},
            res: {a: true}
        }]
    });
    assert.commandWorked(res);
    assert.eq(1, res.applied);

    // Use majority write concern to clear the drop-pending that can cause lock conflicts with
    // transactions.
    coll.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(coll.insert({_id: 1, a: true}));
}
}());
