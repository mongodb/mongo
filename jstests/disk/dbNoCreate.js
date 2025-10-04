let baseName = "jstests_dbNoCreate";

let m = MongoRunner.runMongod({});

let t = m.getDB(baseName).t;

assert.eq(0, t.find().toArray().length);
t.remove({});
t.update({}, {a: 1});
t.drop();

MongoRunner.stopMongod(m);

m = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: m.dbpath});
assert.eq(-1, m.getDBNames().indexOf(baseName), "found " + baseName + " in " + tojson(m.getDBNames()));
MongoRunner.stopMongod(m);
