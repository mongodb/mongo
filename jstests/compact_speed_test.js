if (0) {

    t = db.compactspeedtest;
    t.drop();

    var obj = { x: 1, y: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", z: [1, 2] };

    print("adding data");
    for (var i = 0; i < 100000; i++) {
        obj.x = i;
        obj.z[1] = i;
        t.insert(obj);
    }

    print("index");
    t.ensureIndex({ x: 1 });
    print("index");
    t.ensureIndex({ y: 1 });
    print("index");
    t.ensureIndex({ z: 1 });

    print("count:" + t.count());

    print("compact");
    var res = db.runCommand({ compact: 'compactspeedtest', dev: true });
    printjson(res);
    assert(res.ok);

    print("validate");
    var v = t.validate(true);
    assert(v.ok);
    assert(t.getIndexes().length == 4);

}
