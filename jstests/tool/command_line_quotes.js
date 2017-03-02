jsTest.log("Testing spaces in bongodump command-line options...");

var bongod = BongoRunner.runBongod();
var coll = bongod.getDB("spaces").coll;
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

var query = "{\"a\": {\"$gt\": 1} }";
assert(!BongoRunner.runBongoTool(
    "bongodump",
    {"host": "127.0.0.1:" + bongod.port, "db": "spaces", "collection": "coll", "query": query}));

BongoRunner.stopBongod(bongod);

jsTest.log("Test completed successfully");
