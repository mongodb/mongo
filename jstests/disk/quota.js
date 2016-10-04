// Check functioning of --quotaFiles parameter, including with respect to SERVER-3293 ('local'
// database).

baseName = "jstests_disk_quota";

var m = MongoRunner.runMongod({quotaFiles: 2, smallfiles: ""});
db = m.getDB(baseName);

big = new Array(10000).toString();

// Insert documents until quota is exhausted.
var coll = db[baseName];
var res = coll.insert({b: big});
while (!res.hasWriteError()) {
    res = coll.insert({b: big});
}

dotTwoDataFile = baseName + ".2";
files = listFiles(m.dbpath);
for (i in files) {
    // Since only one data file is allowed, a .0 file is expected and a .1 file may be preallocated
    // (SERVER-3410) but no .2 file is expected.
    assert.neq(dotTwoDataFile, files[i].baseName);
}

dotTwoDataFile = "local" + ".2";
// Check that quota does not apply to local db, and a .2 file can be created.
l = m.getDB("local")[baseName];
for (i = 0; i < 10000; ++i) {
    assert.writeOK(l.insert({b: big}));
    dotTwoFound = false;
    if (i % 100 != 0) {
        continue;
    }
    files = listFiles(m.dbpath);
    for (f in files) {
        if (files[f].baseName == dotTwoDataFile) {
            dotTwoFound = true;
        }
    }
    if (dotTwoFound) {
        break;
    }
}

assert(dotTwoFound);
