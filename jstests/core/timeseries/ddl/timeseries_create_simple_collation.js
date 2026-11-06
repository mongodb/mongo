/**
 * Tests that creating a time-series collection with the simple collation succeeds and does not
 * crash the server (SERVER-138665).
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {timeField: "t"},
        collation: {locale: "simple"},
    }),
);

assert.commandWorked(coll.insert({t: new Date(), m: 1}));
assert.eq(1, coll.find().itcount());
coll.drop();
