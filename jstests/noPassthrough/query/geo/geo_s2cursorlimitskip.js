/**
 * Test various cursor behaviors with 2dsphere index: getmore, limit, and skip.
 *
 * We insert a bunch of points and get a cursor. We then fetch some of the points and verify that
 * the number of query and getmore operations are correct via the profiler. Finally, we fetch the
 * rest of the points and again verify that the number of query and getmore operations are correct.
 *
 * @tags: [requires_profiling]
 */

import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const testDB = conn.getDB("geo_s2cursorlimitskip");
const t = testDB.geo_s2getmmm;
t.drop();
t.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded());

Random.setRandomSeed();
const random = Random.rand;

function sign() {
    return random() > 0.5 ? 1 : -1;
}

function insertRandomPoints(num, minDist, maxDist) {
    for (let i = 0; i < num; i++) {
        const lat = sign() * (minDist + random() * (maxDist - minDist));
        const lng = sign() * (minDist + random() * (maxDist - minDist));
        const point = {geo: {type: "Point", coordinates: [lng, lat]}};
        assert.commandWorked(t.insert(point));
    }
}

const totalPointCount = 200;
const initialAdvance = 10;
const batchSize = 4;

// Insert points between 0.01 and 1.0 away.
insertRandomPoints(totalPointCount, 0.01, 1.0);

let cursor = t
    .find({
        geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}},
    })
    .batchSize(batchSize);
assert.eq(cursor.count(), totalPointCount);

// Disable profiling in order to drop the system.profile collection.
// Then enable profiling for all operations.
testDB.setProfilingLevel(0);
testDB.system.profile.drop();
// Create 4MB system.profile collection to prevent the 'getmore' operations from overwriting the
// original query.
assert.commandWorked(testDB.createCollection("system.profile", {capped: true, size: 4 * 1024 * 1024}));
assert.commandWorked(testDB.setProfilingLevel(2));

for (let j = 0; j < initialAdvance; j++) {
    assert(cursor.hasNext());
    cursor.next();
}
// We looked at (totalPointCount - initialAdvance) points, there should still be more.
assert(cursor.hasNext());

// Cursor was advanced 10 times, batchSize=4 => 1 query + 2 getmore.
assert.eq(1, testDB.system.profile.count({op: "query", ns: t.getFullName()}));
assert.eq(2, testDB.system.profile.count({op: "getmore", ns: t.getFullName()}));

for (let k = initialAdvance; k < totalPointCount; k++) {
    assert(cursor.hasNext());
    cursor.next();
}

// Cursor was advanced 200 times, batchSize=4 => 1 query + 49 getmore.
assert.eq(1, testDB.system.profile.count({op: "query", ns: t.getFullName()}));
assert.eq(49, testDB.system.profile.count({op: "getmore", ns: t.getFullName()}));

// Disable profiling again - no longer needed for remainder of test.
testDB.setProfilingLevel(0);
testDB.system.profile.drop();

// Shouldn't be any more points to look at now.
assert(!cursor.hasNext());

const someLimit = 23;
// Make sure limit does something.
cursor = t
    .find({
        geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}},
    })
    .limit(someLimit);
// Count doesn't work here -- ignores limit/skip, so we use itcount.
assert.eq(cursor.itcount(), someLimit);

// Make sure skip works by skipping some stuff ourselves.
const someSkip = 3;
cursor = t
    .find({
        geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}},
    })
    .limit(someLimit + someSkip);
for (let i = 0; i < someSkip; ++i) {
    cursor.next();
}
let cursor2 = t
    .find({geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}}})
    .skip(someSkip)
    .limit(someLimit);
while (cursor.hasNext()) {
    assert(cursor2.hasNext());
    assert.eq(cursor.next(), cursor2.next());
}

MongoRunner.stopMongod(conn);
