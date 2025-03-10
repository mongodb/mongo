/**
 * Tests that the validate command checks the min max timestamps in the control field respect the
 * bucket max span.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const collName = jsTestName();
const bucketName = `system.buckets.${collName}`;
const currentDate = ISODate('2025-01-23T02:00:00.000Z');
const outOfRangeDate = ISODate('2025-01-23T03:00:00.000Z');  // currentDate + 3600s

// Inserts one measurement and verifies the collection is valid.
const coll = db.getCollection(collName);
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "time", metaField: "tag", granularity: "seconds"}}));
assert.commandWorked(coll.insert({time: currentDate, tag: 1, a: 1}));
let res = coll.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.errors.length, 0);

// Sets the max timestamp to outside the bucket max span.
const bucketColl = db.getCollection(bucketName);
bucketColl.updateOne({meta: 1}, {$set: {"control.max.time": outOfRangeDate}});
res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
