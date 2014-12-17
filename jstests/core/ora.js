var t = db.jstests_ora;

// $where
t.drop();
for (var i = 0; i < 10; i += 1) {
    t.save({x: i, y: 10 - i});
}
assert.eq.automsg("1", "t.find({$or: [{$where: 'this.x === 2'}]}).count()");
assert.eq.automsg("2", "t.find({$or: [{$where: 'this.x === 2'}, {$where: 'this.y === 2'}]}).count()");
assert.eq.automsg("1", "t.find({$or: [{$where: 'this.x === 2'}, {$where: 'this.y === 8'}]}).count()");
assert.eq.automsg("10", "t.find({$or: [{$where: 'this.x === 2'}, {x: {$ne: 2}}]}).count()");

// geo
t.drop();
t.ensureIndex({loc: "2d"});

assert.throws(function () {t.find({$or: [{loc: {$near: [11, 11]}}]}).limit(1).next()['_id'];});
