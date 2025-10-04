// Test various cursor behaviors
//
// @tags: [
//   # The test runs commands that are not allowed with security token: profile, setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # This test attempts to enable profiling on a server and then get profiling data by reading
//   # nodes the "system.profile" collection. The former operation must be routed to the primary in
//   # a replica set, whereas the latter may be routed to a secondary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   requires_capped,
//   requires_getmore,
//   requires_profiling,
//   # The test does not expect concurrent reads against its test collections (e.g. the checks
//   # aren't expecting concurrent reads but initial sync will be reading those collections).
//   does_not_support_concurrent_reads
// ]

let testDB = db.getSiblingDB("geo_s2cursorlimitskip");
let t = testDB.geo_s2getmmm;
t.drop();
t.createIndex({geo: "2dsphere"});

Random.setRandomSeed();
let random = Random.rand;

/*
 * To test that getmore is working within 2dsphere index.
 * We insert a bunch of points and get a cursor. We then
 * fetch some of the points and verify that the number of
 * query and getmore operations are correct. Finally, we
 * fetch the rest of the points and again verify that the
 * number of query and getmore operations are correct.
 */
function sign() {
    return random() > 0.5 ? 1 : -1;
}
function insertRandomPoints(num, minDist, maxDist) {
    for (let i = 0; i < num; i++) {
        let lat = sign() * (minDist + random() * (maxDist - minDist));
        let lng = sign() * (minDist + random() * (maxDist - minDist));
        let point = {geo: {type: "Point", coordinates: [lng, lat]}};
        assert.commandWorked(t.insert(point));
    }
}

let totalPointCount = 200;
let initialAdvance = 10;
let batchSize = 4;

// Insert points between 0.01 and 1.0 away.
insertRandomPoints(totalPointCount, 0.01, 1.0);

let cursor = t
    .find({
        geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}},
    })
    .batchSize(batchSize);
assert.eq(cursor.count(), totalPointCount);

// Disable profiling in order to drop the system.profile collection.
// Then enable profiling for all operations. This is acceptable because
// our test is denylisted from the parallel suite.
testDB.setProfilingLevel(0);
testDB.system.profile.drop();
// Create 4MB system.profile collection to prevent the 'getmore' operations from overwriting the
// original query.
assert.commandWorked(testDB.createCollection("system.profile", {capped: true, size: 4 * 1024 * 1024}));
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

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
    let tmpPoint = cursor.next();
}

// Cursor was advanced 200 times, batchSize=4 => 1 query + 49 getmore.
assert.eq(1, testDB.system.profile.count({op: "query", ns: t.getFullName()}));
assert.eq(49, testDB.system.profile.count({op: "getmore", ns: t.getFullName()}));

// Disable profiling again - no longer needed for remainder of test
testDB.setProfilingLevel(0);
testDB.system.profile.drop();

// Shouldn't be any more points to look at now.
assert(!cursor.hasNext());

let someLimit = 23;
// Make sure limit does something.
cursor = t
    .find({
        geo: {$geoNear: {$geometry: {type: "Point", coordinates: [0.0, 0.0]}}},
    })
    .limit(someLimit);
// Count doesn't work here -- ignores limit/skip, so we use itcount.
assert.eq(cursor.itcount(), someLimit);
// Make sure skip works by skipping some stuff ourselves.
let someSkip = 3;
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
