/**
 * Part 1: Simple test of TTL.  Create a new collection with 24 docs, with timestamps at one hour
 *         intervals, from now-minus-23 hours ago until now.  Also add some docs with non-date
 *         values.  Then create a TTL index that expires all docs older than a string. Wait 70
 *         seconds (TTL monitor runs every 60) and check that no documents were deleted.
 * Part 2: Add a second TTL index that expires all docs older than ~5.5 hours (20000
 *         seconds).  Wait 70 seconds and check that 18 docs deleted.
 * Part 3: Add a third TTL index on an identical field. The second index expires docs older than
 *         ~2.8 hours (10000 seconds). Wait 70 seconds and check that 3 more docs deleted.
 */

// Part 1
let t = db.ttl1;
t.drop();
t.runCommand("create", {flags: 0});
let now = new Date().getTime();

for (let i = 0; i < 24; i++) {
    let past = new Date(now - 3600 * 1000 * i);
    t.insert({x: past, y: past, z: past});
}
t.insert({a: 1}); // no x value
t.insert({x: null}); // non-date value
t.insert({x: true}); // non-date value
t.insert({x: "yo"}); // non-date value
t.insert({x: 3}); // non-date value
t.insert({x: /foo/}); // non-date value

assert.eq(30, t.count());

sleep(70 * 1000);

assert.eq(t.count(), 30);

// Part 2
t.createIndex({x: 1}, {expireAfterSeconds: 20000});

assert.soon(
    function () {
        return t.count() < 30;
    },
    "TTL index on x didn't delete",
    70 * 1000,
);

// We know the TTL thread has started deleting. Wait a few seconds to give it a chance to finish.
assert.soon(
    function () {
        return t.find({x: {$lt: new Date(now - 20000 * 1000)}}).count() === 0;
    },
    "TTL index on x didn't finish deleting",
    5 * 1000,
);
assert.eq(12, t.count());

assert.lte(18, db.serverStatus().metrics.ttl.deletedDocuments);
assert.lte(1, db.serverStatus().metrics.ttl.passes);

// Part 3
t.createIndex({y: 1}, {expireAfterSeconds: 10000});

assert.soon(
    function () {
        return t.count() < 12;
    },
    "TTL index on y didn't delete",
    70 * 1000,
);

assert.soon(
    function () {
        return t.find({y: {$lt: new Date(now - 10000 * 1000)}}).count() === 0;
    },
    "TTL index on y didn't finish deleting",
    5 * 1000,
);
assert.eq(9, t.count());
