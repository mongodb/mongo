var baseName = "jstests_dbNoCreate";

var m = MerizoRunner.runMerizod({});

var t = m.getDB(baseName).t;

assert.eq(0, t.find().toArray().length);
t.remove({});
t.update({}, {a: 1});
t.drop();

MerizoRunner.stopMerizod(m);

m = MerizoRunner.runMerizod({restart: true, cleanData: false, dbpath: m.dbpath});
assert.eq(
    -1, m.getDBNames().indexOf(baseName), "found " + baseName + " in " + tojson(m.getDBNames()));
MerizoRunner.stopMerizod(m);