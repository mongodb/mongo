// SERVER-9137 test that the httpinterface parameter enables the web interface
port = allocatePorts(1)[0];
httpPort = port + 1000;
var conn = MongoRunner.runMongod({port: port, smallfiles: ""});

tryHttp = function() {
    try {
        var mongo = new Mongo('localhost:'+httpPort) ;
    }
    catch (e) {
        return false;
    }
    // if we managed to start and connect a new mongo then the web interface is working
    return true;
}

assert.throws(function() {assert.soon(tryHttp, "tryHttp failed, like we expected it to")});

MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({ port:port, smallfiles: "", httpinterface: ""});
assert.soon(tryHttp,
            "the web interface should be running on " + httpPort);

MongoRunner.stopMongod(conn);
