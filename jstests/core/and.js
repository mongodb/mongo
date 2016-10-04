// Some tests for $and SERVER-1089

t = db.jstests_and;
t.drop();

t.save({a: [1, 2]});
t.save({a: 'foo'});

function check() {
    // $and must be an array
    assert.throws(function() {
        t.find({$and: 4}).toArray();
    });
    // $and array must not be empty
    assert.throws(function() {
        t.find({$and: []}).toArray();
    });
    // $and elements must be objects
    assert.throws(function() {
        t.find({$and: [4]}).toArray();
    });

    // Check equality matching
    assert.eq(1, t.count({$and: [{a: 1}]}));
    assert.eq(1, t.count({$and: [{a: 1}, {a: 2}]}));
    assert.eq(0, t.count({$and: [{a: 1}, {a: 3}]}));
    assert.eq(0, t.count({$and: [{a: 1}, {a: 2}, {a: 3}]}));
    assert.eq(1, t.count({$and: [{a: 'foo'}]}));
    assert.eq(0, t.count({$and: [{a: 'foo'}, {a: 'g'}]}));

    // Check $and with other fields
    assert.eq(1, t.count({a: 2, $and: [{a: 1}]}));
    assert.eq(0, t.count({a: 0, $and: [{a: 1}]}));
    assert.eq(0, t.count({a: 2, $and: [{a: 0}]}));
    assert.eq(1, t.count({a: 1, $and: [{a: 1}]}));

    // Check recursive $and
    assert.eq(1, t.count({a: 2, $and: [{$and: [{a: 1}]}]}));
    assert.eq(0, t.count({a: 0, $and: [{$and: [{a: 1}]}]}));
    assert.eq(0, t.count({a: 2, $and: [{$and: [{a: 0}]}]}));
    assert.eq(1, t.count({a: 1, $and: [{$and: [{a: 1}]}]}));

    assert.eq(1, t.count({$and: [{a: 2}, {$and: [{a: 1}]}]}));
    assert.eq(0, t.count({$and: [{a: 0}, {$and: [{a: 1}]}]}));
    assert.eq(0, t.count({$and: [{a: 2}, {$and: [{a: 0}]}]}));
    assert.eq(1, t.count({$and: [{a: 1}, {$and: [{a: 1}]}]}));

    // Some of these cases were more important with an alternative $and syntax
    // that was rejected, but they're still valid checks.

    // Check simple regex
    assert.eq(1, t.count({$and: [{a: /foo/}]}));
    // Check multiple regexes
    assert.eq(1, t.count({$and: [{a: /foo/}, {a: /^f/}, {a: /o/}]}));
    assert.eq(0, t.count({$and: [{a: /foo/}, {a: /^g/}]}));
    assert.eq(1, t.count({$and: [{a: /^f/}, {a: 'foo'}]}));
    // Check regex flags
    assert.eq(0, t.count({$and: [{a: /^F/}, {a: 'foo'}]}));
    assert.eq(1, t.count({$and: [{a: /^F/i}, {a: 'foo'}]}));

    // Check operator
    assert.eq(1, t.count({$and: [{a: {$gt: 0}}]}));

    // Check where
    assert.eq(1, t.count({a: 'foo', $where: 'this.a=="foo"'}));
    assert.eq(1, t.count({$and: [{a: 'foo'}], $where: 'this.a=="foo"'}));
    assert.eq(1, t.count({$and: [{a: 'foo'}], $where: 'this.a=="foo"'}));

    // Nested where ok
    assert.eq(1, t.count({$and: [{$where: 'this.a=="foo"'}]}));
    assert.eq(1, t.count({$and: [{a: 'foo'}, {$where: 'this.a=="foo"'}]}));
    assert.eq(1, t.count({$and: [{$where: 'this.a=="foo"'}], $where: 'this.a=="foo"'}));
}

check();
t.ensureIndex({a: 1});
check();

assert.eq(1, t.find({a: 1, $and: [{a: 2}]}).itcount());
assert.eq(1, t.find({$and: [{a: 1}, {a: 2}]}).itcount());
