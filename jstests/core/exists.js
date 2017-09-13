t = db.jstests_exists;
t.drop();

t.save({});
t.save({a: 1});
t.save({a: {b: 1}});
t.save({a: {b: {c: 1}}});
t.save({a: {b: {c: {d: null}}}});

function dotest(n) {
    assert.eq(5, t.count(), n);
    assert.eq(1, t.count({a: null}), n);
    assert.eq(2, t.count({'a.b': null}), n);
    assert.eq(3, t.count({'a.b.c': null}), n);
    assert.eq(5, t.count({'a.b.c.d': null}), n);

    assert.eq(5, t.count(), n);
    assert.eq(4, t.count({a: {$ne: null}}), n);
    assert.eq(3, t.count({'a.b': {$ne: null}}), n);
    assert.eq(2, t.count({'a.b.c': {$ne: null}}), n);
    assert.eq(0, t.count({'a.b.c.d': {$ne: null}}), n);

    assert.eq(4, t.count({a: {$exists: true}}), n);
    assert.eq(3, t.count({'a.b': {$exists: true}}), n);
    assert.eq(2, t.count({'a.b.c': {$exists: true}}), n);
    assert.eq(1, t.count({'a.b.c.d': {$exists: true}}), n);

    assert.eq(1, t.count({a: {$exists: false}}), n);
    assert.eq(2, t.count({'a.b': {$exists: false}}), n);
    assert.eq(3, t.count({'a.b.c': {$exists: false}}), n);
    assert.eq(4, t.count({'a.b.c.d': {$exists: false}}), n);
}

dotest("before index");
t.ensureIndex({"a": 1});
t.ensureIndex({"a.b": 1});
t.ensureIndex({"a.b.c": 1});
t.ensureIndex({"a.b.c.d": 1});
dotest("after index");
assert.eq(1, t.find({a: {$exists: false}}).hint({a: 1}).itcount());

t.drop();

t.save({r: [{s: 1}]});
assert(t.findOne({'r.s': {$exists: true}}));
assert(!t.findOne({'r.s': {$exists: false}}));
assert(!t.findOne({'r.t': {$exists: true}}));
assert(t.findOne({'r.t': {$exists: false}}));
