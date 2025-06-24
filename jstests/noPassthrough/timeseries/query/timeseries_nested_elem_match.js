/**
 * Test the behavior of a nested $elemMatch in a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const collName = jsTestName();
const coll = db[jsTestName()];

assert(coll.drop());
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));
assert.commandWorked(coll.insert([{
    "_id": 0,
    "t": ISODate("1970-01-01T00:00:00Z"),
    "a": [[0], 0],
}]));

// This nested $elemMatch checks for an 'a' that both has a subarray that contains a 0 and that the
// subarray is equal to 0.
let results = coll.aggregate([
                      {$match: {a: {$elemMatch: {$elemMatch: {$eq: 0}, $eq: 0}}}},
                      {"$project": {"b": "$a"}},
                  ])
                  .toArray();
assert.eq(results, []);

// This netsted $elemMatch checks if 'a' contains [0].
results = coll.aggregate([
                  {$match: {a: {$elemMatch: {$elemMatch: {$eq: 0}, $eq: [0]}}}},
                  {"$project": {"b": "$a"}},
              ])
              .toArray();
assert.eq(results, [{_id: 0, b: [[0], 0]}]);

// This nested $elemMatch checks if 'a' contains [0] and 0.
results = coll.aggregate([
                  {"$match": {"a": {"$elemMatch": {"$elemMatch": {"$eq": 0}}, "$eq": 0}}},
                  {"$project": {"b": "$a"}},
              ])
              .toArray();
assert.eq(results, [{_id: 0, b: [[0], 0]}]);
MongoRunner.stopMongod(conn);
