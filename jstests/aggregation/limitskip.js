
var coll = "numbers";

db[coll].drop();
for (i=0; i<100; i++) {
    db[coll].save({_id : i, mod : [i%2, i%3, i%5]});
}

print("-----LIMIT-----");

print("normal limit");
var doc = db.runCommand({ aggregate : coll, pipeline : [{ $limit : 2}]});
assert.eq(doc.result.length, 2, tojson(doc));

print("limit larger than result size");
doc = db.runCommand({ aggregate : coll, pipeline : [{ $limit : 200}]});
assert.eq(doc.result.length, 100, tojson(doc));


print("limit on sort");
doc = db.runCommand({ aggregate : coll, pipeline : [{$sort : {_id : -1}}, {$limit : 3}]});
r = doc.result;
assert.eq(doc.result.length, 3);
for (var i=0; i<r; i++) {
    assert.eq(100 - r[i]._id, i, tojson(doc));
}

print("TODO: invalid limit"); // once assert has been replaced with uassert


print("-----SKIP------");

print("normal skip");
doc = db.runCommand({ aggregate : coll, pipeline : [{ $skip : 95}]});
assert.eq(doc.result.length, 5, tojson(doc));

print("skip larger than result size");
doc = db.runCommand({ aggregate : coll, pipeline : [{ $skip : 102}]});
assert.eq(doc.result.length, 0, tojson(doc));


print("check skip results");
doc = db.runCommand({ aggregate : coll, pipeline : [{ $sort : {_id : 1}}, {$skip : 6}, {$limit : 3}]});
assert.eq(doc.result.length, 3, tojson(doc));
for (var i=0; i<3; i++) {
    assert.eq(i+6, doc.result[i]._id, tojson(doc));
}

print("TODO: invalid skip"); // once assert has been replaced with uassert


print("on virtual collection");
doc = db.runCommand({ aggregate : coll, pipeline : [
    {
	$unwind : "$mod"
    },
    {
        $project : { m : "$mod" }
    },
    {
        $sort : {
            m : 1,
            _id : -1
        }
    },
    {
        $skip : 150
    },
    {
        $limit : 5
    }
]});

assert.eq(doc.result.length, 5);
for (var i=0; i<5; i++) {
    assert.eq(1, doc.result[i].m, tojson(doc));
}
assert.eq(doc.result[0]._id, 55, tojson(doc));
assert.eq(doc.result[1]._id, 53, tojson(doc));
assert.eq(doc.result[2]._id, 52, tojson(doc));
assert.eq(doc.result[3]._id, 51, tojson(doc));
assert.eq(doc.result[4]._id, 51, tojson(doc));


print("size 0 collection");
db[coll].drop();

doc = db.runCommand({ aggregate : coll, pipeline : [{$skip : 6}]});
assert.eq(doc.ok, 1);
assert.eq(doc.result.length, 0);

doc = db.runCommand({ aggregate : coll, pipeline : [{$limit : 3}]});
assert.eq(doc.ok, 1);
assert.eq(doc.result.length, 0);

