var baseName = "jstests_dbNoCreate";

var m = BongoRunner.runBongod({});

var t = m.getDB(baseName).t;

assert.eq(0, t.find().toArray().length);
t.remove({});
t.update({}, {a: 1});
t.drop();

BongoRunner.stopBongod(m);

m = BongoRunner.runBongod({restart: true, cleanData: false, dbpath: m.dbpath});
assert.eq(
    -1, m.getDBNames().indexOf(baseName), "found " + baseName + " in " + tojson(m.getDBNames()));
