// test dropping a db with simultaneous commits

m = db.getMongo();
baseName = "jstests_dur_droprace";
d = db.getSisterDB(baseName);
t = d.foo;

assert(d.adminCommand({setParameter: 1, syncdelay: 5}).ok);

var s = 0;

var start = new Date();

for (var pass = 0; pass < 100; pass++) {
    if (pass % 2 == 0) {
        // sometimes wait for create db first, to vary the timing of things
        var options = (pass % 4 == 0) ? {writeConcern: {fsync: true}} : undefined;
        t.insert({}, options);
    }
    t.insert({x: 1});
    t.insert({x: 3});
    t.ensureIndex({x: 1});
    sleep(s);
    if (pass % 13 == 0)
        t.drop();
    else if (pass % 17 == 0)
        t.dropIndexes();
    else
        d.dropDatabase();

    if (pass % 7 == 0)
        d.runCommand({getLastError: 1, j: 1});

    d.getLastError();
    s = (s + 1) % 25;
    // print(pass);
    if ((new Date()) - start > 60000) {
        print("stopping early");
        break;
    }
}
