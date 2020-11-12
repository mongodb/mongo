
t = db.perf.index1;
t.drop();

for (var i = 0; i < 100000; i++) {
    t.save({x: i});
}

t.findOne();

printjson(db.serverStatus().mem);

for (var i = 0; i < 5; i++) {
    nonu = Date.timeFunc(function() {
        t.ensureIndex({x: 1});
    });
    t.dropIndex({x: 1});
    u = Date.timeFunc(function() {
        t.ensureIndex({x: 1}, {unique: 1});
    });
    t.dropIndex({x: 1});
    print("non unique: " + nonu + "   unique: " + u);
    printjson(db.serverStatus().mem);
}
