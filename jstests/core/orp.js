// $or clause deduping with result set sizes > 101 (smaller result sets are now also deduped by the
// query optimizer cursor).

t = db.jstests_orp;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.ensureIndex({c: 1});

for (i = 0; i < 200; ++i) {
    t.save({a: 1, b: 1});
}

// Deduping results from the previous clause.
assert.eq(200, t.count({$or: [{a: 1}, {b: 1}]}));

// Deduping results from a prior clause.
assert.eq(200, t.count({$or: [{a: 1}, {c: 1}, {b: 1}]}));
t.save({c: 1});
assert.eq(201, t.count({$or: [{a: 1}, {c: 1}, {b: 1}]}));

// Deduping results that would normally be index only matches on overlapping and double scanned $or
// field regions.
t.drop();
t.ensureIndex({a: 1, b: 1});
for (i = 0; i < 16; ++i) {
    for (j = 0; j < 16; ++j) {
        t.save({a: i, b: j});
    }
}
assert.eq(16 * 16, t.count({$or: [{a: {$gte: 0}, b: {$gte: 0}}, {a: {$lte: 16}, b: {$lte: 16}}]}));

// Deduping results from a clause that completed before the multi cursor takeover.
t.drop();
t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.save({a: 1, b: 200});
for (i = 0; i < 200; ++i) {
    t.save({b: i});
}
assert.eq(201, t.count({$or: [{a: 1}, {b: {$gte: 0}}]}));
