
t = db.repair_cursor1;
t.drop();

t.insert({x: 1});
t.insert({x: 2});

res = t.runCommand("repairCursor");
assert(res.ok, tojson(res));

t2 = db.repair_cursor1a;
t2.drop();

cursor = new DBCommandCursor(db._mongo, res);
cursor.forEach(function(z) {
    t2.insert(z);
});
assert.eq(t.find().itcount(), t2.find().itcount());
assert.eq(t.hashAllDocs(), t2.hashAllDocs());
