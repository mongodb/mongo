var bongo = BongoRunner.runBongod({smallfiles: "", storageEngine: "devnull"});

db = bongo.getDB("test");

res = db.foo.insert({x: 1});
assert.eq(1, res.nInserted, tojson(res));

BongoRunner.stopBongod(bongo);
