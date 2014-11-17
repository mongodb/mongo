(function() {

load("jstests/libs/mongostat.js");

port = allocatePorts(1);

baseName = "stat_header";

m = startMongod("--port", port[0], "--dbpath", MongoRunner.dataPath + baseName + port[0], "--nohttpinterface", "--bind_ip", "127.0.0.1");

clearRawMongoProgramOutput();

x = runMongoProgram("mongostat", "--port", port[0], "--rowcount", 1);

match = rawMongoProgramOutput().split("\n").some(function(i) {
    return i.match(/^sh\d+\| insert/);
});

assert.eq(true, match, "normally a header appears");

clearRawMongoProgramOutput();

x = runMongoProgram("mongostat", "--port", port[0], "--rowcount", 1, "--noheaders");

match = rawMongoProgramOutput().split("\n").some(function(i) {
    return i.match(/^sh.....\| insert/);
});

assert.eq(false, match, "--noheaders suppress the header");

}());
