
// Test that non boolean value types are allowed with $explain spec. SERVER-2322

t = db.jstests_exists7;
t.drop();

function testIntegerExistsSpec() {
    t.remove({});
    t.save({});
    t.save({a: 1});
    t.save({a: 2});
    t.save({a: 3, b: 3});
    t.save({a: 4, b: 4});

    assert.eq(2, t.count({b: {$exists: 1}}));
    assert.eq(3, t.count({b: {$exists: 0}}));
}

testIntegerExistsSpec();
t.ensureIndex({b: 1});
testIntegerExistsSpec();
