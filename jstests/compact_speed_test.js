if (1) {

    t = db.compactspeedtest;
    t.drop();

    var obj = { x: 1, y: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", z: [1, 2] };

    var start = new Date();
    function timed() {
        db.getLastError();
        print("time: " + (new Date() - start));
        start = new Date();
    }

    print("adding data");
    for (var i = 0; i < 100000; i++) {
        obj.x = i;
        obj.z[1] = i;
        t.insert(obj);
    }
    timed();

    print("index");
    t.ensureIndex({ x: 1 });
    print("index");
    t.ensureIndex({ y: 1 });
    print("index");
    t.ensureIndex({ z: 1 });

    timed();

    print("count:" + t.count());

    timed();

    if (1) {
        print("compact");
        var res = db.runCommand({ compact: 'compactspeedtest', dev: true });
        timed();
        printjson(res);
        assert(res.ok);

        print("validate");
        var v = t.validate(true);
        timed();

        assert(v.ok);
        assert(t.getIndexes().length == 4);
    }
}
