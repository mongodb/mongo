// @tags: [
//     # Uses $where operator
//     requires_scripting
// ]

var t = db.jstests_ora;

// $where
t.drop();
for (var i = 0; i < 10; i += 1) {
    t.save({x: i, y: 10 - i});
}
assert.eq(t.find({$or: [{$where: 'this.x === 2'}]}).count(), 1);
assert.eq(t.find({$or: [{$where: 'this.x === 2'}, {$where: 'this.y === 2'}]}).count(), 2);
assert.eq(t.find({$or: [{$where: 'this.x === 2'}, {$where: 'this.y === 8'}]}).count(), 1);
assert.eq(t.find({$or: [{$where: 'this.x === 2'}, {x: {$ne: 2}}]}).count(), 10);

// geo
t.drop();
t.createIndex({loc: "2d"});

assert.throws(function() {
    t.find({$or: [{loc: {$near: [11, 11]}}]}).limit(1).next();
});
