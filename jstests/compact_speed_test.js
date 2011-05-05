if (1) {

    t = db.compactspeedtest;
    t.drop();

    var obj = { x: 1, y: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", z: [1, 2] };

    var start = new Date();
    function timed() {
        db.getLastError();
        var dt = (new Date()) - start;
        print("time: " + dt);
        start = new Date();
        return dt;
    }

    print("adding data");
    for (var i = 0; i < 100000; i++) {
        obj.x = i;
        obj.z[1] = i;
        t.insert(obj);
    }
    var a = timed();

    print("index");
    t.ensureIndex({ x: 1 });
    print("index");
    t.ensureIndex({ y: 1 });
    print("index");
    t.ensureIndex({ z: 1 });

    a += timed();

    print("count:" + t.count());

    timed();

    {
        print("compact");
        var res = db.runCommand({ compact: 'compactspeedtest', dev: true });
        b = timed();
        printjson(res);
        assert(res.ok);

        print("validate");
        var v = t.validate(true);

        assert(v.ok);
        assert(t.getIndexes().length == 4);

        if (b < a) {
            // consider making this fail/assert
            print("\n\n\nwarning WARNING compact command was slower than it should be");
            print("a:" + a + " b:" + b);
            print("\n\n\n");
        }
    }
}
