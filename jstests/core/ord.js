// check that we don't crash if an index used by an earlier or clause is dropped

// Dropping an index kills all cursors on the indexed namespace, not just those
// cursors using the dropped index.  This test is to serve as a reminder that
// the $or implementation may need minor adjustments (memory ownership) if this
// behavior is changed.

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

const t = db.jstests_ord;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (let i = 0; i < 80; ++i) {
    t.save({a: 1});
}

for (let i = 0; i < 100; ++i) {
    t.save({b: 1});
}

const c = t.find({$or: [{a: 1}, {b: 1}]}).batchSize(100);
for (let i = 0; i < 100; ++i) {
    c.next();
}
// At this point, our initial query has ended and there is a client cursor waiting
// to read additional documents from index {b:1}.  Deduping is performed against
// the index key {a:1}.

t.dropIndex({a: 1});

// Dropping an index kills all cursors on the indexed namespace, not just those
// cursors using the dropped index.
if (FixtureHelpers.isMongos(db)) {
    // mongos may have some data left from a previous batch stored in memory, so it might not
    // return an error immediately, but it should eventually.
    assert.soon(function() {
        try {
            c.next();
            return false;  // We didn't throw an error yet.
        } catch (e) {
            return true;
        }
    });
} else {
    assert.throws(() => c.next());
}
})();
