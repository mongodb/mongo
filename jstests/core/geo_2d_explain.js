var t = db.geo_2d_explain;

t.drop();

var n = 1000;

// insert n documents with integer _id, a can be 1-5, loc is close to [40, 40]
t.drop()
t.ensureIndex({loc: "2d", _id: 1})

var x = 40;
var y = 40;
for (var i = 0; i < n; i++) { 
    // random number in range [1, 5]
    var a = Math.floor(Math.random() * 5) + 1;
    var dist = 4.0;
    var dx = (Math.random() - 0.5) * dist;
    var dy = (Math.random() - 0.5) * dist;
    var loc = [x + dx, y + dy];
    t.save({_id: i, a: a, loc: loc}); 
}

var explain = t.find({loc: {$near: [40, 40]}, _id: {$lt: 50}}).explain();

print('explain = ' + tojson(explain));

assert.eq(explain.n, explain.nscannedObjects);
assert.lte(explain.n, explain.nscanned);
