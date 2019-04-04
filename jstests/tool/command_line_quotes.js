jsTest.log("Testing spaces in merizodump command-line options...");

var merizod = MerizoRunner.runMerizod();
var coll = merizod.getDB("spaces").coll;
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

var query = "{\"a\": {\"$gt\": 1} }";
assert(!MerizoRunner.runMerizoTool(
    "merizodump",
    {"host": "127.0.0.1:" + merizod.port, "db": "spaces", "collection": "coll", "query": query}));

MerizoRunner.stopMerizod(merizod);

jsTest.log("Test completed successfully");
